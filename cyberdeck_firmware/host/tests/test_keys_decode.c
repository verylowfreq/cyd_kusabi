/* cdk_keys_decode の単体テスト。
 *
 * 目玉は往復テスト。encode でバイト列にして decode で戻し、
 * 元のキーイベントと一致することを全組み合わせで確認する。
 * 対応表が片方だけずれたら必ず落ちる。
 */
#include "cdk_keys.h"
#include "t.h"

#include <stdio.h>

#define MAX_CAPTURED 16

typedef struct {
    int       n;
    cdk_key_t keys[MAX_CAPTURED];
} capture_t;

static void on_key(void *user, const cdk_key_t *k)
{
    capture_t *c = (capture_t *)user;
    if (c->n < MAX_CAPTURED) {
        c->keys[c->n++] = *k;
    }
}

static capture_t          CAP;
static cdk_keys_decoder_t DEC;

static void reset(void)
{
    memset(&CAP, 0, sizeof(CAP));
    cdk_keys_decode_init(&DEC, on_key, &CAP);
}

static void feed(const char *bytes)
{
    cdk_keys_decode_feed(&DEC, bytes, strlen(bytes));
}

static void eq_key(int i, uint16_t keycode, uint8_t mods, uint32_t cp, const char *what)
{
    t_checks++;
    if (i >= CAP.n) {
        t_failures++;
        printf("   FAIL %s: キーが %d 個しか来ていない\n", what, CAP.n);
        return;
    }
    const cdk_key_t *k = &CAP.keys[i];
    if (k->keycode != keycode || k->mods != mods || k->codepoint != cp) {
        t_failures++;
        printf("   FAIL %s: 期待 code=0x%02X mods=0x%02X cp=U+%04X / "
               "実際 code=0x%02X mods=0x%02X cp=U+%04X\n",
               what, keycode, mods, (unsigned)cp, k->keycode, k->mods,
               (unsigned)k->codepoint);
    }
}

int main(void)
{
    T_CASE("普通の文字");
    {
        reset();
        feed("abc");
        T_EQ_INT(CAP.n, 3);
        eq_key(0, CDK_K_NONE, 0, 'a', "a");
        eq_key(1, CDK_K_NONE, 0, 'b', "b");
        eq_key(2, CDK_K_NONE, 0, 'c', "c");
    }

    T_CASE("UTF-8 は 1 キーにまとまる");
    {
        reset();
        feed("\xE3\x81\x82"); /* あ */
        T_EQ_INT(CAP.n, 1);
        eq_key(0, CDK_K_NONE, 0, 0x3042, "あ");
    }

    T_CASE("制御文字");
    {
        reset();
        feed("\r");
        eq_key(0, CDK_K_ENTER, 0, 0, "enter");
        reset();
        feed("\n");
        eq_key(0, CDK_K_ENTER, 0, 0, "lf も enter");
        reset();
        feed("\t");
        eq_key(0, CDK_K_TAB, 0, 0, "tab");
        reset();
        feed("\177");
        eq_key(0, CDK_K_BS, 0, 0, "del は backspace");
        reset();
        feed(" ");
        eq_key(0, CDK_K_SPACE, 0, 0, "space");
    }

    T_CASE("Ctrl + 文字");
    {
        reset();
        feed("\x03");
        eq_key(0, CDK_K_NONE, CDK_MOD_CTRL_BIT, 'c', "ctrl+c");
        reset();
        feed("\x01");
        eq_key(0, CDK_K_NONE, CDK_MOD_CTRL_BIT, 'a', "ctrl+a");
        reset();
    }

    T_CASE("Ctrl+Space (NUL) はゼロ終端に埋もれない");
    {
        reset();
        const uint8_t nul[] = {0x00};
        cdk_keys_decode_feed(&DEC, nul, sizeof(nul));
        eq_key(0, CDK_K_SPACE, CDK_MOD_CTRL_BIT, 0, "ctrl+space");
    }

    T_CASE("カーソルキー");
    {
        reset();
        feed("\033[A\033[B\033[C\033[D");
        T_EQ_INT(CAP.n, 4);
        eq_key(0, CDK_K_UP, 0, 0, "up");
        eq_key(1, CDK_K_DOWN, 0, 0, "down");
        eq_key(2, CDK_K_RIGHT, 0, 0, "right");
        eq_key(3, CDK_K_LEFT, 0, 0, "left");

        /* アプリケーションカーソルモードの SS3 形式 */
        reset();
        feed("\033OA\033OD");
        eq_key(0, CDK_K_UP, 0, 0, "up (SS3)");
        eq_key(1, CDK_K_LEFT, 0, 0, "left (SS3)");
    }

    T_CASE("修飾子つきカーソルキー");
    {
        reset();
        feed("\033[1;5C");
        eq_key(0, CDK_K_RIGHT, CDK_MOD_CTRL_BIT, 0, "ctrl+right");
        reset();
        feed("\033[1;2D");
        eq_key(0, CDK_K_LEFT, CDK_MOD_SHIFT_BIT, 0, "shift+left");
        reset();
        feed("\033[1;3A");
        eq_key(0, CDK_K_UP, CDK_MOD_ALT_BIT, 0, "alt+up");
    }

    T_CASE("編集キーとファンクションキー");
    {
        reset();
        feed("\033[2~\033[3~\033[5~\033[6~");
        eq_key(0, CDK_K_INSERT, 0, 0, "insert");
        eq_key(1, CDK_K_DELETE, 0, 0, "delete");
        eq_key(2, CDK_K_PGUP, 0, 0, "pgup");
        eq_key(3, CDK_K_PGDN, 0, 0, "pgdn");

        reset();
        feed("\033[H\033[F");
        eq_key(0, CDK_K_HOME, 0, 0, "home");
        eq_key(1, CDK_K_END, 0, 0, "end");

        reset();
        feed("\033OP\033[15~\033[24~");
        eq_key(0, CDK_K_F1, 0, 0, "f1");
        eq_key(1, CDK_K_F1 + 4, 0, 0, "f5");
        eq_key(2, CDK_K_F12, 0, 0, "f12");

        reset();
        feed("\033[Z");
        eq_key(0, CDK_K_TAB, CDK_MOD_SHIFT_BIT, 0, "shift+tab");
    }

    T_CASE("Alt は ESC 前置として戻る");
    {
        reset();
        feed("\033x");
        eq_key(0, CDK_K_NONE, CDK_MOD_ALT_BIT, 'x', "alt+x");
        reset();
        feed("\033\177");
        eq_key(0, CDK_K_BS, CDK_MOD_ALT_BIT, 0, "alt+backspace");
        reset();
        feed("\033\r");
        eq_key(0, CDK_K_ENTER, CDK_MOD_ALT_BIT, 0, "alt+enter");
    }

    T_CASE("ESC 単独は flush で確定する");
    {
        /* ここが decode の肝。ESC の次が来るまで Esc キーか
         * エスケープ列の始まりか決まらない。 */
        reset();
        feed("\033");
        T_EQ_INT(CAP.n, 0); /* まだ確定しない */
        cdk_keys_decode_flush(&DEC);
        T_EQ_INT(CAP.n, 1);
        eq_key(0, CDK_K_ESC, 0, 0, "esc");

        /* ESC ESC は 1 つ目が確定する */
        reset();
        feed("\033\033");
        T_EQ_INT(CAP.n, 1);
        eq_key(0, CDK_K_ESC, 0, 0, "esc esc の 1 つ目");
    }

    T_CASE("シーケンスが分割されても組み立てられる");
    {
        /* cdk.py type の 1 回の送信がエスケープ列の途中で切れることがある */
        reset();
        const char *seq = "\033[1;5C";
        for (const char *p = seq; *p; p++) {
            cdk_keys_decode_feed(&DEC, p, 1);
        }
        T_EQ_INT(CAP.n, 1);
        eq_key(0, CDK_K_RIGHT, CDK_MOD_CTRL_BIT, 0, "ctrl+right (1 バイトずつ)");

        /* UTF-8 も同様 */
        reset();
        const char *utf = "\xE3\x81\x82";
        for (const char *p = utf; *p; p++) {
            cdk_keys_decode_feed(&DEC, p, 1);
        }
        eq_key(0, CDK_K_NONE, 0, 0x3042, "あ (1 バイトずつ)");
    }

    T_CASE("知らないシーケンスは捨てるが後続は拾う");
    {
        reset();
        feed("\033[99;99w" "a");
        T_CHECK(DEC.stat_unknown > 0);
        eq_key(0, CDK_K_NONE, 0, 'a', "後続の a");
    }

    T_CASE("往復: encode -> decode で元に戻る");
    {
        /* 対応表が片方だけずれたらここで落ちる。 */
        static const uint16_t codes[] = {
            CDK_K_NONE, CDK_K_ENTER, CDK_K_ESC, CDK_K_BS, CDK_K_TAB, CDK_K_SPACE,
            CDK_K_UP, CDK_K_DOWN, CDK_K_LEFT, CDK_K_RIGHT,
            CDK_K_HOME, CDK_K_END, CDK_K_INSERT, CDK_K_DELETE,
            CDK_K_PGUP, CDK_K_PGDN,
            CDK_K_F1, CDK_K_F1 + 3, CDK_K_F1 + 4, CDK_K_F1 + 5, CDK_K_F12,
        };
        static const uint8_t modsets[] = {
            0, CDK_MOD_CTRL_BIT, CDK_MOD_SHIFT_BIT, CDK_MOD_ALT_BIT,
        };
        static const uint32_t cps[] = {'a', 'Z', '0', 0x3042, 0x00E9};

        int checked = 0;
        for (size_t ci = 0; ci < sizeof(codes) / sizeof(codes[0]); ci++) {
            for (size_t mi = 0; mi < sizeof(modsets); mi++) {
                const uint16_t code = codes[ci];
                const uint8_t mods = modsets[mi];

                /* 文字キーは codepoint 側で回す */
                const size_t ncp = (code == CDK_K_NONE) ? sizeof(cps) / sizeof(cps[0]) : 1;
                for (size_t pi = 0; pi < ncp; pi++) {
                    const uint32_t cp = (code == CDK_K_NONE) ? cps[pi] : 0;

                    /* 古典的な端末の入力符号化では表現できない組み合わせがある。
                     * 実際の端末でも Ctrl+Enter が効かないのはこれが理由。
                     * 往復しないのは実装の不足ではなくプロトコルの限界なので、
                     * ここで明示的に除外する。
                     *
                     *  - 文字キー + Shift : 大文字で表すので修飾子が消える
                     *  - 文字キー + Ctrl  : a-z 以外は対応する制御コードが無い
                     *  - Enter/Esc/BS/Space + Ctrl/Shift : 単一バイトしか送れない
                     *  - Tab + Ctrl       : 同上（Shift+Tab だけ CSI Z がある）
                     *  - Alt + Esc        : Esc を 2 回押したのと区別できない
                     */
                    const bool is_char = (code == CDK_K_NONE);
                    const bool has_ctrl = (mods & CDK_MOD_CTRL_BIT) != 0;
                    const bool has_shift = (mods & CDK_MOD_SHIFT_BIT) != 0;
                    const bool has_alt = (mods & CDK_MOD_ALT_BIT) != 0;

                    if (is_char && has_shift) {
                        continue;
                    }
                    if (is_char && has_ctrl && !(cp >= 'a' && cp <= 'z')) {
                        continue;
                    }
                    if ((code == CDK_K_ENTER || code == CDK_K_ESC || code == CDK_K_BS) &&
                        (has_ctrl || has_shift)) {
                        continue;
                    }
                    if (code == CDK_K_SPACE && has_shift) {
                        continue;
                    }
                    if (code == CDK_K_TAB && has_ctrl) {
                        continue;
                    }
                    if (code == CDK_K_ESC && has_alt) {
                        continue;
                    }

                    const cdk_key_t in = {code, mods, cp};
                    uint8_t bytes[16];
                    const size_t n = cdk_keys_encode(&in, false, bytes, sizeof(bytes));
                    if (n == 0) {
                        continue; /* encode 側が送らないキー */
                    }

                    reset();
                    cdk_keys_decode_feed(&DEC, bytes, n);
                    cdk_keys_decode_flush(&DEC);

                    t_checks++;
                    if (CAP.n != 1) {
                        t_failures++;
                        printf("   FAIL 往復 code=0x%02X mods=0x%02X cp=U+%04X: "
                               "キーが %d 個\n", code, mods, (unsigned)cp, CAP.n);
                        continue;
                    }
                    const cdk_key_t *out = &CAP.keys[0];
                    const uint32_t want_cp = cp;
                    if (out->keycode != code || out->mods != mods ||
                        out->codepoint != want_cp) {
                        t_failures++;
                        printf("   FAIL 往復: 入 code=0x%02X mods=0x%02X cp=U+%04X / "
                               "出 code=0x%02X mods=0x%02X cp=U+%04X\n",
                               code, mods, (unsigned)want_cp, out->keycode, out->mods,
                               (unsigned)out->codepoint);
                    }
                    checked++;
                }
            }
        }
        printf("   往復を確認した組み合わせ: %d\n", checked);
        T_CHECK(checked > 40);
    }

    return t_report("test_keys_decode");
}
