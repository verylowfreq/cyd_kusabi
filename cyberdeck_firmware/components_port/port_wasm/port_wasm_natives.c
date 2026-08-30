/* wasm3 の native シム。**線形メモリの境界検査はここだけでやる。**
 *
 * このファイルは ESP-IDF に依存しない（wasm3.h と cdk_wasm.h だけ）。
 * ホストの ctest がこのファイルをそのままリンクして回すので、
 * 実機で動くのと同じ境界検査コードが検証される。
 *
 * ## 規則（ctest wasm_natives_checked が機械的に見ている）
 *
 * ひとつの native 関数の中で、`m3ApiGetArgMem` の数と `m3ApiCheckMem` の数を
 * **必ず一致させる**。「近くに 1 個あればよい」にすると、バッファを 2 本取る
 * i2c_wrrd や sock_connect や con_size で片方だけ検査していても気づけない。
 *
 * 長さ引数を持たない出力ポインタは `m3ApiCheckMem(p, 4)` と明示する。
 *
 * なお m3ApiCheckMem は len を uint64_t に広げて比べるので、
 * 負の長さ（ゲストは i32 で渡してくる）は巨大な値になって必ずトラップする。
 */
#include "cdk_wasm.h"
#include "wasm3.h"

/* Ctrl-C。native の入口で見て、立っていたらトラップで巻き戻す。
 * native を一度も呼ばないループ（while(1);）は止められない。DESIGN.md §15。 */
static const char *const k_trap_stopped = "[trap] Ctrl-C で止めました";
static const char *const k_trap_exit    = "[trap] sys_exit";
static const char *const k_trap_noctx   = "[trap] 文脈がありません";

/* 文脈を取り出し、停止要求を見る。すべての native の先頭に置く。
 *
 * m3ApiRawFunction は _ctx / _mem を必ず引数に取るが、使わない native もある。
 * -Werror=unused-parameter に引っかかるので、ここでまとめて消しておく。 */
#define CDK_ENTER()                                                       \
    (void)_ctx;                                                           \
    (void)_mem;                                                           \
    cdk_wasm_ctx_t *ctx = (cdk_wasm_ctx_t *)m3_GetUserData(runtime);      \
    if (!ctx) m3ApiTrap(k_trap_noctx);                                    \
    if (ctx->stop) m3ApiTrap(k_trap_stopped)

/* ---- sys ------------------------------------------------------------- */

static m3ApiRawFunction(n_sys_exit)
{
    CDK_ENTER();
    m3ApiGetArg(int32_t, code);
    ctx->exit_code = code;
    ctx->exited    = true;
    m3ApiTrap(k_trap_exit);
}

static m3ApiRawFunction(n_sys_now_ms)
{
    m3ApiReturnType(int64_t);
    CDK_ENTER();
    m3ApiReturn((int64_t)cdk_wasm_sys_now_ms(ctx));
}

static m3ApiRawFunction(n_sys_delay)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArg(int32_t, ms);
    const int r = cdk_wasm_sys_delay(ctx, ms);
    /* 待っている間に Ctrl-C が来たら、戻り値ではなくトラップで抜ける。
     * 戻り値だとアプリが無視して回り続けられてしまう。 */
    if (r == CDK_E_ABORT) m3ApiTrap(k_trap_stopped);
    m3ApiReturn((int32_t)r);
}

static m3ApiRawFunction(n_sys_argc)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiReturn((int32_t)cdk_wasm_sys_argc(ctx));
}

static m3ApiRawFunction(n_sys_arg)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArg(int32_t, idx);
    m3ApiGetArgMem(char *, out);
    m3ApiGetArg(int32_t, cap);
    m3ApiCheckMem(out, cap);
    m3ApiReturn((int32_t)cdk_wasm_sys_arg(ctx, idx, out, cap));
}

static m3ApiRawFunction(n_sys_random)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArgMem(void *, out);
    m3ApiGetArg(int32_t, len);
    m3ApiCheckMem(out, len);
    m3ApiReturn((int32_t)cdk_wasm_sys_random(ctx, out, len));
}

/* ---- con ------------------------------------------------------------- */

static m3ApiRawFunction(n_con_write)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArgMem(const void *, buf);
    m3ApiGetArg(int32_t, len);
    m3ApiCheckMem(buf, len);
    const int r = cdk_wasm_con_write(ctx, buf, len);
    /* リングが空くのを待っている間に Ctrl-C が来たら、戻り値ではなく
     * トラップで抜ける。sys_delay と揃える（戻り値だと無視して回り続けられる）。 */
    if (r == CDK_E_ABORT) m3ApiTrap(k_trap_stopped);
    m3ApiReturn((int32_t)r);
}

static m3ApiRawFunction(n_con_read)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArgMem(void *, buf);
    m3ApiGetArg(int32_t, len);
    m3ApiCheckMem(buf, len);
    m3ApiReturn((int32_t)cdk_wasm_con_read(ctx, buf, len));
}

static m3ApiRawFunction(n_con_size)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArgMem(void *, pcols);
    m3ApiGetArgMem(void *, prows);
    m3ApiCheckMem(pcols, 4);
    m3ApiCheckMem(prows, 4);
    int32_t cols = 0, rows = 0;
    const int r = cdk_wasm_con_size(ctx, &cols, &rows);
    /* wasm は常にリトルエンディアン。ホストが違っても正しく置く。 */
    m3ApiWriteMem32(pcols, (uint32_t)cols);
    m3ApiWriteMem32(prows, (uint32_t)rows);
    m3ApiReturn((int32_t)r);
}

/* ---- fs -------------------------------------------------------------- */

static m3ApiRawFunction(n_fs_open)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArgMem(const char *, path);
    m3ApiGetArg(int32_t, plen);
    m3ApiGetArg(int32_t, mode);
    m3ApiCheckMem(path, plen);
    m3ApiReturn((int32_t)cdk_wasm_fs_open(ctx, path, plen, mode));
}

static m3ApiRawFunction(n_fs_read)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArg(int32_t, fd);
    m3ApiGetArgMem(void *, buf);
    m3ApiGetArg(int32_t, len);
    m3ApiCheckMem(buf, len);
    m3ApiReturn((int32_t)cdk_wasm_fs_read(ctx, fd, buf, len));
}

static m3ApiRawFunction(n_fs_write)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArg(int32_t, fd);
    m3ApiGetArgMem(const void *, buf);
    m3ApiGetArg(int32_t, len);
    m3ApiCheckMem(buf, len);
    m3ApiReturn((int32_t)cdk_wasm_fs_write(ctx, fd, buf, len));
}

static m3ApiRawFunction(n_fs_seek)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArg(int32_t, fd);
    m3ApiGetArg(int32_t, off);
    m3ApiGetArg(int32_t, whence);
    m3ApiReturn((int32_t)cdk_wasm_fs_seek(ctx, fd, off, whence));
}

static m3ApiRawFunction(n_fs_close)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArg(int32_t, fd);
    m3ApiReturn((int32_t)cdk_wasm_fs_close(ctx, fd));
}

static m3ApiRawFunction(n_fs_size)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArgMem(const char *, path);
    m3ApiGetArg(int32_t, plen);
    m3ApiCheckMem(path, plen);
    m3ApiReturn((int32_t)cdk_wasm_fs_size(ctx, path, plen));
}

static m3ApiRawFunction(n_fs_unlink)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArgMem(const char *, path);
    m3ApiGetArg(int32_t, plen);
    m3ApiCheckMem(path, plen);
    m3ApiReturn((int32_t)cdk_wasm_fs_unlink(ctx, path, plen));
}

static m3ApiRawFunction(n_fs_mkdir)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArgMem(const char *, path);
    m3ApiGetArg(int32_t, plen);
    m3ApiCheckMem(path, plen);
    m3ApiReturn((int32_t)cdk_wasm_fs_mkdir(ctx, path, plen));
}

static m3ApiRawFunction(n_fs_opendir)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArgMem(const char *, path);
    m3ApiGetArg(int32_t, plen);
    m3ApiCheckMem(path, plen);
    m3ApiReturn((int32_t)cdk_wasm_fs_opendir(ctx, path, plen));
}

static m3ApiRawFunction(n_fs_readdir)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArg(int32_t, dh);
    m3ApiGetArgMem(char *, out);
    m3ApiGetArg(int32_t, cap);
    m3ApiCheckMem(out, cap);
    m3ApiReturn((int32_t)cdk_wasm_fs_readdir(ctx, dh, out, cap));
}

static m3ApiRawFunction(n_fs_closedir)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArg(int32_t, dh);
    m3ApiReturn((int32_t)cdk_wasm_fs_closedir(ctx, dh));
}

/* ---- gpio / adc ------------------------------------------------------ */

static m3ApiRawFunction(n_gpio_mode)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArg(int32_t, pin);
    m3ApiGetArg(int32_t, mode);
    m3ApiReturn((int32_t)cdk_wasm_gpio_mode(ctx, pin, mode));
}

static m3ApiRawFunction(n_gpio_set)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArg(int32_t, pin);
    m3ApiGetArg(int32_t, level);
    m3ApiReturn((int32_t)cdk_wasm_gpio_set(ctx, pin, level));
}

static m3ApiRawFunction(n_gpio_get)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArg(int32_t, pin);
    m3ApiReturn((int32_t)cdk_wasm_gpio_get(ctx, pin));
}

static m3ApiRawFunction(n_adc_read)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArg(int32_t, ch);
    m3ApiReturn((int32_t)cdk_wasm_adc_read(ctx, ch));
}

/* ---- i2c ------------------------------------------------------------- */

static m3ApiRawFunction(n_i2c_open)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArg(int32_t, sda);
    m3ApiGetArg(int32_t, scl);
    m3ApiGetArg(int32_t, hz);
    m3ApiReturn((int32_t)cdk_wasm_i2c_open(ctx, sda, scl, hz));
}

static m3ApiRawFunction(n_i2c_write)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArg(int32_t, addr);
    m3ApiGetArgMem(const void *, buf);
    m3ApiGetArg(int32_t, len);
    m3ApiCheckMem(buf, len);
    m3ApiReturn((int32_t)cdk_wasm_i2c_write(ctx, addr, buf, len));
}

static m3ApiRawFunction(n_i2c_read)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArg(int32_t, addr);
    m3ApiGetArgMem(void *, buf);
    m3ApiGetArg(int32_t, len);
    m3ApiCheckMem(buf, len);
    m3ApiReturn((int32_t)cdk_wasm_i2c_read(ctx, addr, buf, len));
}

static m3ApiRawFunction(n_i2c_wrrd)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArg(int32_t, addr);
    m3ApiGetArgMem(const void *, wbuf);
    m3ApiGetArg(int32_t, wlen);
    m3ApiGetArgMem(void *, rbuf);
    m3ApiGetArg(int32_t, rlen);
    m3ApiCheckMem(wbuf, wlen);
    m3ApiCheckMem(rbuf, rlen);
    m3ApiReturn((int32_t)cdk_wasm_i2c_wrrd(ctx, addr, wbuf, wlen, rbuf, rlen));
}

static m3ApiRawFunction(n_i2c_close)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiReturn((int32_t)cdk_wasm_i2c_close(ctx));
}

/* ---- uart ------------------------------------------------------------ */

static m3ApiRawFunction(n_uart_open)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArg(int32_t, tx);
    m3ApiGetArg(int32_t, rx);
    m3ApiGetArg(int32_t, baud);
    m3ApiReturn((int32_t)cdk_wasm_uart_open(ctx, tx, rx, baud));
}

static m3ApiRawFunction(n_uart_write)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArg(int32_t, h);
    m3ApiGetArgMem(const void *, buf);
    m3ApiGetArg(int32_t, len);
    m3ApiCheckMem(buf, len);
    m3ApiReturn((int32_t)cdk_wasm_uart_write(ctx, h, buf, len));
}

static m3ApiRawFunction(n_uart_read)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArg(int32_t, h);
    m3ApiGetArgMem(void *, buf);
    m3ApiGetArg(int32_t, len);
    m3ApiCheckMem(buf, len);
    m3ApiReturn((int32_t)cdk_wasm_uart_read(ctx, h, buf, len));
}

static m3ApiRawFunction(n_uart_close)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArg(int32_t, h);
    m3ApiReturn((int32_t)cdk_wasm_uart_close(ctx, h));
}

/* ---- net ------------------------------------------------------------- */

static m3ApiRawFunction(n_net_up)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiReturn((int32_t)cdk_wasm_net_up(ctx));
}

static m3ApiRawFunction(n_sock_open)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArg(int32_t, type);
    m3ApiReturn((int32_t)cdk_wasm_sock_open(ctx, type));
}

static m3ApiRawFunction(n_sock_connect)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArg(int32_t, sd);
    m3ApiGetArgMem(const char *, host);
    m3ApiGetArg(int32_t, hlen);
    m3ApiGetArg(int32_t, port);
    m3ApiCheckMem(host, hlen);
    m3ApiReturn((int32_t)cdk_wasm_sock_connect(ctx, sd, host, hlen, port));
}

static m3ApiRawFunction(n_sock_send)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArg(int32_t, sd);
    m3ApiGetArgMem(const void *, buf);
    m3ApiGetArg(int32_t, len);
    m3ApiCheckMem(buf, len);
    m3ApiReturn((int32_t)cdk_wasm_sock_send(ctx, sd, buf, len));
}

static m3ApiRawFunction(n_sock_recv)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArg(int32_t, sd);
    m3ApiGetArgMem(void *, buf);
    m3ApiGetArg(int32_t, len);
    m3ApiCheckMem(buf, len);
    m3ApiReturn((int32_t)cdk_wasm_sock_recv(ctx, sd, buf, len));
}

static m3ApiRawFunction(n_sock_close)
{
    m3ApiReturnType(int32_t);
    CDK_ENTER();
    m3ApiGetArg(int32_t, sd);
    m3ApiReturn((int32_t)cdk_wasm_sock_close(ctx, sd));
}

/* ---- gfx（予約。まだ実装しない） -------------------------------------
 *
 * 名前と署名だけ先に決めておく。呼ぶと CDK_E_NOSYS が返る。
 * 引数は読まない（wasm3 は native が _sp を消費しなくても構わない）。
 *
 * キャンバスはメインループが専有していて、所有権の受け渡しを設計しないと
 * 開放できない。それは M8 の範囲外（DESIGN.md §15「やらないこと」）。
 */
#define CDK_GFX_STUB(name)                     \
    static m3ApiRawFunction(n_##name)          \
    {                                          \
        m3ApiReturnType(int32_t);              \
        CDK_ENTER();                           \
        (void)ctx;                             \
        m3ApiReturn((int32_t)CDK_E_NOSYS);     \
    }

CDK_GFX_STUB(gfx_size)
CDK_GFX_STUB(gfx_clear)
CDK_GFX_STUB(gfx_fill_rect)
CDK_GFX_STUB(gfx_blit)
CDK_GFX_STUB(gfx_text)
CDK_GFX_STUB(gfx_present)

/* ---- 表 -------------------------------------------------------------- */

typedef struct {
    const char *name;
    const char *sig;
    M3RawCall   fn;
} cdk_native_t;

static const cdk_native_t k_natives[] = {
    {"sys_exit",      "v(i)",      &n_sys_exit},
    {"sys_now_ms",    "I()",       &n_sys_now_ms},
    {"sys_delay",     "i(i)",      &n_sys_delay},
    {"sys_argc",      "i()",       &n_sys_argc},
    {"sys_arg",       "i(i*i)",    &n_sys_arg},
    {"sys_random",    "i(*i)",     &n_sys_random},

    {"con_write",     "i(*i)",     &n_con_write},
    {"con_read",      "i(*i)",     &n_con_read},
    {"con_size",      "i(**)",     &n_con_size},

    {"fs_open",       "i(*ii)",    &n_fs_open},
    {"fs_read",       "i(i*i)",    &n_fs_read},
    {"fs_write",      "i(i*i)",    &n_fs_write},
    {"fs_seek",       "i(iii)",    &n_fs_seek},
    {"fs_close",      "i(i)",      &n_fs_close},
    {"fs_size",       "i(*i)",     &n_fs_size},
    {"fs_unlink",     "i(*i)",     &n_fs_unlink},
    {"fs_mkdir",      "i(*i)",     &n_fs_mkdir},
    {"fs_opendir",    "i(*i)",     &n_fs_opendir},
    {"fs_readdir",    "i(i*i)",    &n_fs_readdir},
    {"fs_closedir",   "i(i)",      &n_fs_closedir},

    {"gpio_mode",     "i(ii)",     &n_gpio_mode},
    {"gpio_set",      "i(ii)",     &n_gpio_set},
    {"gpio_get",      "i(i)",      &n_gpio_get},
    {"adc_read",      "i(i)",      &n_adc_read},

    {"i2c_open",      "i(iii)",    &n_i2c_open},
    {"i2c_write",     "i(i*i)",    &n_i2c_write},
    {"i2c_read",      "i(i*i)",    &n_i2c_read},
    {"i2c_wrrd",      "i(i*i*i)",  &n_i2c_wrrd},
    {"i2c_close",     "i()",       &n_i2c_close},

    {"uart_open",     "i(iii)",    &n_uart_open},
    {"uart_write",    "i(i*i)",    &n_uart_write},
    {"uart_read",     "i(i*i)",    &n_uart_read},
    {"uart_close",    "i(i)",      &n_uart_close},

    {"net_up",        "i()",       &n_net_up},
    {"sock_open",     "i(i)",      &n_sock_open},
    {"sock_connect",  "i(i*ii)",   &n_sock_connect},
    {"sock_send",     "i(i*i)",    &n_sock_send},
    {"sock_recv",     "i(i*i)",    &n_sock_recv},
    {"sock_close",    "i(i)",      &n_sock_close},

    /* 予約。CDK_E_NOSYS を返す。 */
    {"gfx_size",      "i(**)",     &n_gfx_size},
    {"gfx_clear",     "i(i)",      &n_gfx_clear},
    {"gfx_fill_rect", "i(iiiii)",  &n_gfx_fill_rect},
    {"gfx_blit",      "i(iiii*i)", &n_gfx_blit},
    {"gfx_text",      "i(ii*iii)", &n_gfx_text},
    {"gfx_present",   "i()",       &n_gfx_present},
};

M3Result cdk_wasm_link_natives(IM3Module mod)
{
    for (size_t i = 0; i < sizeof k_natives / sizeof k_natives[0]; i++) {
        const M3Result r = m3_LinkRawFunction(mod, CDK_WASM_IMPORT_MODULE,
                                              k_natives[i].name, k_natives[i].sig,
                                              k_natives[i].fn);
        /* モジュールが使っていないインポートは lookup failed になる。
         * 使う関数だけ import するのが普通なので、これは失敗ではない。 */
        if (r && r != m3Err_functionLookupFailed) return r;
    }
    return m3Err_none;
}

int cdk_wasm_native_count(void)
{
    return (int)(sizeof k_natives / sizeof k_natives[0]);
}
