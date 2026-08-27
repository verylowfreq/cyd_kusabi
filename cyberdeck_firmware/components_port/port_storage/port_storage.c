#include "cdk_storage.h"

#include "bsp/m5stack_core_s3.h"
#include "cdk_spibus.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

#include <string.h>
#include <sys/stat.h>

static const char *TAG = "cdk_sd";

static bool s_mounted;

void cdk_sd_lock(void)
{
    cdk_spibus_acquire(CDK_SPIBUS_SD);
}

void cdk_sd_unlock(void)
{
    cdk_spibus_release();
}

esp_err_t cdk_sd_mount(void)
{
    if (s_mounted) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(cdk_spibus_init(), TAG, "spibus_init");

    /* マウント中もカードを読む。バスを SD 側にしておかないと
     * カードの応答ではなく LCD の DC の値を読んでしまう。 */
    cdk_sd_lock();
    const esp_err_t err = bsp_sdcard_mount();
    cdk_sd_unlock();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "マウントできません: %s", esp_err_to_name(err));
        return err;
    }
    s_mounted = true;

    if (bsp_sdcard != NULL) {
        ESP_LOGI(TAG, "%s: %s %lluMB", CDK_SD_MOUNT_POINT, bsp_sdcard->cid.name,
                 ((uint64_t)bsp_sdcard->csd.capacity * bsp_sdcard->csd.sector_size) /
                     (1024ULL * 1024ULL));
    }
    return ESP_OK;
}

esp_err_t cdk_sd_unmount(void)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    cdk_sd_lock();
    const esp_err_t err = bsp_sdcard_unmount();
    cdk_sd_unlock();
    if (err == ESP_OK) {
        s_mounted = false;
    }
    return err;
}

bool cdk_sd_mounted(void)
{
    return s_mounted;
}

esp_err_t cdk_sd_info(char *name, size_t name_cap, uint64_t *capacity_bytes,
                      uint32_t *speed_khz)
{
    if (!s_mounted || bsp_sdcard == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (name != NULL && name_cap > 0) {
        strncpy(name, bsp_sdcard->cid.name, name_cap - 1);
        name[name_cap - 1] = '\0';
    }
    if (capacity_bytes != NULL) {
        *capacity_bytes =
            (uint64_t)bsp_sdcard->csd.capacity * bsp_sdcard->csd.sector_size;
    }
    if (speed_khz != NULL) {
        *speed_khz = (uint32_t)bsp_sdcard->max_freq_khz;
    }
    return ESP_OK;
}

esp_err_t cdk_sd_usage(uint64_t *total_bytes, uint64_t *free_bytes)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    /* f_getfree("0:") はドライブ番号の決め打ちで、SD が 0 番に載っている
     * 保証がない。マウント先のパスで引ける esp_vfs_fat_info を使う。 */
    uint64_t total = 0;
    uint64_t freeb = 0;

    cdk_sd_lock();
    const esp_err_t err = esp_vfs_fat_info(CDK_SD_MOUNT_POINT, &total, &freeb);
    cdk_sd_unlock();

    if (err != ESP_OK) {
        return err;
    }
    if (total_bytes != NULL) {
        *total_bytes = total;
    }
    if (free_bytes != NULL) {
        *free_bytes = freeb;
    }
    return ESP_OK;
}
