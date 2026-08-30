/* cdk_wasm_vm — wasm3 の出し入れ。1 回の `wasm run` の一生ぶん。
 *
 * このヘッダと port_wasm_vm.c は **ESP-IDF に依存しない**。
 * ホストの ctest が同じものをリンクして、実機と同じ経路で .wasm を回す。
 * FreeRTOS のタスクや PSRAM のアリーナは cdk_wasm_run.h 側の仕事。
 */
#ifndef CDK_WASM_VM_H
#define CDK_WASM_VM_H

#include "cdk_wasm.h"
#include "wasm3.h"

#ifdef __cplusplus
extern "C" {
#endif

/* wasm3 のオペランドスタック。ゲストのネイティブスタックとは別物。 */
#define CDK_WASM_STACK_BYTES (16 * 1024)

/* 走らせた結果。 */
typedef enum {
    CDK_WASM_DONE = 0,   /* cdk_main が返った */
    CDK_WASM_EXITED,     /* sys_exit で抜けた */
    CDK_WASM_STOPPED,    /* Ctrl-C */
    CDK_WASM_TRAPPED,    /* 境界外アクセスなど、ゲストの落ち方 */
    CDK_WASM_BADMODULE   /* 読めない / cdk_main が無い / リンクできない */
} cdk_wasm_result_t;

/* 1 本走らせる。parse → load → link → cdk_main → 後片づけ、まで。
 *
 * **wasm のバイト列は呼び出しの間ずっと生きていること。**
 * wasm3 は m3_ParseModule でバイト列を写さず、そこを指したまま実行する。
 *
 * ret には cdk_main の戻り値（または sys_exit のコード）を書く。
 * err には失敗したときの説明を NUL 終端で書く（成功時は空文字列）。
 *
 * ctx->stop はいつでも別のタスクから立ててよい。native の入口で見ている。
 */
cdk_wasm_result_t cdk_wasm_vm_run(cdk_wasm_ctx_t *ctx,
                                  const void *wasm, int wasm_len,
                                  int *ret, char *err, int err_cap);

/* port_wasm_natives.c 側。ABI の表をモジュールに結びつける。 */
M3Result cdk_wasm_link_natives(IM3Module mod);
int      cdk_wasm_native_count(void);

#ifdef __cplusplus
}
#endif

#endif /* CDK_WASM_VM_H */
