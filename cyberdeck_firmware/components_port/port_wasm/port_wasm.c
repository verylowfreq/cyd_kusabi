/* ユーザーアプリを走らせるタスク。
 *
 * ## なぜ別タスクなのか
 *
 * この機械はシェルのコマンドをメインループの中で同期実行する
 * （cdk_shell_key → route_key → メインループ）。走っている間は
 * cdk_link_poll も cdk_usbhid_poll も画面フラッシュも回らない。
 * ユーザーアプリは何秒でも回りうるので、そこに置くと画面が固まり、
 * Ctrl-C を受け取る手段も無くなる。
 *
 * ## タスクは CPU0 に固定する
 *
 * メインタスクと同じ CPU に載せる。cdk_wasm のリングは単一生産者・
 * 単一消費者で、volatile な head/tail しか持っていない。同じ CPU なら
 * それで足りる。別 CPU に散らすとバリアの議論が要る。
 *
 * ## 触ってよいもの
 *
 * wasm タスクが触るのはリングとバックエンド（port_wasm_backend.c）だけ。
 * 端末・画面・SKK はメインループの持ち物のままにする。
 * SD は cdk_sd_lock が再帰ミューテックスなので別タスクからでも安全。
 */
#include "cdk_wasm_run.h"

#include <stdio.h>
#include <string.h>

#include "cdk_wasm.h"
#include "cdk_wasm_vm.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

extern const cdk_wasm_host_t *cdk_wasm_backend(void); /* port_wasm_backend.c */

static const char *TAG = "wasm";

/* 起動時に 1 回だけ確保する。DESIGN.md §14。 */
static uint8_t *s_module;
static int      s_module_len;

static cdk_wasm_ctx_t s_ctx;
static TaskHandle_t   s_task;

/* メインループが読み、タスクが書く。どちらも 1 語なので volatile で足りる。 */
static volatile bool s_busy;
static volatile bool s_finished;

/* 結果。タスクが書き終えてから s_finished を立てるので、
 * メインループは s_finished を見てから読めばよい。 */
static cdk_wasm_result_t s_result;
static int               s_ret;
static char              s_err[128];

esp_err_t cdk_wasm_run_init(void)
{
    if (s_module) return ESP_OK;

    s_module = heap_caps_malloc(CDK_WASM_MODULE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_module) {
        ESP_LOGE(TAG, "モジュール置き場 %d バイトが PSRAM から取れません",
                 CDK_WASM_MODULE_MAX);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "モジュール置き場 %d バイトを PSRAM に確保", CDK_WASM_MODULE_MAX);
    return ESP_OK;
}

bool cdk_wasm_run_busy(void) { return s_busy; }

void cdk_wasm_run_stop(void)
{
    if (s_busy) cdk_wasm_request_stop(&s_ctx);
}

bool cdk_wasm_stop_requested(void)
{
    return s_busy && s_ctx.stop != 0;
}

void cdk_wasm_run_feed(const void *data, size_t len)
{
    if (!s_busy || !data || len == 0) return;
    (void)cdk_wasm_push_in(&s_ctx, data, (int)len);
}

/* ---- タスク ---------------------------------------------------------- */

static void wasm_task(void *arg)
{
    (void)arg;

    s_result = cdk_wasm_vm_run(&s_ctx, s_module, s_module_len, &s_ret,
                               s_err, (int)sizeof s_err);

    /* スタックがどれだけ要ったか。SSH と同じ手当て（main.c の高水位ログ）。
     * ここが 2KB を切るようなら CDK_WASM_TASK_STACK を上げる。 */
    ESP_LOGI(TAG, "スタック残り %u バイト",
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));

    /* **s_finished は最後に立てる。** 先に立てると、メインループが
     * 書き終わっていない s_result / s_err を読む。 */
    s_finished = true;
    s_task = NULL;
    /* WithCaps で作ったタスクは WithCaps で消す。vTaskDelete だと
     * PSRAM から取ったスタックが返らない。 */
    vTaskDeleteWithCaps(NULL);
}

/* ---- 開始 ------------------------------------------------------------ */

/* SD から .wasm を読む。cdk_wasm のサンドボックスを通してから開く。 */
static esp_err_t load_module(const char *path, char *why, size_t why_cap)
{
    char abs[CDK_WASM_PATH_MAX];
    const int r = cdk_wasm_path_resolve(path, (int)strlen(path), abs, (int)sizeof abs);
    if (r != CDK_E_OK) {
        snprintf(why, why_cap, "%s は開けません（microSD の外です）", path);
        return ESP_ERR_INVALID_ARG;
    }

    const cdk_wasm_host_t *h = cdk_wasm_backend();
    void *f = h->file_open(h->user, abs, CDK_OPEN_READ);
    if (!f) {
        snprintf(why, why_cap, "%s がありません", abs);
        return ESP_ERR_NOT_FOUND;
    }

    s_module_len = 0;
    for (;;) {
        const int room = CDK_WASM_MODULE_MAX - s_module_len;
        if (room <= 0) break;
        const int n = h->file_read(h->user, f, s_module + s_module_len, room);
        if (n <= 0) break;
        s_module_len += n;
    }
    /* 読み切れたか。上限ちょうどで止まったなら、まだ続きがあるかもしれない。 */
    uint8_t probe;
    const int more = h->file_read(h->user, f, &probe, 1);
    h->file_close(h->user, f);

    if (more > 0) {
        snprintf(why, why_cap, "大きすぎます（上限 %d バイト）", CDK_WASM_MODULE_MAX);
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_module_len <= 0) {
        snprintf(why, why_cap, "中身がありません");
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t cdk_wasm_run_start(const char *path, int argc, char **argv,
                             int cols, int rows, char *why, size_t why_cap)
{
    if (why && why_cap) why[0] = '\0';

    if (!s_module) {
        snprintf(why, why_cap, "wasm の置き場が用意できていません");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_busy) {
        snprintf(why, why_cap, "すでに動いています");
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t err = load_module(path, why, why_cap);
    if (err != ESP_OK) return err;

    cdk_wasm_ctx_init(&s_ctx, cdk_wasm_backend());
    cdk_wasm_set_size(&s_ctx, cols, rows);

    /* **引数はここで写す。** cdk_shell_split の argv は execute() の
     * スタックを指していて、コマンドが return した時点で無効になる。
     * アプリは別タスクで後から sys_arg を読むので、写さないと化ける。 */
    if (!cdk_wasm_set_args(&s_ctx, argc, argv)) {
        snprintf(why, why_cap, "引数が多すぎるか長すぎます");
        return ESP_ERR_INVALID_ARG;
    }

    s_finished = false;
    s_busy     = true;
    s_err[0]   = '\0';
    s_ret      = 0;
    s_result   = CDK_WASM_DONE;

    /* メインタスクと同じ CPU0 に固定する（リングの前提）。
     * 優先度はメインタスク (1) と同じにして、どちらかが飢えないようにする。
     * スタックは PSRAM から取る（cdk_wasm_run.h に理由）。 */
    if (xTaskCreatePinnedToCoreWithCaps(wasm_task, "wasm", CDK_WASM_TASK_STACK, NULL,
                                        1, &s_task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_busy = false;
        snprintf(why, why_cap, "タスクを作れません（内部メモリ不足）");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ---- メインループから ------------------------------------------------ */

int cdk_wasm_run_poll(char *out, int cap, bool *done, char *report, size_t report_cap)
{
    if (done) *done = false;
    if (!s_busy) return 0;

    /* 出力は終わったあとも汲み切る。最後の 1 行を落とさない。 */
    const int n = cdk_wasm_drain_out(&s_ctx, out, cap);

    if (s_finished && n == 0) {
        s_busy = false;
        if (done) *done = true;
        if (report && report_cap) {
            switch (s_result) {
            case CDK_WASM_DONE:
                snprintf(report, report_cap, "\r\nwasm  : 終了 (%d)\r\n", s_ret);
                break;
            case CDK_WASM_EXITED:
                snprintf(report, report_cap, "\r\nwasm  : 終了 (%d)\r\n", s_ret);
                break;
            case CDK_WASM_STOPPED:
                snprintf(report, report_cap,
                         "\r\n\033[33mwasm  : 止めました\033[0m\r\n");
                break;
            case CDK_WASM_TRAPPED:
                snprintf(report, report_cap,
                         "\r\n\033[31mwasm  : 落ちました: %s\033[0m\r\n", s_err);
                break;
            case CDK_WASM_BADMODULE:
            default:
                snprintf(report, report_cap,
                         "\r\n\033[31mwasm  : 読めません: %s\033[0m\r\n", s_err);
                break;
            }
        }
        if (s_ctx.dropped > 0) {
            ESP_LOGW(TAG, "出力を %d バイト捨てました", s_ctx.dropped);
        }
    }
    return n;
}
