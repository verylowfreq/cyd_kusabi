/* cdk_board — 基板ごとの違いを 1 か所に集める
 *
 * ピン番号・画面の大きさ・「その基板にその機能があるか」を、
 * ここだけで持つ。ポート層の各ファイルが `#if CONFIG_CDK_BOARD_...` を
 * 散らかし始めると、基板が 3 枚目になったときに追えなくなる。
 *
 * **v1 のピン番号は HARDWARE_DESIGN.md のピン表と同じもの。**
 * 両方を手で直すとずれるので、`tools/check_board_pins.py` が
 * ctest で突き合わせている。片方だけ直すとテストが落ちる。
 *
 * 機能の有無は `CDK_BOARD_HAS_*` で 0/1 を必ず定義する。`#ifdef` ではなく
 * `#if` で書けるようにするため。未定義を偽として扱うと、綴りを間違えた
 * ときに黙って「機能なし」に倒れる。
 */
#pragma once

/* CONFIG_* を使うので明示的に取り込む。このプロジェクトのビルドでは
 * sdkconfig.h が強制インクルードされないため、忘れると
 * 「基板が選ばれていません」で止まる。 */
#include "sdkconfig.h"

#if defined(CONFIG_CDK_BOARD_CORES3)

#define CDK_BOARD_NAME "M5Stack CoreS3"
#define CDK_BOARD_IS_CORES3 1
#define CDK_BOARD_IS_V1     0

/* BSP が電源投入・SPI・パネル初期化をやる。ピンは BSP が持っている。 */
#define CDK_BOARD_HAS_BSP 1

/* LCD と SD が SPI3 を共有し、さらに LCD の DC と SD の MISO が同じピン。
 * 所有者が変わるたびにピンの向きを切り替える（port_spibus）。 */
#define CDK_BOARD_SPI_SHARED 1

#define CDK_BOARD_HAS_BATTERY_SENSE 0
#define CDK_BOARD_HAS_USB_HOST      0

/* 制御チャネルは USB-Serial-JTAG。ログは UART1 → FT232。 */
#define CDK_BOARD_LINK_IS_USB_SERIAL_JTAG 1

#elif defined(CONFIG_CDK_BOARD_V1)

#define CDK_BOARD_NAME "cyberdeck v1"
#define CDK_BOARD_IS_CORES3 0
#define CDK_BOARD_IS_V1     1

#define CDK_BOARD_HAS_BSP    0
#define CDK_BOARD_SPI_SHARED 0

#define CDK_BOARD_HAS_BATTERY_SENSE 1
#define CDK_BOARD_HAS_USB_HOST      1

/* USB は OTG ホストに使う。内蔵 USB PHY は 1 組しかないので
 * USB-Serial-JTAG は使えない（HARDWARE_DESIGN.md F-1）。 */
#define CDK_BOARD_LINK_IS_USB_SERIAL_JTAG 0

/* ---- 画面 ---- */
#define CDK_BOARD_LCD_WIDTH  480
#define CDK_BOARD_LCD_HEIGHT 320

/* ---- LCD (SPI2 = FSPI の IO_MUX ピン。GPIO マトリクスを通らないので 80MHz 可) ----
 *
 * **ホストは列挙子の名前で書く。** spi_host_device_t は
 * SPI1_HOST=0 / SPI2_HOST=1 / SPI3_HOST=2 で、**周辺機能の番号と 1 つずれる**。
 * 「SPI2 だから 2」と書くと SPI3 に載る。LCD は初期化まで通ってしまい、
 * IO_MUX 直結を失って GPIO マトリクス経由になっていることに気づけない
 * （実機の初火入れで踏んだ）。 */
#define CDK_BOARD_LCD_SPI_HOST SPI2_HOST
#define CDK_BOARD_LCD_BL       8
#define CDK_BOARD_LCD_DC       9
#define CDK_BOARD_LCD_CS      10
#define CDK_BOARD_LCD_MOSI    11
#define CDK_BOARD_LCD_SCLK    12
#define CDK_BOARD_LCD_MISO    13 /* ファームは読み戻さない。未接続でもよい */
#define CDK_BOARD_LCD_RST     14

/* ---- microSD (SPI3。LCD とは別バス) ---- */
#define CDK_BOARD_SD_SPI_HOST SPI3_HOST
#define CDK_BOARD_SD_MISO    39
#define CDK_BOARD_SD_MOSI    40
#define CDK_BOARD_SD_CS      41
#define CDK_BOARD_SD_SCLK    42

/* ---- I2C（Grove） ---- */
#define CDK_BOARD_I2C_SDA 17
#define CDK_BOARD_I2C_SCL 18

/* ---- USB ホスト ---- */
#define CDK_BOARD_USB_DM       19
#define CDK_BOARD_USB_DP       20
#define CDK_BOARD_USB_VBUS_EN   4

/* ---- 電池電圧（ADC1。ADC2 は WiFi 使用中に読めない） ---- */
#define CDK_BOARD_BATT_ADC_GPIO 5
#define CDK_BOARD_BATT_ADC_CH   4 /* ADC1_CH4 */

/* ---- デバッグの 2 チャネル（HARDWARE_DESIGN.md F-1） ---- */
#define CDK_BOARD_LINK_UART     1 /* 制御チャネル */
#define CDK_BOARD_LINK_UART_RX 15
#define CDK_BOARD_LINK_UART_TX 16
#define CDK_BOARD_LOG_UART      0 /* ログ + 書き込み */
#define CDK_BOARD_LOG_UART_TX  43
#define CDK_BOARD_LOG_UART_RX  44

/* ---- 外部 UART（D-sub / Grove。排他） ---- */
#define CDK_BOARD_EXT_UART      2
#define CDK_BOARD_EXT_UART_RX  47
#define CDK_BOARD_EXT_UART_TX  48
#define CDK_BOARD_EXT_UART_RTS 21
#define CDK_BOARD_EXT_UART_CTS 38

/* ---- Grove の ADC ---- */
#define CDK_BOARD_GROVE_ADC0 1
#define CDK_BOARD_GROVE_ADC1 2

#else
#error "基板が選ばれていません。idf.py menuconfig の Cyberdeck → 基板、または sdkconfig.defaults.<board> を指定してください"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* 起動ログに出す用。 */
const char *cdk_board_name(void);

#ifdef __cplusplus
}
#endif
