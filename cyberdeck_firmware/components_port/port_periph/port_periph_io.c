/* I2C の読み書きと UART。ユーザーアプリ (cdk_wasm) 向けに開けたままにする口。
 *
 * `cdk_i2c_scan` は毎回バスを作って壊すので、センサを何度も読む用途には
 * 使えない。こちらはバスを開けたままにして、アドレスごとのデバイスハンドルを
 * キャッシュする。
 *
 * **どちらも同時に 1 本だけ。** 理由が違う:
 *  - I2C: 内蔵バス (G11/G12) は BSP が握っていて、そこに 2 つ目のマスタは作れない。
 *  - UART: UART0 がコンソール、UART1 が制御チャネルで、残りが 1 本しかない。
 *
 * ロックは持たない。この機械はアプリが走っている間シェルを止めるので、
 * ここを 2 つのタスクから同時に叩く経路が無い（port_wasm.c を見よ）。
 */
#include "cdk_periph.h"

#include "cdk_board.h"

#include "driver/i2c_master.h"
#include "driver/uart.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "cdk_io";

/* ------------------------------------------------------------------ */
/* I2C                                                                 */
/* ------------------------------------------------------------------ */

#define I2C_DEVS_MAX   8
#define I2C_TIMEOUT_MS 100

/* scan と同じ I2C_NUM_1 を使う。scan は一時的に作って壊すだけなので、
 * 開けている間に scan を呼ばれるとぶつかる。開いていたら scan を断る。 */
static i2c_master_bus_handle_t s_bus;

static struct {
    uint8_t                    addr;
    i2c_master_dev_handle_t    dev;
} s_devs[I2C_DEVS_MAX];
static int s_ndevs;

/* open で言われた速度。**デバイスを足すときに使う。**
 * ESP-IDF の新しい I2C ドライバは、クロックをバスではなくデバイスごとに
 * 持つ。ここに覚えておかないと dev_for() が既定値で足してしまい、
 * 「400kHz と言ったのに 100kHz で動く」という、ログだけ正しくて実物が
 * 違う状態になる。 */
static uint32_t s_freq_hz;

bool cdk_i2c_is_open(void) { return s_bus != NULL; }

esp_err_t cdk_i2c_open(int sda, int scl, uint32_t freq_hz)
{
    if (s_bus != NULL) return ESP_ERR_INVALID_STATE;
    if (sda < 0 || scl < 0 || sda == scl) return ESP_ERR_INVALID_ARG;
    if (freq_hz == 0 || freq_hz > 1000000) return ESP_ERR_INVALID_ARG;

#if CDK_BOARD_HAS_BSP
    /* CoreS3 の内蔵バスは BSP が握っている。同じピンで 2 つ目のマスタは
     * 作れない。v1 に G11/G12 は無いので見ない（予約表が弾く）。 */
    if (sda == 11 || sda == 12 || scl == 11 || scl == 12) {
        ESP_LOGE(TAG, "G11/G12 は内蔵 I2C (BSP が使用中) です");
        return ESP_ERR_INVALID_STATE;
    }
#endif

    /* 予約ピンは GPIO と同じ表で弾く。表を 2 つ持たない。 */
    const char *why = NULL;
    if (cdk_gpio_is_reserved(sda, &why) || cdk_gpio_is_reserved(scl, &why)) {
        ESP_LOGE(TAG, "予約ピンです: %s", why ? why : "?");
        return ESP_ERR_INVALID_ARG;
    }

    const i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = (gpio_num_t)sda,
        .scl_io_num = (gpio_num_t)scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    const esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        s_bus = NULL;
        return err;
    }
    s_ndevs = 0;
    s_freq_hz = freq_hz;
    ESP_LOGI(TAG, "I2C 開始 SDA=G%d SCL=G%d %u Hz", sda, scl, (unsigned)freq_hz);
    return ESP_OK;
}

/* アドレスごとのハンドル。毎回 add/remove すると遅いのでキャッシュする。 */
static esp_err_t dev_for(int addr, i2c_master_dev_handle_t *out)
{
    if (s_bus == NULL) return ESP_ERR_INVALID_STATE;
    if (addr < 0x08 || addr > 0x77) return ESP_ERR_INVALID_ARG;

    for (int i = 0; i < s_ndevs; i++) {
        if (s_devs[i].addr == (uint8_t)addr) { *out = s_devs[i].dev; return ESP_OK; }
    }
    if (s_ndevs >= I2C_DEVS_MAX) return ESP_ERR_NO_MEM;

    const i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = (uint16_t)addr,
        .scl_speed_hz = s_freq_hz, /* open で言われた速度。既定値で足さない */
    };
    i2c_master_dev_handle_t dev = NULL;
    const esp_err_t err = i2c_master_bus_add_device(s_bus, &dcfg, &dev);
    if (err != ESP_OK) return err;

    s_devs[s_ndevs].addr = (uint8_t)addr;
    s_devs[s_ndevs].dev = dev;
    s_ndevs++;
    *out = dev;
    return ESP_OK;
}

esp_err_t cdk_i2c_write(int addr, const void *p, size_t n)
{
    if (p == NULL || n == 0) return ESP_ERR_INVALID_ARG;
    i2c_master_dev_handle_t dev = NULL;
    const esp_err_t err = dev_for(addr, &dev);
    if (err != ESP_OK) return err;
    return i2c_master_transmit(dev, (const uint8_t *)p, n, I2C_TIMEOUT_MS);
}

esp_err_t cdk_i2c_read(int addr, void *p, size_t n)
{
    if (p == NULL || n == 0) return ESP_ERR_INVALID_ARG;
    i2c_master_dev_handle_t dev = NULL;
    const esp_err_t err = dev_for(addr, &dev);
    if (err != ESP_OK) return err;
    return i2c_master_receive(dev, (uint8_t *)p, n, I2C_TIMEOUT_MS);
}

esp_err_t cdk_i2c_write_read(int addr, const void *w, size_t wn, void *r, size_t rn)
{
    if (w == NULL || wn == 0 || r == NULL || rn == 0) return ESP_ERR_INVALID_ARG;
    i2c_master_dev_handle_t dev = NULL;
    const esp_err_t err = dev_for(addr, &dev);
    if (err != ESP_OK) return err;
    return i2c_master_transmit_receive(dev, (const uint8_t *)w, wn,
                                       (uint8_t *)r, rn, I2C_TIMEOUT_MS);
}

esp_err_t cdk_i2c_close(void)
{
    if (s_bus == NULL) return ESP_ERR_INVALID_STATE;
    for (int i = 0; i < s_ndevs; i++) {
        (void)i2c_master_bus_rm_device(s_devs[i].dev);
    }
    s_ndevs = 0;
    const esp_err_t err = i2c_del_master_bus(s_bus);
    s_bus = NULL;
    s_freq_hz = 0;
    return err;
}

/* ------------------------------------------------------------------ */
/* UART                                                                */
/* ------------------------------------------------------------------ */

/* 外に出せるのはここだけ。UART0 = コンソール、UART1 = 制御チャネル。
 * 取り違えるとログか制御チャネルのどちらかが死に、しかも
 * 「実機と話せない」という一番デバッグしにくい形で出る。 */
#define EXT_UART UART_NUM_2

_Static_assert(EXT_UART != UART_NUM_0, "UART0 はコンソール");
_Static_assert(EXT_UART != UART_NUM_1, "UART1 は制御チャネル (cdk_proto)");

#define UART_RX_BUF 1024
#define UART_TX_BUF 1024

static bool s_uart_open;

bool cdk_uart_is_open(void) { return s_uart_open; }

esp_err_t cdk_uart_open(int tx, int rx, int baud)
{
    if (s_uart_open) return ESP_ERR_INVALID_STATE;
    if (baud < 300 || baud > 3000000) return ESP_ERR_INVALID_ARG;
    if (tx == rx) return ESP_ERR_INVALID_ARG;

    const char *why = NULL;
    if (cdk_gpio_is_reserved(tx, &why) || cdk_gpio_is_reserved(rx, &why)) {
        ESP_LOGE(TAG, "予約ピンです: %s", why ? why : "?");
        return ESP_ERR_INVALID_ARG;
    }

    const uart_config_t cfg = {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(EXT_UART, UART_RX_BUF, UART_TX_BUF, 0, NULL, 0);
    if (err != ESP_OK) return err;

    err = uart_param_config(EXT_UART, &cfg);
    if (err == ESP_OK) {
        err = uart_set_pin(EXT_UART, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        (void)uart_driver_delete(EXT_UART);
        return err;
    }

    s_uart_open = true;
    ESP_LOGI(TAG, "UART%d 開始 TX=G%d RX=G%d %d bps", EXT_UART, tx, rx, baud);
    return ESP_OK;
}

int cdk_uart_write(const void *p, int n)
{
    if (!s_uart_open || p == NULL || n <= 0) return -1;
    return uart_write_bytes(EXT_UART, p, (size_t)n);
}

int cdk_uart_read(void *p, int n)
{
    if (!s_uart_open || p == NULL || n <= 0) return -1;
    /* **待たない。** ここでブロックすると、アプリが Ctrl-C を
     * 受け取れる位置（native の入口）に戻ってこられなくなる。 */
    return uart_read_bytes(EXT_UART, p, (uint32_t)n, 0);
}

esp_err_t cdk_uart_close(void)
{
    if (!s_uart_open) return ESP_ERR_INVALID_STATE;
    const esp_err_t err = uart_driver_delete(EXT_UART);
    s_uart_open = false;
    return err;
}
