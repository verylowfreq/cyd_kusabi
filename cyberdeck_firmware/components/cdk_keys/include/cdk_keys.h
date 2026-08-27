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

#ifdef __cplusplus
}
#endif

#endif /* CDK_KEYS_H */
