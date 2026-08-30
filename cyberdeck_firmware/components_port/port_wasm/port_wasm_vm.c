/* wasm3 の出し入れ。ESP-IDF に依存しない（ホストの ctest が同じものを回す）。
 *
 * 毎回 env と runtime を作って壊す。**トラップしたあとにもう一度走れること**を
 * host/tests/test_wasm_run.c の最初のケースで確かめている。ここが崩れると
 * Ctrl-C で止めたあとに 2 本目が走らない。
 */
#include "cdk_wasm_vm.h"

#include <string.h>

static void set_err(char *err, int cap, const char *s)
{
    if (!err || cap <= 0) return;
    if (!s) { err[0] = '\0'; return; }
    int n = (int)strlen(s);
    if (n > cap - 1) n = cap - 1;
    memcpy(err, s, (size_t)n);
    err[n] = '\0';
}

cdk_wasm_result_t cdk_wasm_vm_run(cdk_wasm_ctx_t *ctx,
                                  const void *wasm, int wasm_len,
                                  int *ret, char *err, int err_cap)
{
    set_err(err, err_cap, NULL);
    if (ret) *ret = 0;

    if (!ctx || !wasm || wasm_len <= 0) {
        set_err(err, err_cap, "モジュールがありません");
        return CDK_WASM_BADMODULE;
    }

    IM3Environment env = m3_NewEnvironment();
    if (!env) {
        set_err(err, err_cap, "wasm3 の環境を作れません");
        return CDK_WASM_BADMODULE;
    }

    /* ctx を runtime のユーザーデータに載せる。native は m3_GetUserData で拾う。 */
    IM3Runtime rt = m3_NewRuntime(env, CDK_WASM_STACK_BYTES, ctx);
    if (!rt) {
        m3_FreeEnvironment(env);
        set_err(err, err_cap, "wasm3 のランタイムを作れません");
        return CDK_WASM_BADMODULE;
    }

    cdk_wasm_result_t result = CDK_WASM_BADMODULE;
    IM3Module         mod    = NULL;

    /* wasm3 はここでバイト列を写さない。呼び出しの間 wasm は生きていること。 */
    M3Result r = m3_ParseModule(env, &mod, (const uint8_t *)wasm, (uint32_t)wasm_len);
    if (r) { set_err(err, err_cap, r); goto done; }

    r = m3_LoadModule(rt, mod);
    if (r) {
        /* load に失敗した時点ではまだ runtime が引き取っていない。自分で返す。
         * 成功したあとは runtime のものになるので、二重に free しない。 */
        m3_FreeModule(mod);
        mod = NULL;
        set_err(err, err_cap, r);
        goto done;
    }

    r = cdk_wasm_link_natives(mod);
    if (r) { set_err(err, err_cap, r); goto done; }

    IM3Function fn = NULL;
    r = m3_FindFunction(&fn, rt, CDK_WASM_ENTRY);
    if (r) {
        set_err(err, err_cap, CDK_WASM_ENTRY " が見つかりません");
        goto done;
    }

    r = m3_CallV(fn, (uint32_t)ctx->argc);
    if (r) {
        /* sys_exit も Ctrl-C もトラップで抜けてくる。どれだったかは ctx で分かる。 */
        if (ctx->exited) {
            if (ret) *ret = ctx->exit_code;
            result = CDK_WASM_EXITED;
        } else if (ctx->stop) {
            result = CDK_WASM_STOPPED;
        } else {
            set_err(err, err_cap, r);
            result = CDK_WASM_TRAPPED;
        }
        goto done;
    }

    {
        int32_t v = 0;
        m3_GetResultsV(fn, &v);
        if (ret) *ret = (int)v;
    }
    result = CDK_WASM_DONE;

done:
    /* アプリが開けっぱなしにしたファイル・ソケット・I2C を閉じる。
     * ランタイムを壊す前にやる（バックエンドはまだ生きている）。 */
    cdk_wasm_ctx_cleanup(ctx);

    m3_FreeRuntime(rt);      /* load 済みのモジュールもここで返る */
    m3_FreeEnvironment(env);
    return result;
}
