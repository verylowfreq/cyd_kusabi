#include "cdk_display.h"

#include "bsp/display.h"
#include "bsp/m5stack_core_s3.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "cdk_disp";

/* 1 回の DMA 転送で送る行数。バウンスバッファ 2 枚ぶんの内部 RAM を食う。
 * 480 幅なら 480*16*2 = 15360 バイト x 2 = 30KB。 */
#define BOUNCE_ROWS 16

static esp_lcd_panel_handle_t    s_panel;
static esp_lcd_panel_io_handle_t s_io;
static uint16_t                 *s_bounce[2];
static size_t                    s_bounce_px;
static int                       s_bounce_idx;
static int                       s_width;
static int                       s_height;

esp_err_t cdk_display_init(cdk_display_info_t *info)
{
    if (s_panel != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_width = BSP_LCD_H_RES;
    s_height = BSP_LCD_V_RES;
    s_bounce_px = (size_t)s_width * BOUNCE_ROWS;

    const bsp_display_config_t cfg = {
        .max_transfer_sz = (int)(s_bounce_px * sizeof(uint16_t)),
    };

    /* BSP が AXP2101 / AW9523 の電源投入、SPI、ILI9342C の初期化までやる。
     * ここを自前で書くと LCD が無言で真っ黒になる。 */
    ESP_RETURN_ON_ERROR(bsp_display_new(&cfg, &s_panel, &s_io), TAG, "bsp_display_new");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp_on");
    ESP_RETURN_ON_ERROR(bsp_display_brightness_init(), TAG, "brightness_init");
    ESP_RETURN_ON_ERROR(bsp_display_backlight_on(), TAG, "backlight_on");

    /* バウンスバッファは内部 RAM の DMA 可能領域に置く。
     * 2 枚あるのは esp_lcd の転送が非同期だから（下の flush のコメント参照）。 */
    for (int i = 0; i < 2; i++) {
        s_bounce[i] = heap_caps_malloc(s_bounce_px * sizeof(uint16_t),
                                       MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (s_bounce[i] == NULL) {
            ESP_LOGE(TAG, "バウンスバッファを確保できません (%u バイト)",
                     (unsigned)(s_bounce_px * sizeof(uint16_t)));
            return ESP_ERR_NO_MEM;
        }
    }
    s_bounce_idx = 0;

    if (info != NULL) {
        info->width = s_width;
        info->height = s_height;
    }
    ESP_LOGI(TAG, "パネル %dx%d、バウンス %d 行 x2", s_width, s_height, BOUNCE_ROWS);
    return ESP_OK;
}

esp_err_t cdk_display_set_brightness(int percent)
{
    return bsp_display_brightness_set(percent);
}

esp_err_t cdk_display_flush(cdk_canvas_t *canvas)
{
    if (s_panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (canvas->w != s_width || canvas->h != s_height) {
        ESP_LOGE(TAG, "キャンバス %dx%d がパネル %dx%d と一致しません", canvas->w,
                 canvas->h, s_width, s_height);
        return ESP_ERR_INVALID_SIZE;
    }

    int y0, y1;
    if (!cdk_canvas_dirty(canvas, &y0, &y1)) {
        return ESP_OK;
    }

    for (int y = y0; y < y1; y += BOUNCE_ROWS) {
        int rows = y1 - y;
        if (rows > BOUNCE_ROWS) {
            rows = BOUNCE_ROWS;
        }
        const size_t n = (size_t)rows * (size_t)s_width;
        const uint16_t *src = canvas->pix + (size_t)y * (size_t)s_width;

        /* esp_lcd の転送は非同期だが、次の draw_bitmap がコマンドを送る前に
         * 必ず in-flight の転送を回収する（esp_lcd_panel_io_spi の実装）。
         * したがってバッファを 2 枚で交互に使えば、書き込もうとしている
         * バッファの転送は必ず完了している。 */
        uint16_t *dst = s_bounce[s_bounce_idx];
        s_bounce_idx ^= 1;

        /* ILI9342C はビッグエンディアンの RGB565 を期待する (BSP_LCD_BIGENDIAN)。
         * キャンバスはネイティブ（リトルエンディアン）なのでここで入れ替える。 */
        for (size_t i = 0; i < n; i++) {
            dst[i] = __builtin_bswap16(src[i]);
        }

        ESP_RETURN_ON_ERROR(
            esp_lcd_panel_draw_bitmap(s_panel, 0, y, s_width, y + rows, dst), TAG,
            "draw_bitmap");
    }

    cdk_canvas_dirty_clear(canvas);
    return ESP_OK;
}
