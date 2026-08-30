/* cdk_wasm_run — ユーザーアプリを実機で走らせる。
 *
 * **アプリはメインループでは走らせない。専用の FreeRTOS タスクで走らせる。**
 * この機械はコマンドをメインループの中で同期実行するので、そこで回すと
 * 画面もキーも制御チャネルも止まり、Ctrl-C すら届かなくなる。
 *
 * 同時に走るのは 1 本だけ。走っている間はシェルを止める（edit / ssh と同じ）。
 * そうすることで、ロックを持たない port_periph / port_cfg をシェルと
 * アプリが同時に叩く事故が起きない。
 */
#ifndef CDK_WASM_RUN_H
#define CDK_WASM_RUN_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 受け入れる .wasm の大きさ。起動時に PSRAM から 1 回だけ確保する
 * （DESIGN.md §14「動的確保は起動時のみ」）。tools/wasmcc.py の
 * MODULE_MAX と対にしてある。 */
#define CDK_WASM_MODULE_MAX (256 * 1024)

/* wasm タスクのネイティブスタック。**PSRAM から取る。**
 *
 * 実機で測ったところ、内部 DRAM は 77KB 空いていても最大ブロックが
 * 31KB しかなく、32KB の連続領域が取れなかった（「タスクを作れません」）。
 * 内部 DRAM はこの機械でいちばん足りない資源なので、そこを削るのではなく
 * キャンバスや辞書と同じく PSRAM へ置く。
 *
 * PSRAM スタックのタスクは**フラッシュのキャッシュが無効な間は走れない。**
 * wasm タスクが触るのは SD (SPI) と GPIO だけで、NVS への書き込みは
 * シェル側（メインタスク）でしか起きないので条件を満たす。
 *
 * wasm3 側の d_m3MaxNativeStack をこれより小さくしてあるので、
 * 暴走した再帰はスタックを壊す前に wasm3 がトラップする。 */
#define CDK_WASM_TASK_STACK 65536

/* 起動時に 1 回。PSRAM のアリーナを確保する。 */
esp_err_t cdk_wasm_run_init(void);

/* SD 上の .wasm を読んで走らせる。**すぐ返る**（タスクが走り出すだけ）。
 * path は cdk_wasm のサンドボックス（/sdcard の下）を通る。
 * 走り出せたら ESP_OK。失敗したら why に理由を書く。 */
esp_err_t cdk_wasm_run_start(const char *path, int argc, char **argv,
                             int cols, int rows, char *why, size_t why_cap);

/* 走っているか。 */
bool cdk_wasm_run_busy(void);

/* Ctrl-C。止めてほしいと伝えるだけで、ここでは待たない。 */
void cdk_wasm_run_stop(void);

/* 止めてほしいと言われているか。バックエンドの待ち（sys_delay や
 * ソケットの受信）が、刻みながらこれを見て早く戻るために使う。 */
bool cdk_wasm_stop_requested(void);

/* メインループが毎周呼ぶ。
 *
 *  - アプリの出した文字を out へ汲み出す（戻り値がバイト数）
 *  - キーは cdk_wasm_run_feed で押し込む
 *
 * アプリが終わっていたら *done を true にし、report に 1 行の結果を書く。
 * done が立った周で呼び出し側が enter_shell() する。 */
int cdk_wasm_run_poll(char *out, int cap, bool *done, char *report, size_t report_cap);

/* 端末から来たバイト列をアプリへ。入りきらなければ捨てる。 */
void cdk_wasm_run_feed(const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* CDK_WASM_RUN_H */
