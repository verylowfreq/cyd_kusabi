#include "cdk_storage.h"

#include "cdk_board.h"
#include "cdk_spibus.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

#if CDK_BOARD_HAS_BSP
#include "bsp/m5stack_core_s3.h"
#else
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "hal/spi_types.h"
#include "sdmmc_cmd.h"
#endif

#include <string.h>
#include <sys/stat.h>

static const char *TAG = "cdk_sd";

static bool s_mounted;

/* カードの素性。BSP のある基板は BSP が持っているものを指す。 */
#if CDK_BOARD_HAS_BSP
#define CARD_HANDLE bsp_sdcard
#else
static sdmmc_card_t *s_card;
#define CARD_HANDLE s_card
#endif

void cdk_sd_lock(void)
{
    cdk_spibus_acquire(CDK_SPIBUS_SD);
}

void cdk_sd_unlock(void)
{
    cdk_spibus_release();
}

/* マウントとアンマウントだけが基板ごとに違う。あとは共通。 */
#if CDK_BOARD_HAS_BSP

static esp_err_t board_mount(void)
{
    return bsp_sdcard_mount();
}

static esp_err_t board_unmount(void)
{
    return bsp_sdcard_unmount();
}

#else /* v1: LCD とは別バス (SPI3) なので自分でバスを起こす */

static esp_err_t board_mount(void)
{
    const spi_bus_config_t bus = {
        .mosi_io_num = CDK_BOARD_SD_MOSI,
        .miso_io_num = CDK_BOARD_SD_MISO,
        .sclk_io_num = CDK_BOARD_SD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    ESP_RETURN_ON_ERROR(
        spi_bus_initialize(CDK_BOARD_SD_SPI_HOST, &bus, SPI_DMA_CH_AUTO), TAG,
        "spi_bus_initialize");

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = CDK_BOARD_SD_SPI_HOST;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = CDK_BOARD_SD_CS;
    slot.host_id = CDK_BOARD_SD_SPI_HOST;

    /* format_if_mount_failed は false。挿さっているカードを勝手に消さない。 */
    const esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    return esp_vfs_fat_sdspi_mount(CDK_SD_MOUNT_POINT, &host, &slot, &mcfg, &s_card);
}

static esp_err_t board_unmount(void)
{
    const esp_err_t err = esp_vfs_fat_sdcard_unmount(CDK_SD_MOUNT_POINT, s_card);
    if (err == ESP_OK) {
        s_card = NULL;
        (void)spi_bus_free(CDK_BOARD_SD_SPI_HOST);
    }
    return err;
}

#endif

esp_err_t cdk_sd_mount(void)
{
    if (s_mounted) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(cdk_spibus_init(), TAG, "spibus_init");

    /* マウント中もカードを読む。共有バスの基板では、バスを SD 側に
     * しておかないとカードの応答ではなく LCD の DC の値を読んでしまう。 */
    cdk_sd_lock();
    const esp_err_t err = board_mount();
    cdk_sd_unlock();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "マウントできません: %s", esp_err_to_name(err));
        return err;
    }
    s_mounted = true;

    if (CARD_HANDLE != NULL) {
        ESP_LOGI(TAG, "%s: %s %lluMB", CDK_SD_MOUNT_POINT, CARD_HANDLE->cid.name,
                 ((uint64_t)CARD_HANDLE->csd.capacity * CARD_HANDLE->csd.sector_size) /
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
    const esp_err_t err = board_unmount();
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
    if (!s_mounted || CARD_HANDLE == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (name != NULL && name_cap > 0) {
        strncpy(name, CARD_HANDLE->cid.name, name_cap - 1);
        name[name_cap - 1] = '\0';
    }
    if (capacity_bytes != NULL) {
        *capacity_bytes =
            (uint64_t)CARD_HANDLE->csd.capacity * CARD_HANDLE->csd.sector_size;
    }
    if (speed_khz != NULL) {
        *speed_khz = (uint32_t)CARD_HANDLE->max_freq_khz;
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
