#include "cdk_cfg.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <string.h>

static const char *TAG = "cdk_cfg";
static const char *NAMESPACE = "cdk";

static bool s_inited;

static const char *const KEYS[] = {
    CDK_CFG_WIFI_SSID, CDK_CFG_WIFI_PSK,
    CDK_CFG_SSH_HOST,  CDK_CFG_SSH_USER, CDK_CFG_SSH_PASS,
    NULL,
};

esp_err_t cdk_cfg_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS を初期化し直します (%s)", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init に失敗 %s", esp_err_to_name(err));
        return err;
    }
    s_inited = true;
    return ESP_OK;
}

esp_err_t cdk_cfg_get(const char *key, char *out, size_t cap)
{
    if (key == NULL || out == NULL || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err; /* 名前空間が無い = まだ何も書いていない */
    }
    size_t len = cap;
    err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        out[0] = '\0';
    }
    return err;
}

esp_err_t cdk_cfg_set(const char *key, const char *value)
{
    if (key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (value == NULL || value[0] == '\0') {
        err = nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK; /* 無いものを消すのは成功扱い */
        }
    } else {
        err = nvs_set_str(h, key, value);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

bool cdk_cfg_has(const char *key)
{
    char buf[128];
    return cdk_cfg_get(key, buf, sizeof(buf)) == ESP_OK && buf[0] != '\0';
}

const char *const *cdk_cfg_keys(void)
{
    return KEYS;
}

bool cdk_cfg_is_secret(const char *key)
{
    if (key == NULL) {
        return false;
    }
    /* パスワード系は表示するとき伏せる。SSID やホスト名は伏せない
     * （伏せると設定が合っているか確認できず、かえって危ない）。 */
    return strcmp(key, CDK_CFG_WIFI_PSK) == 0 || strcmp(key, CDK_CFG_SSH_PASS) == 0;
}
