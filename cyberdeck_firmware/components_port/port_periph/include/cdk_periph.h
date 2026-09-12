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

/* ---- I2C（読み書き） ----
 *
 * scan と違って**バスを開けたままにする**。ユーザーアプリが
 * センサを何度も読むための口（cdk_wasm の i2c_*）。
 *
 * **同時に開けるのは 1 本だけ。** 内蔵バス (G11/G12) は BSP が握って
 * いるので使えない。addr ごとのデバイスハンドルは中でキャッシュする。
 */
esp_err_t cdk_i2c_open(int sda, int scl, uint32_t freq_hz);
bool      cdk_i2c_is_open(void);
/* 開いているバスで 1 アドレスだけ叩く。**走査はこれで書く。**
 * 「アドレス + 書き 0 バイト」しか出さないので、相手は 1 ビットも
 * 吐き出さない。読みで走査すると、予期しない読みを投げられた相手が
 * 来ない続きを待って SCL を握り続け、電源を切るまで戻らなくなる
 * （実機の SHT30 で再現した。port_periph_io.c を見よ）。
 * 居なければ ESP_ERR_NOT_FOUND。 */
esp_err_t cdk_i2c_probe(int addr);
esp_err_t cdk_i2c_write(int addr, const void *p, size_t n);
esp_err_t cdk_i2c_read(int addr, void *p, size_t n);
/* 書いてから読む（リピートスタート）。レジスタ読みはこれ。 */
esp_err_t cdk_i2c_write_read(int addr, const void *w, size_t wn, void *r, size_t rn);
esp_err_t cdk_i2c_close(void);

/* ---- UART ----
 *
 * **外部に出せる UART は 1 本しかない。** UART0 はコンソール、UART1 は
 * 制御チャネル（cdk_proto）で埋まっている。残る 1 本を貸し出す。
 * 本数を増やせないので、cdk_wasm 側の上限も 1 にしてある。
 *
 * ピンは予約表 (cdk_gpio_is_reserved) を通す。塞がっているピンを
 * 指定すると ESP_ERR_INVALID_ARG。
 *
 * `flow` が真なら RTS/CTS のハードウェアフロー制御を入れ、rts/cts の
 * ピンも掴む（予約表にも通す）。偽なら rts/cts は見ないし、そのピンは
 * 触らないままにする（フロー制御を使わないなら空くピンなので）。
 */
esp_err_t cdk_uart_open(int tx, int rx, int baud, bool flow, int rts, int cts);
bool      cdk_uart_is_open(void);
/* **詰まっていたら待たずに捨てて -1 を返す。**
 * フロー制御を入れて相手の CTS が下がったままだと TX が捌けないので、
 * uart_write_bytes に渡すと無期限に待つ。呼ぶのはメインループなので、
 * そこで待つと画面・キー・制御チャネルまで一緒に止まる。 */
int       cdk_uart_write(const void *p, int n);
/* 待たない。無ければ 0。 */
int       cdk_uart_read(void *p, int n);
esp_err_t cdk_uart_close(void);

#ifdef __cplusplus
}
#endif
