/* port_wget — HTTP / HTTPS でファイルを 1 本取ってくる
 *
 * ## 作り（scp と同じ形）
 *
 * **繋ぐところはブロッキング、中身はメインループが刻む。専用タスクは
 * 作らない。** 名前解決・TCP・TLS ハンドシェイク・リダイレクト・ヘッダ受信は
 * `esp_http_client_open` / `fetch_headers` の中で刻めないので、
 * `cdk_wget_start()` で待つ（scp の認証待ちと同じ扱い。この間 Ctrl-C は
 * 効かない）。
 *
 * ヘッダが取れたら読みの期限を `CDK_WGET_READ_TIMEOUT_MS` に落とす。
 * `esp_http_client_read` は**呼ぶたびに** `client->timeout_ms` で select し、
 * 来ていなければ `-ESP_ERR_HTTP_EAGAIN` を返す（IDF v5.5.5 で読んだ）。
 * 期限は transport の read 1 回ごとにかかるので、1 回に頼む量を受信
 * バッファと揃えて待ちを 1 回に抑える（下の WGET_READ）。状態は全部
 * メインループのスレッドだけが触る。排他は要らない。
 *
 * ## TLS
 *
 * 証明書は常にバンドルで検証する（`-k` は置かない）。mbedtls の確保先は
 * PSRAM（sdkconfig.defaults の EXTERNAL_MEM_ALLOC）なので、SSH と並んでも
 * 内部 DRAM を食い合わない。
 *
 * ## 踏みやすい所
 *
 *  - **長さの分からない応答（Content-Length も chunked も無い）は読めない。**
 *    esp_http_client は `data_process < content_length(-1)` で残りを判断する
 *    ので、read が即 0 を返す。黙って 0 バイトのファイルを置かないよう断る。
 *  - **リダイレクトは中身を読み捨ててから開き直す**（esp_https_ota と同じ手順）。
 *    読み捨てないと次の応答の頭に前の本文が混ざる。
 *  - **`cdk_sd_lock()` は 1 回の書き込みごとに取る。** CoreS3 は LCD の DC と
 *    SD の MISO が同じ G35 なので、握っている間は画面が出せない。
 *  - **FATFS の rename は上書きしないので、先に消す。**
 */
#include "cdk_net.h"

#include "cdk_storage.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "cdk_wget";

/* 読み捨て・受け取りの領域。PSRAM から起動時に 1 回だけ取る。 */
#define WGET_CHUNK 4096

/* **1 回の read で頼む量。esp_http_client の受信バッファ（既定 512）と揃える。**
 *
 * `esp_http_client_read` は頼んだ量が埋まるまで、受信バッファ単位で
 * transport の read を繰り返す。**期限はその 1 回ごとにかかる**ので、
 * 4KB を頼むと 1 呼び出しで最大 8 回（80ms）待つ。512 なら待つのは 1 回。
 * 受信バッファを 4KB に広げる手は取らない（esp_http_client は内部 DRAM から
 * 取るので、この機械でいちばん足りない資源を食う）。 */
#define WGET_READ 512

/* 1 周で刻む時間の上限 (ms)。**呼び出しの途中では割り込めない**ので、
 * 呼び出しの間で時計を見る。 */
#define WGET_SLICE_MS 30

/* 1 周で読む回数の上限。実際の上限はほぼ WGET_SLICE_MS が決める。 */
#define WGET_READS_PER_POLL 32

static uint8_t *s_buf;

static bool                     s_busy;
static bool                     s_abort;
static cdk_wget_phase_t         s_phase;
static esp_http_client_handle_t s_client;
static FILE                    *s_fp;

static size_t   s_fill; /* s_buf に溜まっていて、まだ SD へ書いていないバイト数 */
static uint32_t s_done;
static uint32_t s_total;
static uint32_t s_last_progress_ms;

/* 刻み方の実績（終了時にログへ出す）。 */
static struct {
    uint32_t polls;  /* 読みに行った周 */
    uint32_t budget; /* WGET_READS_PER_POLL 回読み切って打ち切った周 */
    uint32_t slice;  /* WGET_SLICE_MS で打ち切った周 */
    uint32_t max_ms;    /* いちばん長く止めた周（finish を含む） */
    uint32_t finish_ms; /* 書き残し・閉じる・rename にかかった時間 */
} s_st;

static char s_final[200];
static char s_tmp[208];
static char s_why[128];

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool is_redirect(int code)
{
    return code == 301 || code == 302 || code == 303 || code == 307 || code == 308;
}

/* 受け取り中だけ、読みのたびに出るログを黙らせる。
 *
 * ノンブロッキングの TLS は「まだレコードが揃っていない」たびに
 * transport_base が E、HTTP_CLIENT が W を 1 行ずつ出す。どちらも
 * 失敗ではない。ログは 115200bps の UART なので、出すだけで 1 周が延びる。
 * 本当の失敗はこちらが理由を付けて報告するので、終わったら元に戻す。 */
static const char *const QUIET_TAGS[] = {"transport_base", "HTTP_CLIENT"};
static esp_log_level_t   s_saved_level[2];
static bool              s_quiet;

static void quiet_logs(bool on)
{
    if (on == s_quiet) {
        return;
    }
    for (int i = 0; i < 2; i++) {
        if (on) {
            s_saved_level[i] = esp_log_level_get(QUIET_TAGS[i]);
            esp_log_level_set(QUIET_TAGS[i], ESP_LOG_NONE);
        } else {
            esp_log_level_set(QUIET_TAGS[i], s_saved_level[i]);
        }
    }
    s_quiet = on;
}

static void close_client(void)
{
    quiet_logs(false);
    if (s_client != NULL) {
        esp_http_client_close(s_client);
        esp_http_client_cleanup(s_client);
        s_client = NULL;
    }
}

static void close_local(void)
{
    if (s_fp != NULL) {
        cdk_sd_lock();
        fclose(s_fp);
        cdk_sd_unlock();
        s_fp = NULL;
    }
}

/* 後片付け。**書きかけは消す**（scp の受信と同じ作法）。 */
static void cleanup(bool ok)
{
    close_local();
    close_client();
    if (!ok && s_tmp[0] != '\0') {
        cdk_sd_lock();
        remove(s_tmp);
        cdk_sd_unlock();
    }
    s_tmp[0] = '\0';
}

static void fail(const char *fallback)
{
    if (s_why[0] == '\0') {
        snprintf(s_why, sizeof(s_why), "%s", fallback);
    }
    cleanup(false);
    s_phase = CDK_WGET_PHASE_DONE;
}

esp_err_t cdk_wget_init(void)
{
    if (s_buf != NULL) {
        return ESP_OK;
    }
    s_buf = heap_caps_malloc(WGET_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_buf == NULL) {
        ESP_LOGE(TAG, "受け取り用の領域を確保できません");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool cdk_wget_busy(void)
{
    return s_busy;
}

void cdk_wget_abort(void)
{
    if (s_busy) {
        s_abort = true;
    }
}

bool cdk_wget_status(cdk_wget_status_t *out)
{
    if (!s_busy || out == NULL) {
        return false;
    }
    out->phase = s_phase;
    out->done = s_done;
    out->total = s_total;
    return true;
}

/* 開いてヘッダを受け取る。リダイレクトは辿る。成功なら HTTP の状態コード、
 * 繋がらなければ負で why を埋める。 */
static int open_following_redirects(char *why, size_t why_cap)
{
    for (int hops = 0;; hops++) {
        esp_err_t err = esp_http_client_open(s_client, 0);
        if (err != ESP_OK) {
            snprintf(why, why_cap, "繋がりません (%s)", esp_err_to_name(err));
            return -1;
        }
        if (esp_http_client_fetch_headers(s_client) < 0) {
            snprintf(why, why_cap, "応答がありません");
            return -1;
        }
        const int code = esp_http_client_get_status_code(s_client);
        if (!is_redirect(code)) {
            return code;
        }
        if (hops >= CDK_WGET_MAX_REDIRECTS) {
            snprintf(why, why_cap, "リダイレクトが多すぎます (%d 回)",
                     CDK_WGET_MAX_REDIRECTS);
            return -1;
        }
        err = esp_http_client_set_redirection(s_client);
        if (err != ESP_OK) {
            snprintf(why, why_cap, "HTTP %d の行き先が読めません", code);
            return -1;
        }
        /* 本文を読み捨てる。**残すと次の応答の頭に混ざる。** */
        for (;;) {
            const int r = esp_http_client_read(s_client, (char *)s_buf, WGET_CHUNK);
            if (r <= 0) {
                break;
            }
        }
    }
}

esp_err_t cdk_wget_start(const char *url, const char *final_path, char *why,
                         size_t why_cap)
{
    if (s_buf == NULL) {
        snprintf(why, why_cap, "受け取り用の領域がありません（reboot）");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_busy) {
        snprintf(why, why_cap, "もう受け取り中です");
        return ESP_ERR_INVALID_STATE;
    }
    if (url == NULL || final_path == NULL) {
        snprintf(why, why_cap, "引数が足りません");
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(final_path) >= sizeof(s_final)) {
        snprintf(why, why_cap, "保存先のパスが長すぎます");
        return ESP_ERR_INVALID_ARG;
    }

    s_abort = false;
    memset(&s_st, 0, sizeof(s_st));
    s_fill = 0;
    s_done = 0;
    s_total = 0;
    s_fp = NULL;
    s_why[0] = '\0';
    s_tmp[0] = '\0';
    snprintf(s_final, sizeof(s_final), "%s", final_path);

    const esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = CDK_WGET_CONNECT_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "cyberdeck-wget/1.0",
        .max_redirection_count = CDK_WGET_MAX_REDIRECTS,
    };
    s_client = esp_http_client_init(&cfg);
    if (s_client == NULL) {
        snprintf(why, why_cap, "HTTP の準備ができません（メモリ不足の疑い）");
        return ESP_ERR_NO_MEM;
    }

    /* ここはブロッキング。 */
    const int code = open_following_redirects(why, why_cap);
    if (code < 0) {
        close_client();
        return ESP_FAIL;
    }
    if (code < 200 || code >= 300) {
        snprintf(why, why_cap, "HTTP %d", code);
        close_client();
        return ESP_FAIL;
    }

    const int64_t len = esp_http_client_get_content_length(s_client);
    const bool    chunked = esp_http_client_is_chunked_response(s_client);
    if (!chunked && len < 0) {
        snprintf(why, why_cap, "長さの分からない応答には対応していません");
        close_client();
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!chunked && len > (int64_t)UINT32_MAX) {
        snprintf(why, why_cap, "大きすぎます (4GB 超)");
        close_client();
        return ESP_ERR_INVALID_SIZE;
    }
    s_total = chunked ? 0u : (uint32_t)len;

    /* ヘッダが取れてから手元を開く。繋がらない相手で空のファイルを作らない。 */
    snprintf(s_tmp, sizeof(s_tmp), "%s.tmp", s_final);
    cdk_sd_lock();
    s_fp = fopen(s_tmp, "wb");
    cdk_sd_unlock();
    if (s_fp == NULL) {
        snprintf(why, why_cap, "%.80s を開けません", s_tmp);
        s_tmp[0] = '\0'; /* 開けていないので消さない */
        close_client();
        return ESP_FAIL;
    }

    /* ここから先はメインループが刻む。読みの待ちを短くする。 */
    (void)esp_http_client_set_timeout_ms(s_client, CDK_WGET_READ_TIMEOUT_MS);

    /* **下のソケットもノンブロッキングにする。** 期限は select にしか
     * かからず、TLS は select が「読める」と言ったあと mbedtls が
     * **レコード 1 つ分（最大 16KB）揃うまで recv で待つ。** 実機の https で
     * 1 周 200ms 止まった。ノンブロッキングなら WANT_READ で戻り、
     * transport がそれを期限切れ（EAGAIN）に写すので、上はそのまま動く。 */
    const int fd = esp_http_client_get_socket(s_client);
    if (fd >= 0) {
        const int fl = fcntl(fd, F_GETFL, 0);
        (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
    quiet_logs(true);

    s_phase = CDK_WGET_PHASE_XFER;
    s_last_progress_ms = now_ms();
    s_busy = true;
    ESP_LOGI(TAG, "%s -> %s (%s)", url, s_final,
             chunked ? "長さ未定" : "長さあり");
    return ESP_OK;
}

/* 溜めた分を SD へ書く。失敗したら fail して false。 */
static bool flush_buf(void)
{
    if (s_fill == 0) {
        return true;
    }
    cdk_sd_lock();
    const size_t n = fwrite(s_buf, 1, s_fill, s_fp);
    cdk_sd_unlock();
    if (n != s_fill) {
        fail("書き込みに失敗しました（容量は足りていますか）");
        return false;
    }
    s_fill = 0;
    return true;
}

static void finish_body(void);

static void finish(void)
{
    const uint32_t t0 = now_ms();
    finish_body();
    s_st.finish_ms = now_ms() - t0;
}

static void finish_body(void)
{
    if (!flush_buf()) {
        return;
    }
    close_local();
    close_client();
    cdk_sd_lock();
    remove(s_final);
    const int rc = rename(s_tmp, s_final);
    cdk_sd_unlock();
    if (rc != 0) {
        /* **`.tmp` は消さない。** 上書きの指定だと元はもう消してあるので、
         * ここで消すと両方失う（scp の受信と同じ約束）。 */
        s_tmp[0] = '\0';
        snprintf(s_why, sizeof(s_why), "%.80s.tmp に残しました（置けません）",
                 s_final);
        fail("置けません");
        return;
    }
    s_tmp[0] = '\0'; /* もう消させない */
    s_phase = CDK_WGET_PHASE_DONE;
}

/* 1 回読む。進んだら true。 */
static bool pump(void)
{
    /* 読みは 512B ずつ、書きは s_buf が埋まってから。**SD へ 512B ずつ
     * 書くと、書き込みのほうが遅くて 1 周の予算を食う**（実機で、周の
     * 2 割が 30ms の時間切れで、最長 77ms だった）。 */
    if (s_fill + WGET_READ > WGET_CHUNK && !flush_buf()) {
        return false;
    }
    const int r = esp_http_client_read(s_client, (char *)s_buf + s_fill, WGET_READ);
    if (r == -ESP_ERR_HTTP_EAGAIN) {
        return false;
    }
    if (r < 0) {
        fail("途中で切れました");
        return false;
    }
    if (r == 0) {
        if (esp_http_client_is_complete_data_received(s_client)) {
            finish();
        } else {
            fail("途中で切れました（相手が閉じました）");
        }
        return false;
    }
    s_fill += (size_t)r;
    s_done += (uint32_t)r;
    s_last_progress_ms = now_ms();
    return true;
}

void cdk_wget_poll(bool *done, char *report, size_t report_cap)
{
    if (done != NULL) {
        *done = false;
    }
    if (!s_busy) {
        return;
    }

    if (s_phase != CDK_WGET_PHASE_DONE) {
        if (s_abort) {
            snprintf(s_why, sizeof(s_why), "止めました");
            cleanup(false);
            s_phase = CDK_WGET_PHASE_DONE;
        } else if (now_ms() - s_last_progress_ms > CDK_WGET_STALL_MS) {
            fail("応答がありません");
        } else {
            /* **1 周で WGET_SLICE_MS まで。** 満杯で返ってこなかった＝いま
             * 来ている分は汲み切ったので、次の周へ回す。続けると読みごとに
             * 期限ぶん待つ。
             *
             * 最初は scp と同じ「8 回まで」にしていたが、実機（国内のミラー）で
             * 周の 8 割が回数で打ち切られ、約 80KB/s で頭打ちだった。回数を
             * 32 に上げて時間で切るようにすると約 107KB/s（周の 9 割が
             * 時間切れ）。メインループの応答（cdk.py ping）は変わらない。 */
            const uint32_t t0 = now_ms();
            int            i = 0;
            bool           slice = false;
            for (; i < WGET_READS_PER_POLL && s_phase == CDK_WGET_PHASE_XFER; i++) {
                const uint32_t before = s_done;
                if (!pump() || s_done - before < WGET_READ) {
                    break;
                }
                if (now_ms() - t0 >= WGET_SLICE_MS) {
                    slice = true;
                    break;
                }
            }
            const uint32_t spent = now_ms() - t0;
            s_st.polls++;
            if (i == WGET_READS_PER_POLL) s_st.budget++;
            if (slice) s_st.slice++;
            if (spent > s_st.max_ms) s_st.max_ms = spent;
        }
    }

    if (s_phase != CDK_WGET_PHASE_DONE) {
        return;
    }

    if (report != NULL && report_cap > 0) {
        if (s_why[0] == '\0') {
            snprintf(report, report_cap,
                     "\r\033[K\033[32mwget  : %luKB 受け取りました\033[0m\r\n",
                     (unsigned long)((s_done + 1023u) / 1024u));
        } else {
            snprintf(report, report_cap, "\r\033[K\033[31mwget  : %s\033[0m\r\n",
                     s_why);
        }
    }
    /* 刻み方の実績。「予算で打ち切った周」が多ければ予算が速さの上限、
     * 最長の周はメインループを止めた長さ。 */
    ESP_LOGI(TAG, "刻み %lu 周 / 予算切れ %lu / 時間切れ %lu / 最長 %lums / 仕上げ %lums",
             (unsigned long)s_st.polls, (unsigned long)s_st.budget,
             (unsigned long)s_st.slice, (unsigned long)s_st.max_ms,
             (unsigned long)s_st.finish_ms);
    ESP_LOGI(TAG, "終了 %lu/%lu バイト %s", (unsigned long)s_done,
             (unsigned long)s_total, (s_why[0] == '\0') ? "OK" : s_why);
    s_busy = false;
    s_phase = CDK_WGET_PHASE_IDLE;
    if (done != NULL) {
        *done = true;
    }
}
