/* cdk_spibus — LCD と microSD が共有する SPI3 の交通整理
 *
 * CoreS3 は LCD と microSD が SPI3 を共有している:
 *
 *     MOSI G37 / SCK G36 / MISO G35 / LCD CS G3 / SD CS G4
 *
 * それだけなら CS で分けられるが、**LCD の DC が MISO と同じ G35** に
 * 割り当てられている。パネルは書き込み専用で MISO を使わないので、
 * M5Stack が兼用させている。
 *
 * その結果、ピンの役割を切り替えないと両立しない:
 *
 *   - LCD を叩くとき : G35 は「出力」。esp_lcd が DC として上げ下げする。
 *   - SD を読むとき   : G35 は「入力」。SPI の MISO として使う。
 *
 * G35 を出力のままにして SD を読むと、カードが返す値ではなく
 * こちらが駆動している DC の値を読んでしまう（しかも両者がピンを
 * 取り合う）。逆に入力のままにすると DC が動かず画面が壊れる。
 *
 * どちらの状態にあるかを 1 か所で持ち、切り替えとロックをまとめて扱う。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CDK_SPIBUS_LCD = 0,
    CDK_SPIBUS_SD
} cdk_spibus_owner_t;

/* ミューテックスを作る。port_display / port_storage より先に呼ぶこと。 */
esp_err_t cdk_spibus_init(void);

/* バスを取る。owner が前回と違えば G35 の役割を切り替える。
 * 再入可能（同じタスクからの入れ子は数えるだけ）。 */
void cdk_spibus_acquire(cdk_spibus_owner_t owner);
void cdk_spibus_release(void);

/* 診断用。切り替えが何回起きたか。 */
unsigned cdk_spibus_switches(void);

#ifdef __cplusplus
}
#endif
