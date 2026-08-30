#include "cdk_link.h"

#include "cdk_board.h"
#if CDK_BOARD_LINK_IS_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#else
#include "driver/uart.h"
#endif
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "cdk_link";

/* 受信リング。**1 フレームぶん (4KB) を超えて余裕を持たせる。**
 * メインループが画面を流している間も届き続けるので、そこを吸える大きさが要る。 */
#define LINK_RX_RING 8192
#define LINK_TX_RING 4096

/* cdk_link_poll が 1 周で汲む上限。これを超えたら次の周に回して、
 * 流し込まれ続けたときにメインループが止まらないようにする。 */
#define LINK_POLL_MAX 8192

/* 1 回の usb_serial_jtag_write_bytes に渡す最大バイト数。
 * この API は内部でリングバッファに丸ごと積むので、リングより大きい書き込みは
 * 何度呼んでも 0 を返し続ける。必ず分割して渡すこと。 */
#define LINK_TX_SLICE 512

/* ホストが CDC を読んでいないとき、いつまでも待たない。
 * PC を繋がずに単体で動かす場合に固まらせないため。 */
#define LINK_TX_TIMEOUT_MS 500

/* 制御チャネルの速度。スクリーンショットは 480x320x2 = 300KB あるので、
 * ログと同じ 115200 だと 1 枚 26 秒かかる。UART は専用線なので上げてよい。 */
#define CDK_LINK_BAUD 921600

static cdk_proto_rx_t s_rx;
static uint8_t        s_rx_buf[CDK_PROTO_RX_BUF_MIN];
static uint8_t        s_tx_buf[CDK_PROTO_MAX_PAYLOAD + CDK_PROTO_OVERHEAD];
static uint8_t        s_chunk[CDK_PROTO_MAX_PAYLOAD];
static bool           s_ready;

static cdk_proto_frame_fn s_handler;
static void              *s_handler_user;

static void dispatch(void *user, uint8_t type, const uint8_t *payload, uint32_t len)
{
    (void)user;
    if (s_handler != NULL) {
        s_handler(s_handler_user, type, payload, len);
    }
}

void cdk_link_set_handler(cdk_proto_frame_fn fn, void *user)
{
    s_handler = fn;
    s_handler_user = user;
}

esp_err_t cdk_link_init(void)
{
    if (s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

#if CDK_BOARD_LINK_IS_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t cfg = {
        .rx_buffer_size = LINK_RX_RING,
        .tx_buffer_size = LINK_TX_RING,
    };
    ESP_RETURN_ON_ERROR(usb_serial_jtag_driver_install(&cfg), TAG,
                        "usb_serial_jtag_driver_install");
    const char *how = "USB-Serial-JTAG";
#else
    /* v1 は USB を OTG ホストに使うので USB-Serial-JTAG が無い
     * （HARDWARE_DESIGN.md F-1）。制御チャネルは専用の UART に載せる。
     * ログ (UART0) とは別の線なので、パニック時のバックトレースや
     * ブートローダの出力がフレームに割り込まない。 */
    const uart_config_t ucfg = {
        .baud_rate = CDK_LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(CDK_BOARD_LINK_UART, LINK_RX_RING,
                                            LINK_TX_RING, 0, NULL, 0),
                        TAG, "uart_driver_install");
    ESP_RETURN_ON_ERROR(uart_param_config(CDK_BOARD_LINK_UART, &ucfg), TAG,
                        "uart_param_config");
    ESP_RETURN_ON_ERROR(uart_set_pin(CDK_BOARD_LINK_UART, CDK_BOARD_LINK_UART_TX,
                                     CDK_BOARD_LINK_UART_RX, UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE),
                        TAG, "uart_set_pin");
    static char howbuf[48];
    snprintf(howbuf, sizeof(howbuf), "UART%d TX=G%d RX=G%d %d bps",
             CDK_BOARD_LINK_UART, CDK_BOARD_LINK_UART_TX, CDK_BOARD_LINK_UART_RX,
             CDK_LINK_BAUD);
    const char *how = howbuf;
#endif

    cdk_proto_rx_init(&s_rx, s_rx_buf, sizeof(s_rx_buf), dispatch, NULL);
    s_ready = true;
    ESP_LOGI(TAG, "制御チャネル準備完了 (%s)", how);
    return ESP_OK;
}

esp_err_t cdk_link_send(uint8_t type, const void *payload, uint32_t len)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t n = cdk_proto_encode(type, payload, len, s_tx_buf, sizeof(s_tx_buf));
    if (n == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t sent = 0;
    while (sent < n) {
        size_t slice = n - sent;
        if (slice > LINK_TX_SLICE) {
            slice = LINK_TX_SLICE;
        }
#if CDK_BOARD_LINK_IS_USB_SERIAL_JTAG
        const int w = usb_serial_jtag_write_bytes(s_tx_buf + sent, slice,
                                                  pdMS_TO_TICKS(LINK_TX_TIMEOUT_MS));
#else
        const int w = uart_write_bytes(CDK_BOARD_LINK_UART, s_tx_buf + sent, slice);
#endif
        if (w <= 0) {
            /* ホストが読んでいない。捨てて先へ進む。 */
            return ESP_ERR_TIMEOUT;
        }
        sent += (size_t)w;
    }
    return ESP_OK;
}

esp_err_t cdk_link_send_chunked(uint8_t type, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    const size_t body_max = CDK_PROTO_MAX_PAYLOAD - 4u;
    size_t off = 0;

    while (off < len) {
        size_t n = len - off;
        if (n > body_max) {
            n = body_max;
        }
        s_chunk[0] = (uint8_t)(off & 0xFFu);
        s_chunk[1] = (uint8_t)((off >> 8) & 0xFFu);
        s_chunk[2] = (uint8_t)((off >> 16) & 0xFFu);
        s_chunk[3] = (uint8_t)((off >> 24) & 0xFFu);
        memcpy(s_chunk + 4, p + off, n);

        ESP_RETURN_ON_ERROR(cdk_link_send(type, s_chunk, (uint32_t)(n + 4)), TAG,
                            "chunk at %u", (unsigned)off);
        off += n;
    }
    return ESP_OK;
}

void cdk_link_poll(uint32_t wait_ms)
{
    if (!s_ready) {
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
        return;
    }
    /* **溜まっているぶんは 1 周で汲み切る。**
     *
     * 1 回 256 バイトだけ読んで帰ると、921600 baud（約 92 バイト/ms）に対して
     * メインループ 1 周ぶん（5ms 強）で 500 バイト以上届くので追いつかない。
     * リングが溢れたぶんは黙って捨てられ、フレームがまるごと落ちる。
     * ファイル転送 (cdk.py push) で実際に踏んだ。**1 フレームで収まる
     * 送り物しか無いうちは表に出なかった。**
     *
     * 最初の 1 回だけ待ち、あとは残りが無くなるまで待たずに読む。
     * 上限を置いて、流し込まれ続けたときにメインループを止めないようにする。 */
    uint8_t   buf[256];
    uint32_t  wait = wait_ms;
    size_t    total = 0;

    while (total < LINK_POLL_MAX) {
#if CDK_BOARD_LINK_IS_USB_SERIAL_JTAG
        const int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(wait));
#else
        const int n = uart_read_bytes(CDK_BOARD_LINK_UART, buf, sizeof(buf),
                                      pdMS_TO_TICKS(wait));
#endif
        if (n <= 0) {
            break;
        }
        cdk_proto_rx_feed(&s_rx, buf, (size_t)n);
        total += (size_t)n;
        wait = 0; /* 2 回目からは待たない */
    }
}

void cdk_link_stats(uint32_t *frames, uint32_t *bad_crc, uint32_t *dropped)
{
    if (frames != NULL) {
        *frames = s_rx.stat_frames;
    }
    if (bad_crc != NULL) {
        *bad_crc = s_rx.stat_bad_crc;
    }
    if (dropped != NULL) {
        *dropped = s_rx.stat_dropped;
    }
}
