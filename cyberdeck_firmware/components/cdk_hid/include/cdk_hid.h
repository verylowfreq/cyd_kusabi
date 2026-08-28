/* cdk_hid — USB HID ブートキーボードのレポートをキーイベントにする
 *
 * ESP-IDF に依存しない純 C。実機でもホストのテストでも同じコードが動く。
 *
 * 扱うのは**ブートプロトコルの 8 バイト固定レポート**だけ:
 *
 *   [0] modifier ビット
 *   [1] 予約 (0)
 *   [2..7] 同時に押されている usage id を最大 6 個
 *
 * レポートディスクリプタの解析はしない。キーボードなら全機種がこの形を
 * 持っていて、しかも usage id が `cdk_keycode_t`（= HID usage id）と
 * そのまま同じ値になる。特殊キーは変換表すら要らない。
 */
#ifndef CDK_HID_H
#define CDK_HID_H

#include "cdk_keys.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CDK_HID_LAYOUT_US = 0,
    CDK_HID_LAYOUT_JIS = 1
} cdk_hid_layout_t;

/* 押しっぱなしの繰り返し。最初の 1 文字が出てから待つ時間と、その後の間隔。 */
#define CDK_HID_REPEAT_DELAY_MS 400u
#define CDK_HID_REPEAT_RATE_MS  35u

typedef struct {
    uint8_t prev[6]; /* 前回のレポートで押されていた usage id */
    uint8_t prev_mods;

    cdk_hid_layout_t layout;
    bool             caps; /* CapsLock の状態。ブートレポートは LED を返さないので自前で持つ */

    /* オートリピート。**時間はレポートではなく呼び出し側の時計で進める。**
     * ブートキーボードは状態が変わったときしかレポートを送らないので、
     * レポート到着を数えて繰り返すと「別のキーを押したときだけ連射される」
     * という妙な動きになる。 */
    uint8_t  rep_usage;
    uint8_t  rep_mods;
    uint32_t rep_next_ms;
    bool     rep_active;
} cdk_hid_t;

void cdk_hid_init(cdk_hid_t *h, cdk_hid_layout_t layout);
void cdk_hid_set_layout(cdk_hid_t *h, cdk_hid_layout_t layout);

/* 8 バイトのレポートを食わせる。**新しく押された**キーだけを fn に渡す。
 * len が 8 未満なら何もしない。 */
void cdk_hid_report(cdk_hid_t *h, const uint8_t *rep, size_t len, uint32_t now_ms,
                    cdk_key_fn fn, void *user);

/* 時間だけを進める。押しっぱなしのキーを繰り返し出す。
 * メインループから毎周呼ぶ。 */
void cdk_hid_tick(cdk_hid_t *h, uint32_t now_ms, cdk_key_fn fn, void *user);

/* usage id と修飾子から 1 つのキーイベントを作る。テストと診断ログ用。
 * 文字にならないキー（CapsLock など）では false を返す。 */
bool cdk_hid_map(const cdk_hid_t *h, uint8_t usage, uint8_t mods, cdk_key_t *out);

/* JIS にしか無い usage id か。挿さっているキーボードの配列を推測するのに使う。 */
bool cdk_hid_usage_is_jis_only(uint8_t usage);

#ifdef __cplusplus
}
#endif

#endif /* CDK_HID_H */
