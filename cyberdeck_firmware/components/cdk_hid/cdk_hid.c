#include "cdk_hid.h"

#include <string.h>

/* ---- 修飾子 ---------------------------------------------------------- */

/* ブートレポートの [0] バイト。左右は区別せず 1 つにまとめる。 */
#define HID_MOD_LCTRL  0x01u
#define HID_MOD_LSHIFT 0x02u
#define HID_MOD_LALT   0x04u
#define HID_MOD_LGUI   0x08u
#define HID_MOD_RCTRL  0x10u
#define HID_MOD_RSHIFT 0x20u
#define HID_MOD_RALT   0x40u
#define HID_MOD_RGUI   0x80u

static uint8_t mods_of(uint8_t raw)
{
    uint8_t m = 0;
    if ((raw & (HID_MOD_LCTRL | HID_MOD_RCTRL)) != 0u) {
        m |= CDK_MOD_CTRL_BIT;
    }
    if ((raw & (HID_MOD_LSHIFT | HID_MOD_RSHIFT)) != 0u) {
        m |= CDK_MOD_SHIFT_BIT;
    }
    if ((raw & (HID_MOD_LALT | HID_MOD_RALT)) != 0u) {
        m |= CDK_MOD_ALT_BIT;
    }
    if ((raw & (HID_MOD_LGUI | HID_MOD_RGUI)) != 0u) {
        m |= CDK_MOD_GUI_BIT;
    }
    return m;
}

/* ---- 配列表 ----------------------------------------------------------
 *
 * 記号だけを持つ。英字は計算で出せるし、特殊キーは usage id が
 * そのまま cdk_keycode_t なので表に入れる必要がない。
 */

typedef struct {
    uint8_t  usage;
    uint16_t plain;
    uint16_t shift;
} sym_t;

/* 数字段と記号（US 配列） */
static const sym_t K_US[] = {
    {0x1E, '1', '!'},  {0x1F, '2', '@'},  {0x20, '3', '#'},  {0x21, '4', '$'},
    {0x22, '5', '%'},  {0x23, '6', '^'},  {0x24, '7', '&'},  {0x25, '8', '*'},
    {0x26, '9', '('},  {0x27, '0', ')'},  {0x2D, '-', '_'},  {0x2E, '=', '+'},
    {0x2F, '[', '{'},  {0x30, ']', '}'},  {0x31, '\\', '|'}, {0x32, '\\', '|'},
    {0x33, ';', ':'},  {0x34, '\'', '"'}, {0x35, '`', '~'},  {0x36, ',', '<'},
    {0x37, '.', '>'},  {0x38, '/', '?'},
};

/* JIS 配列。**キーの位置は同じでも刻印が違う**ので、同じ usage id から
 * 別の文字を出す。0x87 (ろ) と 0x89 (￥) は JIS にしか無いキー。 */
static const sym_t K_JIS[] = {
    {0x1E, '1', '!'},  {0x1F, '2', '"'},  {0x20, '3', '#'},  {0x21, '4', '$'},
    {0x22, '5', '%'},  {0x23, '6', '&'},  {0x24, '7', '\''}, {0x25, '8', '('},
    {0x26, '9', ')'},  {0x27, '0', '0'},  {0x2D, '-', '='},  {0x2E, '^', '~'},
    {0x2F, '@', '`'},  {0x30, '[', '{'},  {0x31, ']', '}'},  {0x32, ']', '}'},
    {0x33, ';', '+'},  {0x34, ':', '*'},  {0x36, ',', '<'},  {0x37, '.', '>'},
    {0x38, '/', '?'},  {0x87, '\\', '_'}, {0x89, 0xA5, '|'},
};

/* テンキー。NumLock は見ない（付いていない機種が多いので常に数字扱い）。 */
static const sym_t K_PAD[] = {
    {0x54, '/', '/'}, {0x55, '*', '*'}, {0x56, '-', '-'}, {0x57, '+', '+'},
    {0x59, '1', '1'}, {0x5A, '2', '2'}, {0x5B, '3', '3'}, {0x5C, '4', '4'},
    {0x5D, '5', '5'}, {0x5E, '6', '6'}, {0x5F, '7', '7'}, {0x60, '8', '8'},
    {0x61, '9', '9'}, {0x62, '0', '0'}, {0x63, '.', '.'},
};

static bool lookup(const sym_t *tab, size_t n, uint8_t usage, bool shift, uint32_t *cp)
{
    for (size_t i = 0; i < n; i++) {
        if (tab[i].usage == usage) {
            const uint16_t v = shift ? tab[i].shift : tab[i].plain;
            if (v == 0u) {
                return false;
            }
            *cp = v;
            return true;
        }
    }
    return false;
}

bool cdk_hid_usage_is_jis_only(uint8_t usage)
{
    /* International1 (ろ) / International3 (￥) / 変換 / 無変換 / かな。
     * これが 1 つでも来たら、挿さっているのは JIS 配列。 */
    return usage == 0x87u || usage == 0x88u || usage == 0x89u || usage == 0x8Au ||
           usage == 0x8Bu;
}

/* 特殊キー: usage id をそのまま keycode として渡すもの。 */
static bool is_passthrough(uint8_t u)
{
    if (u == CDK_K_ENTER || u == CDK_K_ESC || u == CDK_K_BS || u == CDK_K_TAB ||
        u == CDK_K_SPACE) {
        return true;
    }
    if (u >= CDK_K_F1 && u <= CDK_K_F12) { /* 0x3A..0x45 */
        return true;
    }
    if (u >= CDK_K_INSERT && u <= CDK_K_UP) { /* 0x49..0x52 */
        return true;
    }
    if (u == 0x58u) { /* テンキーの Enter */
        return true;
    }
    return false;
}

bool cdk_hid_map(const cdk_hid_t *h, uint8_t usage, uint8_t mods, cdk_key_t *out)
{
    if (h == NULL || out == NULL || usage == 0u) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->mods = mods;

    const bool shift = (mods & CDK_MOD_SHIFT_BIT) != 0u;

    if (usage == 0x58u) {
        out->keycode = CDK_K_ENTER; /* テンキーの Enter も普通の Enter として扱う */
        return true;
    }
    if (is_passthrough(usage)) {
        out->keycode = usage;
        return true;
    }

    /* 英字。CapsLock は Shift と排他ではなく XOR。 */
    if (usage >= 0x04u && usage <= 0x1Du) {
        const bool upper = (shift != h->caps);
        out->codepoint = (uint32_t)((upper ? 'A' : 'a') + (usage - 0x04u));
        return true;
    }

    uint32_t cp = 0;
    if (h->layout == CDK_HID_LAYOUT_JIS &&
        lookup(K_JIS, sizeof(K_JIS) / sizeof(K_JIS[0]), usage, shift, &cp)) {
        out->codepoint = cp;
        return true;
    }
    if (h->layout == CDK_HID_LAYOUT_US &&
        lookup(K_US, sizeof(K_US) / sizeof(K_US[0]), usage, shift, &cp)) {
        out->codepoint = cp;
        return true;
    }
    if (lookup(K_PAD, sizeof(K_PAD) / sizeof(K_PAD[0]), usage, shift, &cp)) {
        out->codepoint = cp;
        return true;
    }

    return false; /* CapsLock、修飾キー単体、かな、変換など */
}

/* ---- 状態機械 -------------------------------------------------------- */

void cdk_hid_init(cdk_hid_t *h, cdk_hid_layout_t layout)
{
    if (h == NULL) {
        return;
    }
    memset(h, 0, sizeof(*h));
    h->layout = layout;
}

void cdk_hid_set_layout(cdk_hid_t *h, cdk_hid_layout_t layout)
{
    if (h != NULL) {
        h->layout = layout;
    }
}

static bool was_down(const cdk_hid_t *h, uint8_t usage)
{
    for (int i = 0; i < 6; i++) {
        if (h->prev[i] == usage) {
            return true;
        }
    }
    return false;
}

static bool still_down(const uint8_t *keys, uint8_t usage)
{
    for (int i = 0; i < 6; i++) {
        if (keys[i] == usage) {
            return true;
        }
    }
    return false;
}

void cdk_hid_report(cdk_hid_t *h, const uint8_t *rep, size_t len, uint32_t now_ms,
                    cdk_key_fn fn, void *user)
{
    if (h == NULL || rep == NULL || len < 8u) {
        return;
    }

    const uint8_t raw_mods = rep[0];
    const uint8_t mods = mods_of(raw_mods);
    const uint8_t *keys = rep + 2;

    /* ロールオーバ超過。6 個すべてが 0x01 で埋まる。押し直すまで何も出さない。 */
    if (keys[0] == 0x01u) {
        memset(h->prev, 0, sizeof(h->prev));
        h->rep_active = false;
        return;
    }

    for (int i = 0; i < 6; i++) {
        const uint8_t u = keys[i];
        if (u == 0u || was_down(h, u)) {
            continue;
        }

        /* CapsLock は文字を出さず、状態だけ変える。 */
        if (u == 0x39u) {
            h->caps = !h->caps;
            continue;
        }

        cdk_key_t k;
        if (cdk_hid_map(h, u, mods, &k)) {
            if (fn != NULL) {
                fn(user, &k);
            }
            /* いちばん最後に押されたキーだけを繰り返しの対象にする。 */
            h->rep_usage = u;
            h->rep_mods = mods;
            h->rep_next_ms = now_ms + CDK_HID_REPEAT_DELAY_MS;
            h->rep_active = true;
        }
    }

    /* 繰り返し中のキーが離されたら止める。 */
    if (h->rep_active && !still_down(keys, h->rep_usage)) {
        h->rep_active = false;
    }
    /* 修飾子だけが変わった場合も、繰り返しには新しい修飾子を反映する。 */
    h->rep_mods = mods;

    memcpy(h->prev, keys, sizeof(h->prev));
    h->prev_mods = raw_mods;
}

void cdk_hid_tick(cdk_hid_t *h, uint32_t now_ms, cdk_key_fn fn, void *user)
{
    if (h == NULL || !h->rep_active) {
        return;
    }
    /* 符号なしの引き算で比較する。now_ms が一周しても止まらない。 */
    if ((uint32_t)(now_ms - h->rep_next_ms) > 0x7FFFFFFFu) {
        return; /* まだ来ていない */
    }

    cdk_key_t k;
    if (cdk_hid_map(h, h->rep_usage, h->rep_mods, &k)) {
        if (fn != NULL) {
            fn(user, &k);
        }
    }
    h->rep_next_ms = now_ms + CDK_HID_REPEAT_RATE_MS;
}
