#include "cdk_keys.h"
#include "t.h"

static uint8_t buf[32];

static const char *enc(uint16_t keycode, uint8_t mods, uint32_t cp, bool app)
{
    static char out[64];
    cdk_key_t k = {keycode, mods, cp};
    const size_t n = cdk_keys_encode(&k, app, buf, sizeof(buf));
    memcpy(out, buf, n);
    out[n] = '\0';
    return out;
}

static void eq(const char *got, const char *want, const char *what)
{
    t_checks++;
    if (strcmp(got, want) != 0) {
        t_failures++;
        printf("   FAIL %s: 期待 [", what);
        for (const char *p = want; *p; p++) {
            printf(*p < 0x20 ? "\\x%02X" : "%c", (unsigned char)*p);
        }
        printf("] 実際 [");
        for (const char *p = got; *p; p++) {
            printf(*p < 0x20 ? "\\x%02X" : "%c", (unsigned char)*p);
        }
        printf("]\n");
    }
}

int main(void)
{
    T_CASE("普通の文字");
    {
        eq(enc(0, 0, 'a', false), "a", "a");
        eq(enc(0, CDK_MOD_SHIFT_BIT, 'A', false), "A", "shift+A");
        /* UTF-8 で送る */
        eq(enc(0, 0, 0x3042, false), "\xE3\x81\x82", "あ");
    }

    T_CASE("Ctrl + 文字");
    {
        eq(enc(0, CDK_MOD_CTRL_BIT, 'c', false), "\x03", "ctrl+c");
        eq(enc(0, CDK_MOD_CTRL_BIT, 'C', false), "\x03", "ctrl+C");
        eq(enc(0, CDK_MOD_CTRL_BIT, 'a', false), "\x01", "ctrl+a");
        eq(enc(0, CDK_MOD_CTRL_BIT, 'd', false), "\x04", "ctrl+d");
        eq(enc(0, CDK_MOD_CTRL_BIT, '[', false), "\x1B", "ctrl+[");
        eq(enc(0, CDK_MOD_CTRL_BIT, '?', false), "\x7F", "ctrl+?");
    }

    T_CASE("Alt は ESC 前置");
    {
        /* CSI 系でないキーはすべて Alt を ESC 前置で表す。
         * Alt+Backspace は readline の backward-kill-word なので落とせない。 */
        eq(enc(CDK_K_BS, CDK_MOD_ALT_BIT, 0, false), "\x1B\x7F",
           "alt+backspace");
        eq(enc(CDK_K_ENTER, CDK_MOD_ALT_BIT, 0, false), "\x1B\r",
           "alt+enter");
        eq(enc(CDK_K_TAB, CDK_MOD_ALT_BIT, 0, false), "\x1B\t",
           "alt+tab");

        eq(enc(0, CDK_MOD_ALT_BIT, 'x', false), "\x1B" "x", "alt+x");
        eq(enc(0, CDK_MOD_ALT_BIT | CDK_MOD_CTRL_BIT, 'c', false), "\x1B\x03",
           "alt+ctrl+c");
    }

    T_CASE("特殊キー");
    {
        eq(enc(CDK_K_ENTER, 0, 0, false), "\r", "enter");
        eq(enc(CDK_K_ESC, 0, 0, false), "\x1B", "esc");
        /* BS は DEL。ここが \b だと CLI の行編集が効かない。 */
        eq(enc(CDK_K_BS, 0, 0, false), "\x7F", "backspace");
        eq(enc(CDK_K_TAB, 0, 0, false), "\t", "tab");
        eq(enc(CDK_K_TAB, CDK_MOD_SHIFT_BIT, 0, false), "\x1B[Z", "shift+tab");
        eq(enc(CDK_K_SPACE, 0, 0, false), " ", "space");
        eq(enc(CDK_K_SPACE, CDK_MOD_CTRL_BIT, 0, false), "\0", "ctrl+space");
    }

    T_CASE("カーソルキー");
    {
        eq(enc(CDK_K_UP, 0, 0, false), "\x1B[A", "up");
        eq(enc(CDK_K_DOWN, 0, 0, false), "\x1B[B", "down");
        eq(enc(CDK_K_RIGHT, 0, 0, false), "\x1B[C", "right");
        eq(enc(CDK_K_LEFT, 0, 0, false), "\x1B[D", "left");

        /* アプリケーションカーソルモード (DECCKM)。vi や less が使う。 */
        eq(enc(CDK_K_UP, 0, 0, true), "\x1BOA", "up (app)");
        eq(enc(CDK_K_LEFT, 0, 0, true), "\x1BOD", "left (app)");

        /* 修飾子付きは常に CSI 形式 */
        eq(enc(CDK_K_RIGHT, CDK_MOD_CTRL_BIT, 0, true), "\x1B[1;5C", "ctrl+right");
        eq(enc(CDK_K_LEFT, CDK_MOD_SHIFT_BIT, 0, false), "\x1B[1;2D", "shift+left");
        eq(enc(CDK_K_UP, CDK_MOD_ALT_BIT, 0, false), "\x1B[1;3A", "alt+up");
    }

    T_CASE("編集キー");
    {
        eq(enc(CDK_K_HOME, 0, 0, false), "\x1B[H", "home");
        eq(enc(CDK_K_END, 0, 0, false), "\x1B[F", "end");
        eq(enc(CDK_K_INSERT, 0, 0, false), "\x1B[2~", "insert");
        eq(enc(CDK_K_DELETE, 0, 0, false), "\x1B[3~", "delete");
        eq(enc(CDK_K_PGUP, 0, 0, false), "\x1B[5~", "pgup");
        eq(enc(CDK_K_PGDN, 0, 0, false), "\x1B[6~", "pgdn");
        eq(enc(CDK_K_DELETE, CDK_MOD_CTRL_BIT, 0, false), "\x1B[3;5~", "ctrl+delete");
    }

    T_CASE("ファンクションキー");
    {
        eq(enc(CDK_K_F1, 0, 0, false), "\x1BOP", "f1");
        eq(enc(CDK_K_F1 + 3, 0, 0, false), "\x1BOS", "f4");
        eq(enc(CDK_K_F1 + 4, 0, 0, false), "\x1B[15~", "f5");
        eq(enc(CDK_K_F1 + 5, 0, 0, false), "\x1B[17~", "f6");
        eq(enc(CDK_K_F12, 0, 0, false), "\x1B[24~", "f12");
        eq(enc(CDK_K_F1, CDK_MOD_CTRL_BIT, 0, false), "\x1B[11;5~", "ctrl+f1");
    }

    T_CASE("知らないキーは何も送らない");
    {
        cdk_key_t k = {0x99, 0, 0};
        T_EQ_INT(cdk_keys_encode(&k, false, buf, sizeof(buf)), 0);
    }

    T_CASE("バッファが足りなければ 0");
    {
        cdk_key_t up = {CDK_K_UP, CDK_MOD_CTRL_BIT, 0};
        uint8_t tiny[2];
        T_EQ_INT(cdk_keys_encode(&up, false, tiny, sizeof(tiny)), 0);
        T_EQ_INT(cdk_keys_encode(&up, false, buf, 0), 0);
    }

    return t_report("test_keys");
}
