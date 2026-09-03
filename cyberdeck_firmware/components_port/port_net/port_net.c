#include "cdk_net.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "libssh2.h"
#include "cdk_cfg.h"

#include <string.h>

static const char *TAG = "cdk_net";

static esp_netif_t   *s_netif;
static bool           s_inited;
static bool           s_connected;
static esp_ip4_addr_t s_ip;
static int            s_last_reason;

/* 状態はイベントタスクが書き、メインループが読む。**volatile が要る。**
 * 32bit の enum なので読み書きは分割されない（読んだ値は必ずどれか 1 つ）。 */
static volatile cdk_wifi_state_t s_state;

/* 最初の接続を諦める刻限 (ms)。0 なら期限なし。
 *
 * **期限の有無が 2 つの繋ぎ方を分ける。** 期限つき = 起動時やコマンドで
 * 繋ぎに行っている最中で、諦めたら FAILED にして人に知らせる。
 * 期限なし = 一度は繋がったあとに切れた場合で、こちらは黙って繋ぎ直し
 * 続ける（電波が戻れば勝手に戻ってほしい）。
 *
 * **64bit で持ってはいけない。** これもイベントタスクが書いてメインループが
 * 読む。64bit の書き込みは 2 語に分かれるので、読む側が半分だけ新しい値を
 * 拾える。「0 を書いた直後の裂けた値」が 0 でない過去の時刻に見えると、
 * 諦めないはずの再接続が FAILED に落ちて二度と繋がらなくなる。 */
static volatile uint32_t s_deadline_ms;

static uint32_t s_timeout_ms;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* 刻限を過ぎたか。**引き算してから符号を見る**ので 49 日で一周しても正しい。 */
static bool past(uint32_t deadline_ms)
{
    return (int32_t)(now_ms() - deadline_ms) > 0;
}

static int s_retries;
#define MAX_RETRIES 5

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    /* **STA_START では繋ぎに行かない。** 繋ぎに行くのは
     * cdk_wifi_connect_start() だけ、という 1 本道にしてある。理由は
     * そちらのコメント（esp_wifi_start の戻り値）を参照。 */
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;
        s_last_reason = ev->reason;
        s_connected = false;

        if (s_state == CDK_WIFI_CONNECTING && s_deadline_ms != 0) {
            /* 最初の接続。回数で諦める。 */
            if (s_retries < MAX_RETRIES) {
                s_retries++;
                ESP_LOGW(TAG, "切断 reason=%d 再試行 %d/%d", ev->reason, s_retries,
                         MAX_RETRIES);
                esp_wifi_connect();
            } else {
                ESP_LOGE(TAG, "接続を諦めます reason=%d", ev->reason);
                s_state = CDK_WIFI_FAILED;
            }
        } else if (s_state == CDK_WIFI_CONNECTED || s_state == CDK_WIFI_CONNECTING) {
            /* 確立後の切断。放っておくと戻らないので繋ぎ直す。
             * 諦めないので期限は持たない。 */
            ESP_LOGW(TAG, "接続が切れました reason=%d 再接続します", ev->reason);
            s_state = CDK_WIFI_CONNECTING;
            s_deadline_ms = 0;
            esp_wifi_connect();
        }
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    if (id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        s_ip = ev->ip_info.ip;
        s_connected = true;
        s_retries = 0;
        s_deadline_ms = 0;
        s_state = CDK_WIFI_CONNECTED;
        ESP_LOGI(TAG, "IP 取得 " IPSTR, IP2STR(&s_ip));
    }
}

esp_err_t cdk_wifi_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(cdk_cfg_init(), TAG, "cdk_cfg_init");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event_loop");

    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == NULL) {
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL),
                        TAG, "wifi handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL, NULL),
                        TAG, "ip handler");

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set_mode");
    /* 認証情報を NVS に残さない。M2 では secrets.h から毎回渡す。 */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set_storage");

    s_inited = true;
    return ESP_OK;
}

esp_err_t cdk_wifi_connect_start(const char *ssid, const char *psk,
                                 uint32_t timeout_ms)
{
    if (!s_inited || ssid == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* **すでに同じ AP に繋がっているなら何もしない。**
     *
     * 接続中に esp_wifi_connect() を呼んでも新しい GOT_IP イベントは
     * 上がらないので、待ち続けて必ずタイムアウトする。接続自体は生きて
     * いるのに「失敗 ESP_ERR_TIMEOUT」と赤く出る、という紛らわしい状態に
     * なっていた（人間の指摘）。
     *
     * SSID が違うときは繋ぎ直す。設定を変えて `wifi connect` したのに
     * 前の AP に繋がったままだと、そちらのほうが分かりにくい。 */
    if (cdk_wifi_ssid_is(ssid)) {
        ESP_LOGI(TAG, "すでに %s に接続しています", ssid);
        s_state = CDK_WIFI_CONNECTED;
        return ESP_OK;
    }
    /* **繋がっているときだけでなく、繋ぎに行っている最中も止める。**
     *
     * esp_wifi_connect() は「まだ前の相手を探している」状態だと
     * ESP_ERR_WIFI_STATE を返して何もしない。起動直後は必ずこの状態なので、
     * 「起動 → 繋がらない → SSID を直して wifi connect」という、待たなく
     * なったことで普通になった順序がそのまま弾かれる（実機で踏んだ）。
     *
     * **先に IDLE へ落とす。** ここで上がる DISCONNECTED を「繋いでいる
     * 最中に切れた」と取られると、まだ古い設定のまま esp_wifi_connect() が
     * 呼ばれて元の AP へ戻ってしまう。 */
    if (s_connected || s_state == CDK_WIFI_CONNECTING) {
        ESP_LOGI(TAG, "前の接続を止めます");
        s_state = CDK_WIFI_IDLE;
        s_deadline_ms = 0;
        (void)esp_wifi_disconnect();
    }

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    if (psk != NULL) {
        strncpy((char *)wc.sta.password, psk, sizeof(wc.sta.password) - 1);
    }
    /* WPA2 以上を要求する。オープン AP に黙って繋がないため。 */
    wc.sta.threshold.authmode = (psk != NULL && psk[0] != '\0') ? WIFI_AUTH_WPA2_PSK
                                                                : WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "set_config");

    /* **状態と期限はドライバを叩く前に置く。** 逆にすると、
     * esp_wifi_connect() がすぐ失敗して DISCONNECTED が上がったときに
     * まだ IDLE のままで、イベント側がどちらの枝にも入らず取りこぼす。 */
    s_retries = 0;
    s_last_reason = 0;
    s_timeout_ms = timeout_ms;
    /* 0 は「期限なし」の意味なので、ちょうど 0 になったら 1 ずらす。
     * 49 日に 1 度しか当たらないが、当たると永久に諦めなくなる。 */
    s_deadline_ms = now_ms() + timeout_ms;
    if (s_deadline_ms == 0) {
        s_deadline_ms = 1;
    }
    s_state = CDK_WIFI_CONNECTING;

    /* **esp_wifi_start() は 2 回目以降も ESP_OK を返す**（冪等）。
     * ESP_ERR_WIFI_NOT_STOPPED ではない。
     *
     * ここを取り違えて「ESP_OK なら STA_START イベントが繋ぎに行く」と
     * していたのが不具合 #60。STA_START は**最初の 1 回しか上がらない**ので、
     * 2 回目以降の `wifi connect` は誰も esp_wifi_connect() を呼ばないまま
     * 20 秒待って reason=0 で失敗していた。ドライバは何もしていないので
     * ログにも一切出ず、「黙って繋がらない」としか見えない。
     *
     * 直し方は**イベント頼みをやめること**。start が通ったら必ず自分で
     * 繋ぎに行き、戻り値も見る。繋ぎに行く道が 1 本になる。 */
    const esp_err_t started = esp_wifi_start();
    if (started != ESP_OK && started != ESP_ERR_WIFI_NOT_STOPPED) {
        s_state = CDK_WIFI_FAILED;
        s_deadline_ms = 0;
        ESP_RETURN_ON_ERROR(started, TAG, "esp_wifi_start");
    }

    esp_err_t rc = esp_wifi_connect();
    if (rc == ESP_ERR_WIFI_STATE) {
        /* 上で止めたつもりでも、ドライバが状態を落とし切る前に来ることが
         * ある。**もう一度だけ切ってから頼む。** 際限なく粘らないのは、
         * ここで回るとメインループが止まるから。 */
        ESP_LOGW(TAG, "まだ前の状態が残っています。切り直します");
        (void)esp_wifi_disconnect();
        rc = esp_wifi_connect();
    }
    if (rc != ESP_OK) {
        /* 繋ぎに行けていないのに CONNECTING で待つと、また 20 秒黙る。 */
        ESP_LOGE(TAG, "esp_wifi_connect が失敗 %s", esp_err_to_name(rc));
        s_state = CDK_WIFI_FAILED;
        s_deadline_ms = 0;
        return rc;
    }

    ESP_LOGI(TAG, "%s に接続中...", ssid);
    return ESP_OK;
}

cdk_wifi_state_t cdk_wifi_poll(void)
{
    /* 時間切れはここでしか見ない。イベントは「切れた」ことしか教えて
     * くれないので、AP が黙っている（応答も切断も返さない）場合は
     * 誰かが時計を見ないと CONNECTING のまま止まる。 */
    const uint32_t deadline = s_deadline_ms; /* 1 回だけ読む */
    if (s_state == CDK_WIFI_CONNECTING && deadline != 0 && past(deadline)) {
        ESP_LOGE(TAG, "接続がタイムアウトしました (%u ms)", (unsigned)s_timeout_ms);
        s_deadline_ms = 0;
        s_state = CDK_WIFI_FAILED;
    }
    return s_state;
}

bool cdk_wifi_ssid_is(const char *ssid)
{
    if (!s_connected || ssid == NULL || ssid[0] == '\0') {
        return false;
    }
    wifi_config_t cur = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &cur) != ESP_OK) {
        return false;
    }
    /* SSID は最大 32 バイトで、終端が無いこともある。 */
    const size_t n = strnlen((const char *)cur.sta.ssid, sizeof(cur.sta.ssid));
    return n == strlen(ssid) && strncmp((const char *)cur.sta.ssid, ssid, n) == 0;
}

bool cdk_wifi_is_connected(void)
{
    return s_connected;
}

void cdk_wifi_ip(char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return;
    }
    if (!s_connected) {
        strncpy(out, "-", cap - 1);
        out[cap - 1] = '\0';
        return;
    }
    snprintf(out, cap, IPSTR, IP2STR(&s_ip));
}

int cdk_wifi_last_disconnect_reason(void)
{
    return s_last_reason;
}

esp_err_t cdk_net_probe(void)
{
    const int rc = libssh2_init(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "libssh2_init が失敗しました rc=%d", rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "libssh2 %s / mbedTLS バックエンド OK", libssh2_version(0));
    libssh2_exit();
    return ESP_OK;
}
