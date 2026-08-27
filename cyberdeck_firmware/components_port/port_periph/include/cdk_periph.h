/* cdk_periph — GPIO / I2C / ADC の実操作
 *
 * 「マイコンであることを活かした端末」の実体。シェルのコマンドから呼ぶ。
 *
 * 出力は呼び出し側が渡す printf 風のコールバックに書く。
 * こうしておくとシェルにも SSH にもログにも同じコードで出せる。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*cdk_periph_out_fn)(void *user, const char *fmt, ...);

/* ---- GPIO ---- */

/* そのピンが基板の他の用途で埋まっているか。
 * 埋まっていれば why に用途を入れて true を返す。
 * 黙って触ると画面が消えたり書き込みできなくなったりするので必ず通すこと。 */
bool cdk_gpio_is_reserved(int pin, const char **why);

esp_err_t cdk_gpio_mode(int pin, const char *mode); /* in / in_pu / in_pd / out */
esp_err_t cdk_gpio_set(int pin, int level);
esp_err_t cdk_gpio_get(int pin, int *level);

/* ---- ADC ---- */

/* ADC1 のチャネル。CoreS3 で外に出ているピンだけが意味を持つ。
 * mv には校正済みの電圧 [mV] が入る（校正が使えなければ -1）。 */
esp_err_t cdk_adc_read(int channel, int *raw, int *mv);

/* ---- I2C ---- */

/* 指定ピンに一時的なマスタを作って 0x08-0x77 を叩く。
 * 見つかったアドレスを out に流し、個数を found に入れる。 */
esp_err_t cdk_i2c_scan(int sda, int scl, uint32_t freq_hz,
                       cdk_periph_out_fn out, void *user, int *found);

#ifdef __cplusplus
}
#endif
