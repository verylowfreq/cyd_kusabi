/* SKK 辞書の置き場所を実機の事情に合わせて用意する。
 *
 * 純 C の cdk_skk_dict は「オフセットとバイト数で読むコールバック」しか
 * 知らない。ここがフラッシュのパーティションと SD 上のファイルを
 * そのコールバックの形にして渡す。
 *
 * 優先順位は SD → フラッシュ。SD のほうが差し替えが容易なので、
 * 置いてあればそちらを使う（DESIGN.md §9.1）。
 */
#pragma once

#include "cdk_skk_dict.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CDK_SKK_SD_PATH "/sdcard/skk/SKK-JISYO.bin"

typedef enum {
    CDK_SKK_SRC_NONE = 0,
    CDK_SKK_SRC_FLASH,
    CDK_SKK_SRC_SD,
} cdk_skk_src_t;

/* 辞書を開く。開けたら d が使える状態になる。 */
esp_err_t cdk_skkdict_open(cdk_skk_dict_t *d);

void cdk_skkdict_close(void);

cdk_skk_src_t cdk_skkdict_source(void);

/* 開いている辞書の説明（画面に出す用）。開いていなければ "なし"。 */
const char *cdk_skkdict_describe(void);

#ifdef __cplusplus
}
#endif
