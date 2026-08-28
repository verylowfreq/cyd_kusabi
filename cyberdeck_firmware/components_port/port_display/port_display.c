#include "cdk_display.h"

#include "cdk_board.h"
#include "cdk_spibus.h"
#if CDK_BOARD_HAS_BSP
#include "bsp/display.h"
#include "bsp/m5stack_core_s3.h"
#else
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_ili9488.h"
#include "hal/spi_types.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_io.h"
#include "soc/spi_pins.h"

/* **不具合 #34 の再発防止。** HARDWARE_DESIGN.md は「LCD の 4 本は SPI2
 * (FSPI) の IO_MUX ピンそのものなので GPIO マトリクスを通らず 80MHz 出せる」
 * を根拠に、この 4 本をここに固定している。
 *
 * ところが `CDK_BOARD_LCD_SPI_HOST` を 1 つ間違えて SPI3 に載せると、
 * **初期化も転送も成功したまま IO_MUX 直結だけが静かに失われる**。
 * 画面は正常に出るので、実機で見ても host テストでも気づけない。
 * 実際、SD が範囲外の番号で落ちてくれなければ見つからなかった。
 *
 * なので「速いはず」の前提そのものを SoC の定義と突き合わせてビルドを止める。
 * `spi_host_device_t` は SPI1_HOST=0 / SPI2_HOST=1 / SPI3_HOST=2 で
 * 周辺機能名と 1 つずれるため、`2` と書くとここで落ちる。 */
/* **メッセージは ASCII で書く。** gcc は静的アサートの文字列を
 * そのままバイトで診断に出すので、日本語だと 8 進エスケープの羅列になり、
 * 肝心の「どう直すか」が読めなくなる（この assert を故障注入で試して判明）。
 * 説明は上のコメントに書き、メッセージは短い ASCII にする。 */
_Static_assert(CDK_BOARD_LCD_SPI_HOST == SPI2_HOST,
               "LCD pins are SPI2 IO_MUX pins: host must be SPI2_HOST. "
               "Note spi_host_device_t is off by one from the peripheral "
               "name (SPI1_HOST=0/SPI2_HOST=1/SPI3_HOST=2). See "
               "HARDWARE_DESIGN.md before changing.");
_Static_assert(CDK_BOARD_LCD_CS   == SPI2_IOMUX_PIN_NUM_CS &&
               CDK_BOARD_LCD_MOSI == SPI2_IOMUX_PIN_NUM_MOSI &&
               CDK_BOARD_LCD_SCLK == SPI2_IOMUX_PIN_NUM_CLK &&
               CDK_BOARD_LCD_MISO == SPI2_IOMUX_PIN_NUM_MISO,
               "LCD pins left the SPI2 IO_MUX set: routing falls back to the "
               "GPIO matrix and 80MHz is lost. The picture still looks fine, "
               "so this is invisible on device.");
#endif
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "cdk_disp";

/* 1 回の転送で送る行数。
 *
 * CoreS3: エンディアン入れ替え用のバウンスバッファ 2 枚。
 *         320 幅で 320*16*2 = 10KB x2。
 * v1:     ILI9488 のドライバが RGB666 への変換バッファを内部に持つ。
 *         480*16*3 = 23KB を起動時に 1 回だけ確保する。
 *         **ドライバ側に境界検査が無い**ので、1 回に渡す画素数を
 *         この帯より大きくしてはいけない。 */
#define BOUNCE_ROWS 16

static esp_lcd_panel_handle_t    s_panel;
static esp_lcd_panel_io_handle_t s_io;
static size_t                    s_bounce_px; /* 帯 1 つぶんの画素数 */
static int                       s_width;
static int                       s_height;
#if CDK_BOARD_HAS_BSP
static uint16_t                 *s_bounce[2];
static int                       s_bounce_idx;
#endif

/* パネルを起こすところだけが基板ごとに違う。転送 (cdk_display_flush) は
 * 大きさを実行時に持っているので、そのまま共通で動く。 */
#if CDK_BOARD_HAS_BSP

static esp_err_t board_panel_init(void)
{
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
    return ESP_OK;
}

static esp_err_t board_set_brightness(int percent)
{
    return bsp_display_brightness_set(percent);
}

#else /* v1: ILI9488 (480x320) を SPI2 の IO_MUX ピンで駆動する */

/* バックライトは PWM。LEDC の 1 チャネルだけ使う。 */
#define BL_TIMER   LEDC_TIMER_0
#define BL_CHANNEL LEDC_CHANNEL_0
#define BL_BITS    LEDC_TIMER_10_BIT

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BL_BITS,
        .timer_num = BL_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc_timer");

    const ledc_channel_config_t ch = {
        .gpio_num = CDK_BOARD_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BL_CHANNEL,
        .timer_sel = BL_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    return ledc_channel_config(&ch);
}

static esp_err_t board_panel_init(void)
{
    s_width = CDK_BOARD_LCD_WIDTH;
    s_height = CDK_BOARD_LCD_HEIGHT;
    s_bounce_px = (size_t)s_width * BOUNCE_ROWS;

    /* LCD は SPI2。CS/MOSI/SCLK/MISO が FSPI の IO_MUX ピンそのものなので、
     * GPIO マトリクスを通らずに 80MHz まで上げられる（HARDWARE_DESIGN.md）。 */
    const spi_bus_config_t bus = {
        .mosi_io_num = CDK_BOARD_LCD_MOSI,
        .miso_io_num = CDK_BOARD_LCD_MISO,
        .sclk_io_num = CDK_BOARD_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = (int)(s_bounce_px * 3), /* RGB666 は 1 画素 3 バイト */
    };
    ESP_RETURN_ON_ERROR(
        spi_bus_initialize(CDK_BOARD_LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO), TAG,
        "spi_bus_initialize");

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = CDK_BOARD_LCD_CS,
        .dc_gpio_num = CDK_BOARD_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 4,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(
                            (esp_lcd_spi_bus_handle_t)CDK_BOARD_LCD_SPI_HOST,
                            &io_cfg, &s_io),
                        TAG, "panel_io_spi");

    /* 初期化列は atanisoft/esp_lcd_ili9488 に任せる。ベンダの
     * レジスタ列を当てずっぽうで書くと、映らないときに
     * 「配線」「初期化」「転送」のどれが悪いのか切り分けられない。
     *
     * **bits_per_pixel は 18 でなければならない。** ILI9488 は SPI 接続だと
     * 16bit を受け付けず、RGB666 の 3 バイトしか通らない。
     *
     * 第 3 引数は変換用バッファの**画素数**（中で x3 して確保される）。
     * これを起動時に 1 回取るので、転送のたびに malloc しない
     * （DESIGN.md §14）。**draw_bitmap 側に境界検査は無い。**
     * 1 回に渡す画素数がここを超えるとヒープを壊すので、
     * 帯の大きさ (BOUNCE_ROWS 行) と必ず一致させること。 */
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = CDK_BOARD_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 18,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_ili9488(s_io, &panel_cfg, s_bounce_px, &s_panel), TAG,
        "new_panel_ili9488");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    /* ILI9488 の素の向きは 320x480（縦）。横長で使うので入れ替える。
     *
     * mirror の 2 つは実機で決めた。最初 (true, false) にしたら 180 度
     * 回った絵が出た（初期化も転送も正しく、向きだけが違う）。
     * ここは論理では決まらないので、パネルを見て合わせるしかない。 */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, true), TAG, "swap_xy");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, false, true), TAG, "mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp_on");

    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight_init");
    ESP_RETURN_ON_ERROR(cdk_display_set_brightness(100), TAG, "brightness");
    return ESP_OK;
}

static esp_err_t board_set_brightness(int percent)
{
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    const uint32_t max = (1u << BL_BITS) - 1u;
    const uint32_t duty = (max * (uint32_t)percent) / 100u;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL, duty), TAG,
                        "set_duty");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL);
}

#endif

esp_err_t cdk_display_init(cdk_display_info_t *info)
{
    if (s_panel != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 共有バスの基板では、ここで役割の切り替え役を用意しておく。
     * 別バスの基板でもロックは持つので、呼び出し側は同じ。 */
    ESP_RETURN_ON_ERROR(cdk_spibus_init(), TAG, "cdk_spibus_init");
    ESP_RETURN_ON_ERROR(board_panel_init(), TAG, "board_panel_init");

    /* バウンスバッファは内部 RAM の DMA 可能領域に置く。
     * 2 枚あるのは esp_lcd の転送が非同期だから（下の flush のコメント参照）。 */
#if CDK_BOARD_HAS_BSP
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
#endif

    if (info != NULL) {
        info->width = s_width;
        info->height = s_height;
    }
    ESP_LOGI(TAG, "パネル %dx%d、帯 %d 行", s_width, s_height, BOUNCE_ROWS);
    return ESP_OK;
}

esp_err_t cdk_display_set_brightness(int percent)
{
    return board_set_brightness(percent);
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

    /* 転送のあいだ G35 を DC（出力）にしておく。SD が入力にしたままだと
     * DC が動かず、コマンドとデータの区別がつかなくなって画面が壊れる。 */
    cdk_spibus_acquire(CDK_SPIBUS_LCD);

    for (int y = y0; y < y1; y += BOUNCE_ROWS) {
        int rows = y1 - y;
        if (rows > BOUNCE_ROWS) {
            rows = BOUNCE_ROWS;
        }
        const uint16_t *src = canvas->pix + (size_t)y * (size_t)s_width;

        /* esp_lcd の転送は非同期だが、次の draw_bitmap がコマンドを送る前に
         * 必ず in-flight の転送を回収する（esp_lcd_panel_io_spi の実装）。
         * したがってバッファを 2 枚で交互に使えば、書き込もうとしている
         * バッファの転送は必ず完了している。 */
#if CDK_BOARD_HAS_BSP
        /* ILI9342C はビッグエンディアンの RGB565 を期待する (BSP_LCD_BIGENDIAN)。
         * キャンバスはネイティブ（リトルエンディアン）なのでここで入れ替える。 */
        uint16_t *dst = s_bounce[s_bounce_idx];
        s_bounce_idx ^= 1;
        const size_t n = (size_t)rows * (size_t)s_width;
        for (size_t i = 0; i < n; i++) {
            dst[i] = __builtin_bswap16(src[i]);
        }
        const void *payload = dst;
#else
        /* ILI9488 はネイティブの RGB565 をそのまま受け取り、ドライバが
         * RGB666 へ広げて送る。入れ替えもバウンスも要らないので、
         * キャンバスの中を直接指す。 */
        const void *payload = src;
#endif
        const esp_err_t err =
            esp_lcd_panel_draw_bitmap(s_panel, 0, y, s_width, y + rows, payload);
        if (err != ESP_OK) {
            cdk_spibus_release();
            ESP_LOGE(TAG, "draw_bitmap に失敗 %s", esp_err_to_name(err));
            return err;
        }
    }

    cdk_spibus_release();
    cdk_canvas_dirty_clear(canvas);
    return ESP_OK;
}
