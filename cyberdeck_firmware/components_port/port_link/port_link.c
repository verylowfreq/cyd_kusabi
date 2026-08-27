#include "cdk_link.h"

#include "driver/usb_serial_jtag.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "cdk_link";

#define LINK_RX_RING 2048
#define LINK_TX_RING 4096

/* 1 回の usb_serial_jtag_write_bytes に渡す最大バイト数。
 * この API は内部でリングバッファに丸ごと積むので、リングより大きい書き込みは
 * 何度呼んでも 0 を返し続ける。必ず分割して渡すこと。 */
#define LINK_TX_SLICE 512

/* ホストが CDC を読んでいないとき、いつまでも待たない。
 * PC を繋がずに単体で動かす場合に固まらせないため。 */
#define LINK_TX_TIMEOUT_MS 500

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

    usb_serial_jtag_driver_config_t cfg = {
        .rx_buffer_size = LINK_RX_RING,
        .tx_buffer_size = LINK_TX_RING,
    };
    ESP_RETURN_ON_ERROR(usb_serial_jtag_driver_install(&cfg), TAG,
                        "usb_serial_jtag_driver_install");

    cdk_proto_rx_init(&s_rx, s_rx_buf, sizeof(s_rx_buf), dispatch, NULL);
    s_ready = true;
    ESP_LOGI(TAG, "制御チャネル準備完了 (USB-Serial-JTAG)");
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
        const int w = usb_serial_jtag_write_bytes(s_tx_buf + sent, slice,
                                                  pdMS_TO_TICKS(LINK_TX_TIMEOUT_MS));
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
    uint8_t buf[256];
    const int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(wait_ms));
    if (n > 0) {
        cdk_proto_rx_feed(&s_rx, buf, (size_t)n);
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
