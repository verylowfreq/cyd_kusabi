/* cdk_storage — microSD (FATFS)
 *
 * LCD と SPI3 を共有しているので、アクセスは必ず cdk_spibus 経由でバスを
 * 取ってから行う。詳しくは cdk_spibus.h。
 *
 * ここは「マウントとバス管理」だけを持つ。ファイル操作は普通の
 * fopen / opendir でよく、呼び出し側が cdk_sd_lock/unlock で囲む。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CDK_SD_MOUNT_POINT "/sdcard"

esp_err_t cdk_sd_mount(void);
esp_err_t cdk_sd_unmount(void);
bool      cdk_sd_mounted(void);

/* カードの素性。未マウントなら ESP_ERR_INVALID_STATE。 */
esp_err_t cdk_sd_info(char *name, size_t name_cap, uint64_t *capacity_bytes,
                      uint32_t *speed_khz);

/* 空き容量。FATFS に問い合わせるので少し時間がかかる。 */
esp_err_t cdk_sd_usage(uint64_t *total_bytes, uint64_t *free_bytes);

/* ファイル操作をこれで囲む。中で SPI バスを SD 側へ切り替える。
 * 囲まないと LCD の DC 出力とぶつかって読み書きが化ける。 */
void cdk_sd_lock(void);
void cdk_sd_unlock(void);

#ifdef __cplusplus
}
#endif
