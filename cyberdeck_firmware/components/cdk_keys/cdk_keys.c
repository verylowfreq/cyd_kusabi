#include "cdk_keys.h"

#include "cdk_text.h"

#include <string.h>

static size_t emit(uint8_t *out, size_t cap, const char *s)
{
    const size_t n = strlen(s);
    if (n > cap) {
        return 0;
    }
    memcpy(out, s, n);
    return n;
}

/* xterm の修飾子コード。1 + shift(1) + alt(2) + ctrl(4) */
static int mod_code(uint8_t mods)
{
    int m = 1;
    if (mods & CDK_MOD_SHIFT_BIT) {
        m += 1;
    }
    if (mods & CDK_MOD_ALT_BIT) {
        m += 2;
    }
    if (mods & CDK_MOD_CTRL_BIT) {
        m += 4;
    }
    return m;
}

/* CSI 系のキーを組み立てる。
 * 修飾子なし: ESC [ <final>   もしくは ESC [ <num> ~
 * 修飾子あり: ESC [ 1 ; <m> <final>  もしくは ESC [ <num> ; <m> ~ */
static size_t csi_key(uint8_t *out, size_t cap, int num, char final, uint8_t mods)
{
    char buf[16];
    int n = 0;
    const int m = mod_code(mods);

    buf[n++] = '\033';
    buf[n++] = '[';
    if (num >= 0) {
        if (num >= 10) {
            buf[n++] = (char)('0' + num / 10);
        }
        buf[n++] = (char)('0' + num % 10);
    } else if (m != 1) {
        buf[n++] = '1';
    }
    if (m != 1) {
        buf[n++] = ';';
        buf[n++] = (char)('0' + m);
    }
    buf[n++] = final;
    buf[n] = '\0';
    return emit(out, cap, buf);
}

/* Ctrl + 文字 → 制御コード。対応が無ければ 0 を返す。 */
static int ctrl_of(uint32_t cp)
{
    if (cp >= 'a' && cp <= 'z') {
        return (int)(cp - 'a' + 1);
    }
    if (cp >= 'A' && cp <= 'Z') {
        return (int)(cp - 'A' + 1);
    }
    switch (cp) {
    case '@': case ' ': return 0x00;
    case '[': return 0x1B;
    case '\\': return 0x1C;
    case ']': return 0x1D;
    case '^': return 0x1E;
    case '_': case '/': return 0x1F;
    case '?': return 0x7F;
    default: return -1;
    }
}

/* ESC 前置なしで、そのまま送る短いバイト列を組み立てる。
 * 戻り値 0 は「このキーは CSI 系なので別扱い」。 */
static size_t simple_key(const cdk_key_t *k, uint8_t *tmp)
{
    switch (k->keycode) {
    case CDK_K_ENTER: tmp[0] = '\r'; return 1;
    case CDK_K_ESC:   tmp[0] = 0x1B; return 1;
    /* BS は DEL を送る。多くの端末と CLI がこちらを期待している。 */
    case CDK_K_BS:    tmp[0] = 0x7F; return 1;
    case CDK_K_SPACE:
        tmp[0] = (k->mods & CDK_MOD_CTRL_BIT) ? 0x00 : ' ';
        return 1;
    case CDK_K_TAB:
        if ((k->mods & CDK_MOD_SHIFT_BIT) != 0u) {
            return 0; /* Shift+Tab は CSI Z */
        }
        tmp[0] = '\t';
        return 1;
    default:
        return 0;
    }
}

size_t cdk_keys_encode(const cdk_key_t *k, bool app_cursor, uint8_t *out, size_t cap)
{
    if (k == NULL || out == NULL || cap == 0) {
        return 0;
    }

    /* CSI 系のキーは修飾子をシーケンスの中に埋め込む。
     * それ以外（文字キー、Enter、BS、Tab、Esc、Space）は Alt を ESC 前置で表す。
     * Alt+Backspace は readline の backward-kill-word なので落とせない。 */
    uint8_t tmp[8];
    size_t n = simple_key(k, tmp);

    if (n == 0 && k->keycode == CDK_K_NONE && k->codepoint != 0u) {
        if ((k->mods & CDK_MOD_CTRL_BIT) != 0u) {
            const int c = ctrl_of(k->codepoint);
            if (c >= 0) {
                tmp[0] = (uint8_t)c;
                n = 1;
            }
        }
        if (n == 0) {
            n = cdk_utf8_encode(k->codepoint, tmp);
        }
    }

    if (n > 0) {
        const bool alt = (k->mods & CDK_MOD_ALT_BIT) != 0u;
        const size_t total = n + (alt ? 1u : 0u);
        if (cap < total) {
            return 0;
        }
        size_t i = 0;
        if (alt) {
            out[i++] = 0x1B;
        }
        memcpy(out + i, tmp, n);
        return total;
    }

    /* ここから CSI 系 */
    switch (k->keycode) {
    case CDK_K_TAB: /* Shift+Tab */
        return emit(out, cap, "\033[Z");

    case CDK_K_UP:
    case CDK_K_DOWN:
    case CDK_K_RIGHT:
    case CDK_K_LEFT: {
        const char final = (k->keycode == CDK_K_UP)      ? 'A'
                           : (k->keycode == CDK_K_DOWN)  ? 'B'
                           : (k->keycode == CDK_K_RIGHT) ? 'C'
                                                         : 'D';
        if (app_cursor && mod_code(k->mods) == 1) {
            const char s[4] = {'\033', 'O', final, '\0'};
            return emit(out, cap, s);
        }
        return csi_key(out, cap, -1, final, k->mods);
    }

    case CDK_K_HOME: return csi_key(out, cap, -1, 'H', k->mods);
    case CDK_K_END:  return csi_key(out, cap, -1, 'F', k->mods);

    case CDK_K_INSERT: return csi_key(out, cap, 2, '~', k->mods);
    case CDK_K_DELETE: return csi_key(out, cap, 3, '~', k->mods);
    case CDK_K_PGUP:   return csi_key(out, cap, 5, '~', k->mods);
    case CDK_K_PGDN:   return csi_key(out, cap, 6, '~', k->mods);

    default:
        if (k->keycode >= CDK_K_F1 && k->keycode <= CDK_K_F12) {
            const int f = k->keycode - CDK_K_F1; /* 0..11 */
            if (f < 4 && mod_code(k->mods) == 1) {
                const char s[4] = {'\033', 'O', (char)('P' + f), '\0'};
                return emit(out, cap, s);
            }
            /* F1-F12 の ~ 形式。番号は歴史的に飛んでいる。 */
            static const int nums[12] = {11, 12, 13, 14, 15, 17,
                                         18, 19, 20, 21, 23, 24};
            return csi_key(out, cap, nums[f], '~', k->mods);
        }
        return 0; /* 知らないキーは何も送らない */
    }
}
