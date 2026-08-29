#include "cdk_net.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "libssh2.h"
#include "cdk_cfg.h"

#include <string.h>

static const char *TAG = "cdk_net";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT    BIT1

static EventGroupHandle_t s_events;
static esp_netif_t       *s_netif;
static bool               s_inited;
static bool               s_connected;
static esp_ip4_addr_t     s_ip;
static int                s_last_reason;

/* 接続中だけ自動再接続する。切断は「失敗」として上位に返したいので、
 * 接続待ちの間だけリトライし、確立後はドライバ任せにしない。 */
static bool s_connecting;
static int  s_retries;
#define MAX_RETRIES 5

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;
        s_last_reason = ev->reason;
        s_connected = false;

        if (s_connecting && s_retries < MAX_RETRIES) {
            s_retries++;
            ESP_LOGW(TAG, "切断 reason=%d 再試行 %d/%d", ev->reason, s_retries,
                     MAX_RETRIES);
            esp_wifi_connect();
        } else if (s_connecting) {
            ESP_LOGE(TAG, "接続を諦めます reason=%d", ev->reason);
            xEventGroupSetBits(s_events, WIFI_FAILED_BIT);
        } else {
            /* 確立後の切断。放っておくと戻らないので繋ぎ直す。 */
            ESP_LOGW(TAG, "接続が切れました reason=%d 再接続します", ev->reason);
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
        ESP_LOGI(TAG, "IP 取得 " IPSTR, IP2STR(&s_ip));
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
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

    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

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

esp_err_t cdk_wifi_connect(const char *ssid, const char *psk, uint32_t timeout_ms)
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
        return ESP_OK;
    }
    if (s_connected) {
        ESP_LOGI(TAG, "別の AP へ移ります");
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

    xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
    s_connecting = true;
    s_retries = 0;
    s_last_reason = 0;

    /* 2 回目以降は start 済みなので connect を直接叩く */
    const esp_err_t started = esp_wifi_start();
    if (started == ESP_ERR_WIFI_NOT_STOPPED || started == ESP_OK) {
        if (started == ESP_OK) {
            /* STA_START イベントが esp_wifi_connect() を呼ぶ */
        } else {
            esp_wifi_connect();
        }
    } else {
        s_connecting = false;
        ESP_RETURN_ON_ERROR(started, TAG, "esp_wifi_start");
    }

    ESP_LOGI(TAG, "%s に接続中...", ssid);
    const EventBits_t bits =
        xEventGroupWaitBits(s_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT, pdFALSE,
                            pdFALSE, pdMS_TO_TICKS(timeout_ms));
    s_connecting = false;

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    if (bits & WIFI_FAILED_BIT) {
        return ESP_ERR_WIFI_NOT_CONNECT;
    }
    ESP_LOGE(TAG, "接続がタイムアウトしました (%u ms)", (unsigned)timeout_ms);
    return ESP_ERR_TIMEOUT;
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
