#include "cdk_periph.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "cdk_periph";

/* ------------------------------------------------------------------ */
/* GPIO                                                                */
/* ------------------------------------------------------------------ */

/* CoreS3 で他の用途に取られているピン。
 * 触ると画面が消える・書き込めなくなる・ログが止まる、といった形で
 * 「原因の分かりにくい壊れ方」をするので、コマンドから触らせない。 */
typedef struct {
    int         pin;
    const char *why;
} reserved_pin_t;

static const reserved_pin_t RESERVED[] = {
    {0,  "ブートストラップ / I2S MCLK"},
    {3,  "LCD CS"},
    {4,  "microSD CS"},
    {11, "内蔵 I2C SCL (AXP2101 / AW9523 / タッチ)"},
    {12, "内蔵 I2C SDA (AXP2101 / AW9523 / タッチ)"},
    {13, "I2S DOUT (スピーカー)"},
    {14, "I2S DSIN (マイク)"},
    {15, "カメラ D4"},
    {16, "カメラ D5"},
    {17, "PORT.C TX (デバッグログ)"},
    {18, "PORT.C RX (デバッグログ)"},
    {19, "USB D-"},
    {20, "USB D+"},
    {33, "I2S LCLK"},
    {34, "I2S SCLK"},
    {35, "LCD MISO / DC"},
    {36, "LCD PCLK / SD SCK"},
    {37, "LCD MOSI / SD MOSI"},
    {38, "カメラ HSYNC"},
    {39, "カメラ D0"},
    {40, "カメラ D1"},
    {41, "カメラ D2"},
    {42, "カメラ D3"},
    {45, "カメラ PCLK"},
    {46, "カメラ VSYNC"},
    {47, "カメラ D7"},
    {48, "カメラ D6"},
};

bool cdk_gpio_is_reserved(int pin, const char **why)
{
    /* フラッシュと PSRAM。ここを触ると即座に落ちる。 */
    if (pin >= 26 && pin <= 32) {
        if (why != NULL) {
            *why = "内蔵フラッシュ / PSRAM";
        }
        return true;
    }
    for (size_t i = 0; i < sizeof(RESERVED) / sizeof(RESERVED[0]); i++) {
        if (RESERVED[i].pin == pin) {
            if (why != NULL) {
                *why = RESERVED[i].why;
            }
            return true;
        }
    }
    return false;
}

static bool pin_ok(int pin)
{
    return pin >= 0 && pin < 49 && !cdk_gpio_is_reserved(pin, NULL);
}

esp_err_t cdk_gpio_mode(int pin, const char *mode)
{
    if (!pin_ok(pin) || mode == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .intr_type = GPIO_INTR_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };

    if (strcmp(mode, "out") == 0) {
        /* GPIO_MODE_OUTPUT だと入力バッファが切れるので、直後に gpio get すると
         * 設定した値ではなく 0 が返る。シェルから触る用途では
         * 「書いた値がそのまま読める」方が期待に合うので入出力にする。 */
        cfg.mode = GPIO_MODE_INPUT_OUTPUT;
    } else if (strcmp(mode, "in") == 0) {
        cfg.mode = GPIO_MODE_INPUT;
    } else if (strcmp(mode, "in_pu") == 0) {
        cfg.mode = GPIO_MODE_INPUT;
        cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    } else if (strcmp(mode, "in_pd") == 0) {
        cfg.mode = GPIO_MODE_INPUT;
        cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    return gpio_config(&cfg);
}

esp_err_t cdk_gpio_set(int pin, int level)
{
    if (!pin_ok(pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    return gpio_set_level(pin, level ? 1 : 0);
}

esp_err_t cdk_gpio_get(int pin, int *level)
{
    if (!pin_ok(pin) || level == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *level = gpio_get_level(pin);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* ADC                                                                 */
/* ------------------------------------------------------------------ */

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali[10];

esp_err_t cdk_adc_read(int channel, int *raw, int *mv)
{
    if (channel < 0 || channel > 9 || raw == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_adc == NULL) {
        const adc_oneshot_unit_init_cfg_t init = {.unit_id = ADC_UNIT_1};
        const esp_err_t err = adc_oneshot_new_unit(&init, &s_adc);
        if (err != ESP_OK) {
            return err;
        }
    }

    const adc_oneshot_chan_cfg_t chan = {
        .atten = ADC_ATTEN_DB_12, /* 0-3.1V 程度まで測れる */
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t err = adc_oneshot_config_channel(s_adc, (adc_channel_t)channel, &chan);
    if (err != ESP_OK) {
        return err;
    }
    err = adc_oneshot_read(s_adc, (adc_channel_t)channel, raw);
    if (err != ESP_OK) {
        return err;
    }

    if (mv == NULL) {
        return ESP_OK;
    }
    *mv = -1;
    if (s_cali[channel] == NULL) {
        const adc_cali_curve_fitting_config_t cc = {
            .unit_id = ADC_UNIT_1,
            .chan = (adc_channel_t)channel,
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_cali_create_scheme_curve_fitting(&cc, &s_cali[channel]) != ESP_OK) {
            s_cali[channel] = NULL;
        }
    }
    if (s_cali[channel] != NULL) {
        (void)adc_cali_raw_to_voltage(s_cali[channel], *raw, mv);
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* I2C                                                                 */
/* ------------------------------------------------------------------ */

esp_err_t cdk_i2c_scan(int sda, int scl, uint32_t freq_hz, cdk_periph_out_fn out,
                       void *user, int *found)
{
    if (found != NULL) {
        *found = 0;
    }
    if (sda < 0 || scl < 0 || sda == scl) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 内蔵バスは BSP が握っている。同じピンで 2 つ目のマスタは作れないので、
     * 分かりにくいエラーになる前にここで止める。 */
    if (sda == 12 || scl == 11 || sda == 11 || scl == 12) {
        ESP_LOGE(TAG, "G11/G12 は内蔵 I2C (BSP が使用中) です");
        return ESP_ERR_INVALID_STATE;
    }

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = (gpio_num_t)sda,
        .scl_io_num = (gpio_num_t)scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) {
        return err;
    }

    int n = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(bus, addr, 50) == ESP_OK) {
            n++;
            if (out != NULL) {
                out(user, "  0x%02X\r\n", addr);
            }
        }
    }
    (void)freq_hz; /* probe は既定クロックで十分。速度指定は M4 以降 */
    i2c_del_master_bus(bus);

    if (found != NULL) {
        *found = n;
    }
    return ESP_OK;
}
