#include "cdk_net.h"

#include "esp_log.h"
#include "libssh2.h"

static const char *TAG = "cdk_net";

esp_err_t cdk_net_probe(void)
{
    /* libssh2_init は mbedTLS を初期化する。ここが通れば
     * バックエンドの結線まで含めて実機で生きている。 */
    const int rc = libssh2_init(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "libssh2_init が失敗しました rc=%d", rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "libssh2 %s / mbedTLS バックエンド OK", libssh2_version(0));
    libssh2_exit();
    return ESP_OK;
}
