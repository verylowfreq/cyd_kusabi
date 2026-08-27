#include "cdk_spibus.h"

#include "cdk_board.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#if CDK_BOARD_SPI_SHARED
#include "bsp/m5stack_core_s3.h"
/* LCD の DC と SPI の MISO が同じピン。BSP の定義から取る。 */
#define SHARED_PIN BSP_LCD_DC
#endif

static const char *TAG = "cdk_spibus";

static SemaphoreHandle_t  s_lock;
static cdk_spibus_owner_t s_owner = CDK_SPIBUS_LCD;
static bool               s_owner_known;
static unsigned           s_switches;

esp_err_t cdk_spibus_init(void)
{
    if (s_lock != NULL) {
        return ESP_OK;
    }
    /* 再帰ミューテックス。描画の中から更に取ることがあるため。 */
    s_lock = xSemaphoreCreateRecursiveMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
#if CDK_BOARD_SPI_SHARED
    ESP_LOGI(TAG, "LCD DC と SD MISO が G%d で共用。役割を切り替えて使います",
             (int)SHARED_PIN);
#else
    ESP_LOGI(TAG, "LCD と SD は別バス。ロックだけ持ちます");
#endif
    return ESP_OK;
}

/* バスの所有者を切り替える。
 *
 * **ピンの向きを変えるのは共用している基板だけ。** v1 は LCD が SPI2、
 * SD が SPI3 で物理的に別なので、切り替えるものが無い。ロックと API は
 * 残してあるので、呼び出し側（port_display / port_storage）は基板を
 * 気にしなくてよい。 */
static void switch_owner(cdk_spibus_owner_t owner)
{
    if (s_owner_known && s_owner == owner) {
        return;
    }

#if CDK_BOARD_SPI_SHARED
    if (owner == CDK_SPIBUS_SD) {
        /* 入力にする。こちらが駆動をやめないとカードの出力を読めない。 */
        gpio_set_direction((gpio_num_t)SHARED_PIN, GPIO_MODE_INPUT);
    } else {
        /* 出力に戻す。esp_lcd が DC として gpio_set_level で上げ下げする。 */
        gpio_set_direction((gpio_num_t)SHARED_PIN, GPIO_MODE_OUTPUT);
    }
#endif
    s_owner = owner;
    s_owner_known = true;
    s_switches++;
}

void cdk_spibus_acquire(cdk_spibus_owner_t owner)
{
    if (s_lock == NULL) {
        /* 初期化前でも描画は動かしたい。切り替えだけはしておく。 */
        switch_owner(owner);
        return;
    }
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    switch_owner(owner);
}

void cdk_spibus_release(void)
{
    if (s_lock != NULL) {
        xSemaphoreGiveRecursive(s_lock);
    }
}

unsigned cdk_spibus_switches(void)
{
    return s_switches;
}
