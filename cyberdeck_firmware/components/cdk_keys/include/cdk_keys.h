/* cdk_keys — キーイベントを端末が期待するバイト列に変換する
 *
 * ESP-IDF に依存しない純 C。
 *
 * 入力源（シリアル注入 / USB HID / BLE）はすべて cdk_key_t に正規化してから
 * ここを通す。試作機で書いたテストが実機でもそのまま通るようにするため。
 */
#ifndef CDK_KEYS_H
#define CDK_KEYS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cdk_text.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CDK_MOD_CTRL_BIT  0x01u
#define CDK_MOD_SHIFT_BIT 0x02u
#define CDK_MOD_ALT_BIT   0x04u
#define CDK_MOD_GUI_BIT   0x08u

/* HID usage id。cdk_proto の cdk_wire_key_t と同じ値を使う。 */
typedef enum {
    CDK_K_NONE   = 0x00,
    CDK_K_ENTER  = 0x28,
    CDK_K_ESC    = 0x29,
    CDK_K_BS     = 0x2A,
    CDK_K_TAB    = 0x2B,
    CDK_K_SPACE  = 0x2C,
    CDK_K_F1     = 0x3A,
    CDK_K_F12    = 0x45,
    CDK_K_INSERT = 0x49,
    CDK_K_HOME   = 0x4A,
    CDK_K_PGUP   = 0x4B,
    CDK_K_DELETE = 0x4C,
    CDK_K_END    = 0x4D,
    CDK_K_PGDN   = 0x4E,
    CDK_K_RIGHT  = 0x4F,
    CDK_K_LEFT   = 0x50,
    CDK_K_DOWN   = 0x51,
    CDK_K_UP     = 0x52
} cdk_keycode_t;

typedef struct {
    uint16_t keycode;   /* cdk_keycode_t。0 なら codepoint だけ見る */
    uint8_t  mods;      /* CDK_MOD_*_BIT */
    uint32_t codepoint; /* 文字が確定するなら Unicode、無ければ 0 */
} cdk_key_t;

/* 端末へ送るバイト列を out に書き、長さを返す。cap が足りなければ 0。
 *
 * app_cursor は DECCKM (ESC[?1h)。有効だとカーソルキーが ESC O A になる。
 * vi や less がこれを見ている。 */
size_t cdk_keys_encode(const cdk_key_t *k, bool app_cursor, uint8_t *out, size_t cap);


/* ---------------- バイト列 → キーイベント ----------------
 *
 * cdk_keys_encode の逆。シェルがエスケープ列を見なくて済むように、
 * 解釈をここ 1 か所に閉じ込める。
 * ホスト側テストで往復（encode -> decode）を検証しているので、
 * 対応表がずれたら必ず落ちる。
 */

typedef void (*cdk_key_fn)(void *user, const cdk_key_t *key);

typedef enum {
    CDK_KD_GROUND = 0,
    CDK_KD_ESC,
    CDK_KD_CSI,
    CDK_KD_SS3
} cdk_keys_decode_state_t;

typedef struct {
    cdk_keys_decode_state_t state;
    int      params[4];
    int      nparams;
    bool     param_seen;
    cdk_utf8_t utf8;
    /* Alt(ESC 前置)は次に確定するキーに付く。UTF-8 が複数バイトに
     * またがるので、文字が組み上がるまで持ち越す必要がある。 */
    uint8_t  pending_mods;

    cdk_key_fn on_key;
    void      *user;

    uint32_t stat_unknown; /* 捨てたシーケンスの数 */
} cdk_keys_decoder_t;

void cdk_keys_decode_init(cdk_keys_decoder_t *d, cdk_key_fn on_key, void *user);
void cdk_keys_decode_feed(cdk_keys_decoder_t *d, const void *data, size_t len);

/* 入力が途切れたところで呼ぶ。ESC 単独が保留されていれば Esc キーとして吐く。
 * これを呼ばないと「Esc を 1 回押した」が永久に確定しない。 */
void cdk_keys_decode_flush(cdk_keys_decoder_t *d);

#ifdef __cplusplus
}
#endif

#endif /* CDK_KEYS_H */
