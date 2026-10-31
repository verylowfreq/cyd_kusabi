/* cdk_term の回帰テスト。
 *
 * ここが緑でないと実機で何が起きているか切り分けられなくなる。
 * CLI が実際に使うシーケンスを中心に固める。
 */
#include "cdk_term.h"
#include "t.h"

#include <stdio.h>

#define COLS 20
#define ROWS 6

static cdk_term_t T;
static uint8_t mem[64 * 1024];
static char linebuf[COLS * 4 + 1];

static char reply_buf[128];
static size_t reply_len;

static void on_reply(void *user, const uint8_t *data, size_t len)
{
    (void)user;
    if (reply_len + len < sizeof(reply_buf)) {
        memcpy(reply_buf + reply_len, data, len);
        reply_len += len;
        reply_buf[reply_len] = '\0';
    }
}

/* 履歴つきの端末。lines 行ぶんの環を付ける。 */
static uint8_t histmem[COLS * 16 * sizeof(cdk_cell_t)];
static void setup_hist(int lines);

static void setup(void)
{
    T_CHECK(cdk_term_init(&T, COLS, ROWS, mem, sizeof(mem)));
    cdk_term_set_reply(&T, on_reply, NULL);
    reply_len = 0;
    reply_buf[0] = '\0';
}

static void setup_hist(int lines)
{
    setup();
    T_CHECK(cdk_term_set_history(&T, histmem, sizeof(histmem), lines));
}

static void feed(const char *s)
{
    cdk_term_feed(&T, s, strlen(s));
}

static const char *line(int y)
{
    cdk_term_row_text(&T, y, linebuf, sizeof(linebuf));
    return linebuf;
}

static void eq_line(int y, const char *want)
{
    const char *got = line(y);
    t_checks++;
    if (strcmp(got, want) != 0) {
        t_failures++;
        printf("   FAIL 行 %d\n     期待 [%s]\n     実際 [%s]\n", y, want, got);
    }
}

/* 末尾の空白を詰めた行 */
static const char *trimmed(int y)
{
    cdk_term_row_text(&T, y, linebuf, sizeof(linebuf));
    size_t n = strlen(linebuf);
    while (n > 0 && linebuf[n - 1] == ' ') {
        linebuf[--n] = '\0';
    }
    return linebuf;
}

static void eq_trim(int y, const char *want)
{
    const char *got = trimmed(y);
    t_checks++;
    if (strcmp(got, want) != 0) {
        t_failures++;
        printf("   FAIL 行 %d\n     期待 [%s]\n     実際 [%s]\n", y, want, got);
    }
}

int main(void)
{
    T_CASE("初期化");
    {
        T_CHECK(cdk_term_mem_size(COLS, ROWS) > 0);
        T_CHECK(cdk_term_mem_size(0, ROWS) == 0);
        cdk_term_t tmp;
        T_CHECK(!cdk_term_init(&tmp, COLS, ROWS, mem, 8)); /* メモリ不足 */
        setup();
        T_EQ_INT(T.cols, COLS);
        T_EQ_INT(T.rows, ROWS);
        T_EQ_INT(T.cx, 0);
        T_EQ_INT(T.cy, 0);
        T_EQ_INT(T.top, 0);
        T_EQ_INT(T.bot, ROWS - 1);
        eq_line(0, "                    ");
    }

    T_CASE("素の文字出力");
    {
        setup();
        feed("hello");
        eq_trim(0, "hello");
        T_EQ_INT(T.cx, 5);
        T_EQ_INT(T.cy, 0);
    }

    T_CASE("CR / LF / BS / TAB");
    {
        setup();
        feed("abc\r\ndef");
        eq_trim(0, "abc");
        eq_trim(1, "def");
        T_EQ_INT(T.cy, 1);

        setup();
        feed("abc\b\bX");
        eq_trim(0, "aXc");

        setup();
        feed("a\tb\tc");
        eq_trim(0, "a       b       c");

        /* LF は桁を動かさない (CR は別) */
        setup();
        feed("abc\ndef");
        eq_trim(0, "abc");
        eq_trim(1, "   def");
    }

    T_CASE("遅延折返し");
    {
        /* 右端に文字を書いた時点では折り返さない。
         * ここを間違えると CLI の行再描画が 1 行ずつずれる。 */
        setup();
        feed("12345678901234567890"); /* ちょうど 20 桁 */
        eq_line(0, "12345678901234567890");
        T_EQ_INT(T.cy, 0);
        T_EQ_INT(T.cx, COLS - 1);
        T_CHECK(T.wrap_pending);

        feed("X"); /* ここで初めて折り返す */
        T_EQ_INT(T.cy, 1);
        eq_trim(1, "X");

        /* 保留中に CR が来たら折り返さない */
        setup();
        feed("12345678901234567890\rY");
        eq_line(0, "Y2345678901234567890");
        T_EQ_INT(T.cy, 0);

        /* 保留中に BS が来たら保留を解くだけ */
        setup();
        feed("12345678901234567890\bZ");
        eq_line(0, "1234567890123456789Z");
        T_EQ_INT(T.cy, 0);
    }

    T_CASE("折返し禁止 (DECAWM off)");
    {
        setup();
        feed("\033[?7l");
        feed("123456789012345678901234"); /* 20 桁を超える */
        T_EQ_INT(T.cy, 0);
        eq_line(0, "12345678901234567894");
    }

    T_CASE("全角文字は 2 セル");
    {
        setup();
        feed("あい");
        T_EQ_INT(T.cx, 4);
        eq_trim(0, "あい");
        T_CHECK((cdk_term_cell(&T, 0, 0)->attr & CDK_ATTR_WIDE_LEAD) != 0);
        T_CHECK((cdk_term_cell(&T, 1, 0)->attr & CDK_ATTR_WIDE_TAIL) != 0);
        T_EQ_U32(cdk_term_cell(&T, 0, 0)->cp, 0x3042u);
        T_EQ_U32(cdk_term_cell(&T, 1, 0)->cp, 0u);
    }

    T_CASE("全角が右端 1 セルに入らないときは次行へ送る");
    {
        setup();
        feed("1234567890123456789"); /* 19 桁。残り 1 セル */
        T_EQ_INT(T.cx, 19);
        feed("あ");
        T_EQ_INT(T.cy, 1);
        eq_trim(1, "あ");
        /* 19 桁目は空白になる（半端に埋めない） */
        T_EQ_U32(cdk_term_cell(&T, 19, 0)->cp, 0u);
    }

    T_CASE("全角の片側を上書きすると相方も消える");
    {
        setup();
        feed("あい");
        feed("\033[1;1H");  /* 1 桁目へ */
        feed("X");
        /* あ が消えて X + 空白 になる */
        T_EQ_U32(cdk_term_cell(&T, 0, 0)->cp, (uint32_t)'X');
        T_EQ_U32(cdk_term_cell(&T, 1, 0)->cp, 0u);
        T_CHECK((cdk_term_cell(&T, 1, 0)->attr & CDK_ATTR_WIDE_TAIL) == 0);
        eq_trim(0, "X い");
    }

    T_CASE("カーソル移動");
    {
        setup();
        feed("\033[3;5H");
        T_EQ_INT(T.cy, 2);
        T_EQ_INT(T.cx, 4);
        feed("\033[A");       /* CUU */
        T_EQ_INT(T.cy, 1);
        feed("\033[2B");      /* CUD */
        T_EQ_INT(T.cy, 3);
        feed("\033[3C");      /* CUF */
        T_EQ_INT(T.cx, 7);
        feed("\033[2D");      /* CUB */
        T_EQ_INT(T.cx, 5);
        feed("\033[10G");     /* CHA */
        T_EQ_INT(T.cx, 9);
        feed("\033[2d");      /* VPA */
        T_EQ_INT(T.cy, 1);
        T_EQ_INT(T.cx, 9);
        feed("\033[E");       /* CNL */
        T_EQ_INT(T.cy, 2);
        T_EQ_INT(T.cx, 0);
        feed("\033[F");       /* CPL */
        T_EQ_INT(T.cy, 1);

        /* 画面外へは出ない */
        feed("\033[99;99H");
        T_EQ_INT(T.cy, ROWS - 1);
        T_EQ_INT(T.cx, COLS - 1);
        feed("\033[99A");
        T_EQ_INT(T.cy, 0);
        feed("\033[99D");
        T_EQ_INT(T.cx, 0);

        /* 引数省略は 1 扱い */
        setup();
        feed("\033[H");
        T_EQ_INT(T.cx, 0);
        T_EQ_INT(T.cy, 0);
    }

    T_CASE("ED 画面消去");
    {
        setup();
        feed("aaaa\r\nbbbb\r\ncccc");
        feed("\033[2;3H\033[J"); /* カーソル以降 */
        eq_trim(0, "aaaa");
        eq_trim(1, "bb");
        eq_trim(2, "");

        setup();
        feed("aaaa\r\nbbbb\r\ncccc");
        feed("\033[2;3H\033[1J"); /* 先頭からカーソルまで */
        eq_trim(0, "");
        eq_trim(1, "   b");
        eq_trim(2, "cccc");

        setup();
        feed("aaaa\r\nbbbb");
        feed("\033[2J");
        eq_trim(0, "");
        eq_trim(1, "");
    }

    T_CASE("EL 行消去");
    {
        setup();
        feed("abcdef\033[1;3H\033[K");
        eq_trim(0, "ab");

        setup();
        feed("abcdef\033[1;3H\033[1K");
        eq_trim(0, "   def");

        setup();
        feed("abcdef\033[1;3H\033[2K");
        eq_trim(0, "");
    }

    T_CASE("ICH / DCH / ECH");
    {
        setup();
        feed("abcdef\033[1;3H\033[2@"); /* 2 文字挿入 */
        eq_trim(0, "ab  cdef");

        setup();
        feed("abcdef\033[1;3H\033[2P"); /* 2 文字削除 */
        eq_trim(0, "abef");

        setup();
        feed("abcdef\033[1;3H\033[2X"); /* 2 文字消去 */
        eq_trim(0, "ab  ef");
    }

    T_CASE("IL / DL");
    {
        setup();
        feed("1\r\n2\r\n3\r\n4");
        feed("\033[2;1H\033[L"); /* 2 行目に 1 行挿入 */
        eq_trim(0, "1");
        eq_trim(1, "");
        eq_trim(2, "2");
        eq_trim(3, "3");

        setup();
        feed("1\r\n2\r\n3\r\n4");
        feed("\033[2;1H\033[M"); /* 2 行目を削除 */
        eq_trim(0, "1");
        eq_trim(1, "3");
        eq_trim(2, "4");
    }

    T_CASE("スクロール領域");
    {
        setup();
        feed("1\r\n2\r\n3\r\n4\r\n5\r\n6");
        feed("\033[2;4r");   /* 2-4 行目をスクロール領域に */
        T_EQ_INT(T.top, 1);
        T_EQ_INT(T.bot, 3);
        T_EQ_INT(T.cy, 0);   /* DECSTBM はカーソルをホームへ */

        feed("\033[4;1H\n"); /* 領域の最下行で改行 → 領域内だけスクロール */
        eq_trim(0, "1");
        eq_trim(1, "3");
        eq_trim(2, "4");
        eq_trim(3, "");
        eq_trim(4, "5");
        eq_trim(5, "6");

        /* 不正な指定は全画面に戻る */
        feed("\033[5;2r");
        T_EQ_INT(T.top, 0);
        T_EQ_INT(T.bot, ROWS - 1);
    }

    T_CASE("SU / SD / RI");
    {
        setup();
        feed("1\r\n2\r\n3");
        feed("\033[S"); /* 上へ 1 行 */
        eq_trim(0, "2");
        eq_trim(1, "3");

        setup();
        feed("1\r\n2\r\n3");
        feed("\033[T"); /* 下へ 1 行 */
        eq_trim(0, "");
        eq_trim(1, "1");
        eq_trim(2, "2");

        setup();
        feed("1\r\n2\r\n3");
        feed("\033[1;1H\033M"); /* RI 最上行 → 下へずれる */
        eq_trim(0, "");
        eq_trim(1, "1");
    }

    T_CASE("最下行での改行はスクロールする");
    {
        setup();
        feed("1\r\n2\r\n3\r\n4\r\n5\r\n6");
        T_EQ_INT(T.cy, ROWS - 1);
        feed("\r\n7");
        eq_trim(0, "2");
        eq_trim(ROWS - 1, "7");
    }

    T_CASE("SGR 基本色と属性");
    {
        setup();
        feed("\033[31mR");
        T_EQ_U32(cdk_term_cell(&T, 0, 0)->fg, CDK_COLOR_IDX(1));
        feed("\033[44mB");
        T_EQ_U32(cdk_term_cell(&T, 1, 0)->bg, CDK_COLOR_IDX(4));
        feed("\033[1mX");
        T_CHECK((cdk_term_cell(&T, 2, 0)->attr & CDK_ATTR_BOLD) != 0);
        feed("\033[22mY");
        T_CHECK((cdk_term_cell(&T, 3, 0)->attr & CDK_ATTR_BOLD) == 0);
        feed("\033[4;7mZ");
        T_CHECK((cdk_term_cell(&T, 4, 0)->attr & CDK_ATTR_UNDERLINE) != 0);
        T_CHECK((cdk_term_cell(&T, 4, 0)->attr & CDK_ATTR_REVERSE) != 0);
        feed("\033[0mW");
        T_EQ_U32(cdk_term_cell(&T, 5, 0)->fg, CDK_COLOR_DEFAULT);
        T_EQ_U32(cdk_term_cell(&T, 5, 0)->bg, CDK_COLOR_DEFAULT);
        T_EQ_INT(cdk_term_cell(&T, 5, 0)->attr, 0);

        /* 明るい色 */
        feed("\033[93mA\033[103mB");
        T_EQ_U32(cdk_term_cell(&T, 6, 0)->fg, CDK_COLOR_IDX(11));
        T_EQ_U32(cdk_term_cell(&T, 7, 0)->bg, CDK_COLOR_IDX(11));

        /* 引数なしの SGR はリセット */
        feed("\033[mC");
        T_EQ_U32(cdk_term_cell(&T, 8, 0)->fg, CDK_COLOR_DEFAULT);
    }

    T_CASE("SGR 256 色と truecolor");
    {
        setup();
        feed("\033[38;5;196mA");
        T_EQ_U32(cdk_term_cell(&T, 0, 0)->fg, CDK_COLOR_IDX(196));
        feed("\033[48;5;21mB");
        T_EQ_U32(cdk_term_cell(&T, 1, 0)->bg, CDK_COLOR_IDX(21));

        feed("\033[38;2;10;20;30mC");
        T_EQ_U32(cdk_term_cell(&T, 2, 0)->fg, CDK_COLOR_RGB(10, 20, 30));
        feed("\033[48;2;255;128;0mD");
        T_EQ_U32(cdk_term_cell(&T, 3, 0)->bg, CDK_COLOR_RGB(255, 128, 0));

        /* 続けて別の SGR が来ても取りこぼさない */
        setup();
        feed("\033[1;38;2;1;2;3;4mA");
        T_EQ_U32(cdk_term_cell(&T, 0, 0)->fg, CDK_COLOR_RGB(1, 2, 3));
        T_CHECK((cdk_term_cell(&T, 0, 0)->attr & CDK_ATTR_BOLD) != 0);
        T_CHECK((cdk_term_cell(&T, 0, 0)->attr & CDK_ATTR_UNDERLINE) != 0);
    }

    T_CASE("SGR truecolor の ':' 形式");
    {
        setup();
        feed("\033[38:2::10:20:30mA"); /* colorspace 付き */
        T_EQ_U32(cdk_term_cell(&T, 0, 0)->fg, CDK_COLOR_RGB(10, 20, 30));

        setup();
        feed("\033[38:2:40:50:60mB"); /* colorspace 無し */
        T_EQ_U32(cdk_term_cell(&T, 0, 0)->fg, CDK_COLOR_RGB(40, 50, 60));

        setup();
        feed("\033[38:5:196mC");
        T_EQ_U32(cdk_term_cell(&T, 0, 0)->fg, CDK_COLOR_IDX(196));
    }

    T_CASE("消去は現在の背景色で塗る");
    {
        setup();
        feed("\033[44m\033[2J");
        T_EQ_U32(cdk_term_cell(&T, 5, 3)->bg, CDK_COLOR_IDX(4));
        /* 背景を戻してから消すと既定色になる */
        feed("\033[49m\033[2J");
        T_EQ_U32(cdk_term_cell(&T, 5, 3)->bg, CDK_COLOR_DEFAULT);
    }

    T_CASE("カーソルの保存と復元");
    {
        setup();
        feed("\033[3;5H\033[31m\0337");   /* DECSC */
        feed("\033[1;1H\033[32mX");
        feed("\0338");                     /* DECRC */
        T_EQ_INT(T.cy, 2);
        T_EQ_INT(T.cx, 4);
        T_EQ_U32(T.fg, CDK_COLOR_IDX(1));

        /* CSI s / u */
        setup();
        feed("\033[2;3H\033[s\033[5;5H\033[u");
        T_EQ_INT(T.cy, 1);
        T_EQ_INT(T.cx, 2);
    }

    T_CASE("代替画面 (1049)");
    {
        setup();
        feed("main\r\nscreen");
        feed("\033[?1049h");
        eq_trim(0, ""); /* 代替画面は空 */
        T_CHECK(T.alt_active);
        feed("alt");
        eq_trim(0, "alt");
        feed("\033[?1049l");
        T_CHECK(!T.alt_active);
        eq_trim(0, "main"); /* 元の画面が戻る */
        eq_trim(1, "screen");
    }

    T_CASE("DA / DSR の応答");
    {
        setup();
        feed("\033[c");
        T_CHECK(strcmp(reply_buf, "\033[?62;22c") == 0);

        reply_len = 0;
        reply_buf[0] = '\0';
        feed("\033[3;7H\033[6n");
        T_CHECK(strcmp(reply_buf, "\033[3;7R") == 0);

        reply_len = 0;
        reply_buf[0] = '\0';
        feed("\033[5n");
        T_CHECK(strcmp(reply_buf, "\033[0n") == 0);
    }

    T_CASE("OSC でタイトルを拾う");
    {
        setup();
        feed("\033]0;my title\007");
        T_CHECK(strcmp(T.title, "my title") == 0);

        feed("\033]2;second\033\\"); /* ST 終端 */
        T_CHECK(strcmp(T.title, "second") == 0);

        /* 知らない OSC は捨てるが画面は壊さない */
        feed("\033]52;c;BASE64DATA\007abc");
        eq_trim(0, "abc");
    }

    T_CASE("DEC 特殊グラフィクス");
    {
        setup();
        feed("\033(0qx\033(B");
        T_EQ_U32(cdk_term_cell(&T, 0, 0)->cp, 0x2500u); /* q → ─ */
        T_EQ_U32(cdk_term_cell(&T, 1, 0)->cp, 0x2502u); /* x → │ */
        feed("q");
        T_EQ_U32(cdk_term_cell(&T, 2, 0)->cp, (uint32_t)'q'); /* 戻っている */
    }

    T_CASE("挿入モード (IRM)");
    {
        setup();
        feed("abcd\033[1;1H\033[4hXY");
        eq_trim(0, "XYabcd");
        feed("\033[4l");
        feed("\033[1;1HZ");
        eq_trim(0, "ZYabcd");
    }

    T_CASE("タブストップ");
    {
        setup();
        setup();
        feed("[1;1H	a");
        T_EQ_INT(T.cx, 9); /* 既定のタブは 8 桁ごと。index 8 に文字を書いて cx=9 */

        setup();
        feed("[1;9H[0g"); /* index 8 のタブを消す */
        feed("[1;1H	a");
        T_EQ_INT(T.cx, 17); /* 次のタブ index 16 まで飛ぶ */

        setup();
        feed("[3g"); /* 全部消す */
        feed("	a");
        T_EQ_INT(T.cx, COLS - 1); /* タブが無いので行末で止まる */
    }

    T_CASE("DECALN");
    {
        setup();
        feed("\033#8");
        eq_line(0, "EEEEEEEEEEEEEEEEEEEE");
        eq_line(ROWS - 1, "EEEEEEEEEEEEEEEEEEEE");
        T_EQ_INT(T.cx, 0);
        T_EQ_INT(T.cy, 0);
    }

    T_CASE("知らないシーケンスで画面が壊れない");
    {
        setup();
        feed("A\033[999yB");           /* 知らない終端 */
        feed("\033[?9999hC");          /* 知らない私用モード */
        feed("\033Pfoo\033\\D");       /* DCS は読み捨て */
        feed("\033_APC\033\\E");       /* APC も読み捨て */
        feed("\033[1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17;18;19;20mF");
        eq_trim(0, "ABCDEF");
        T_CHECK(T.stat_unknown > 0);
    }

    T_CASE("分割された入力でも壊れない");
    {
        setup();
        /* エスケープシーケンスと UTF-8 を 1 バイトずつ食わせる */
        const char *s = "\033[31mあ\033[0mA\033[2;1Hい";
        for (const char *p = s; *p; p++) {
            cdk_term_feed(&T, p, 1);
        }
        T_EQ_U32(cdk_term_cell(&T, 0, 0)->cp, 0x3042u);
        T_EQ_U32(cdk_term_cell(&T, 0, 0)->fg, CDK_COLOR_IDX(1));
        T_EQ_U32(cdk_term_cell(&T, 2, 0)->cp, (uint32_t)'A');
        T_EQ_U32(cdk_term_cell(&T, 2, 0)->fg, CDK_COLOR_DEFAULT);
        T_EQ_U32(cdk_term_cell(&T, 0, 1)->cp, 0x3044u);
    }

    T_CASE("ダーティ行の追跡");
    {
        setup();
        cdk_term_clear_dirty(&T);
        T_CHECK(!cdk_term_line_dirty(&T, 0));
        feed("\033[3;1Hx");
        T_CHECK(cdk_term_line_dirty(&T, 2));
        T_CHECK(!cdk_term_line_dirty(&T, 1));

        cdk_term_clear_dirty(&T);
        feed("\033[2J");
        for (int y = 0; y < ROWS; y++) {
            T_CHECK(cdk_term_line_dirty(&T, y));
        }
    }

    T_CASE("RIS (ESC c) で初期状態に戻る");
    {
        setup();
        feed("\033[44m\033[?1049h\033[2;5rabc");
        feed("\033c");
        T_CHECK(!T.alt_active);
        T_EQ_INT(T.top, 0);
        T_EQ_INT(T.bot, ROWS - 1);
        T_EQ_U32(T.bg, CDK_COLOR_DEFAULT);
        eq_trim(0, "");
    }

    T_CASE("モードのフラグ");
    {
        setup();
        feed("\033[?25l");
        T_CHECK(!T.cursor_visible);
        feed("\033[?25h");
        T_CHECK(T.cursor_visible);
        feed("\033[?2004h");
        T_CHECK(T.bracketed_paste);
        feed("\033[?1h");
        T_CHECK(T.app_cursor_keys);
        feed("\033[?1000h\033[?1006h");
        T_EQ_INT(T.mouse_mode, 1000);
        T_EQ_INT(T.mouse_enc, 1006);
    }

    T_CASE("Shift+PgUp の行き先: 形式だけではホイールにしない");
    {
        setup();
        T_EQ_INT(cdk_term_page_route(&T), CDK_PAGE_LOCAL);
        feed("\033[?1006h"); /* 形式だけ */
        T_EQ_INT(T.mouse_mode, 0);
        T_EQ_INT(cdk_term_page_route(&T), CDK_PAGE_LOCAL);
        uint8_t b[32];
        T_EQ_INT((int)cdk_term_wheel_report(&T, true, b, sizeof(b)), 0);
    }

    T_CASE("Shift+PgUp の行き先: 報告を頼まれたらホイール、落としたら戻る");
    {
        setup();
        feed("\033[?1002h\033[?1006h");
        T_EQ_INT(cdk_term_page_route(&T), CDK_PAGE_WHEEL);
        feed("\033[?1049h"); /* 代替画面でも報告が優先 */
        T_EQ_INT(cdk_term_page_route(&T), CDK_PAGE_WHEEL);
        feed("\033[?1002l");
        T_EQ_INT(T.mouse_mode, 0);
        T_EQ_INT(T.mouse_enc, 1006); /* 形式は残る */
        T_EQ_INT(cdk_term_page_route(&T), CDK_PAGE_KEY);
        feed("\033[?1049l");
        T_EQ_INT(cdk_term_page_route(&T), CDK_PAGE_LOCAL);
        feed("\033[?1006l");
        T_EQ_INT(T.mouse_enc, 0);
    }

    T_CASE("Shift+PgUp の行き先: 報告の無い代替画面は PgUp キー");
    {
        setup();
        feed("\033[?1049h");
        T_EQ_INT(cdk_term_page_route(&T), CDK_PAGE_KEY);
        feed("\033c"); /* RIS で全部戻る */
        T_EQ_INT(cdk_term_page_route(&T), CDK_PAGE_LOCAL);
        feed("\033[?1000h");
        feed("\033c");
        T_EQ_INT(T.mouse_mode, 0);
    }

    T_CASE("ホイールの報告: 形式ごとのバイト列（位置は横が中央、縦が上から 1/4）");
    {
        /* 20x6 なので (11, 1)。6 / 4 = 1。 */
        uint8_t b[32];
        setup();
        feed("\033[?1000h\033[?1006h");
        size_t n = cdk_term_wheel_report(&T, true, b, sizeof(b));
        T_EQ_INT((int)n, 11);
        T_EQ_MEM(b, "\033[<64;11;1M", n);
        n = cdk_term_wheel_report(&T, false, b, sizeof(b));
        T_EQ_MEM(b, "\033[<65;11;1M", n);

        setup();
        feed("\033[?1000h\033[?1015h");
        n = cdk_term_wheel_report(&T, true, b, sizeof(b));
        T_EQ_INT((int)n, 10);
        T_EQ_MEM(b, "\033[96;11;1M", n);

        setup();
        feed("\033[?1000h"); /* 既定の形式: ESC [ M と 32 足した 3 バイト */
        n = cdk_term_wheel_report(&T, false, b, sizeof(b));
        T_EQ_INT((int)n, 6);
        T_EQ_MEM(b, "\033[M\x61\x2b\x21", n); /* 32+65, 32+11, 32+1 */
    }

    T_CASE("ホイールの報告: 40x12（2 倍表示）では 3 行目。入力欄の上に当てない");
    {
        /* codex は入力欄とその上 2 行ほどではホイールに反応しない。
         * 中央（7 行目）だとそこに入って効かなかった（2026-10-31）。 */
        static cdk_term_t big;
        static uint8_t bmem[64 * 1024];
        uint8_t b[32];
        T_CHECK(cdk_term_init(&big, 40, 12, bmem, sizeof(bmem)));
        cdk_term_feed(&big, "\033[?1003h\033[?1006h", 16);
        const size_t n = cdk_term_wheel_report(&big, true, b, sizeof(b));
        T_EQ_MEM(b, "\033[<64;21;3M", n);
    }

    T_CASE("ホイールの報告: 入らないときは書かない");
    {
        uint8_t b[32];

        setup();
        feed("\033[?1000h\033[?1006h");
        T_EQ_INT((int)cdk_term_wheel_report(&T, true, b, 5), 0); /* 入らない */
    }

    T_CASE("ホイールの段数は行数 / 3（最低 1）");
    {
        setup();
        T_EQ_INT(cdk_term_wheel_steps(&T), 2); /* 6 行 */
        static cdk_term_t small;
        static uint8_t smem[8 * 1024];
        T_CHECK(cdk_term_init(&small, 10, 2, smem, sizeof(smem)));
        T_EQ_INT(cdk_term_wheel_steps(&small), 1);
    }

    T_CASE("ESC [ 3 J を数える（診断用）");
    {
        setup();
        T_EQ_U32(T.stat_clear_hist, 0u);
        feed("\033[2J");
        T_EQ_U32(T.stat_clear_hist, 0u);
        feed("\033[3J\033[3J");
        T_EQ_U32(T.stat_clear_hist, 2u);
    }

    T_CASE("原点モード (DECOM)");
    {
        setup();
        feed("\033[2;4r");    /* 2-4 行目 */
        feed("\033[?6h");     /* 原点モード on。カーソルは領域先頭へ */
        T_EQ_INT(T.cy, 1);
        feed("\033[1;1HX");
        T_EQ_INT(T.cy, 1);    /* 領域の 1 行目 = 画面の 2 行目 */
        eq_trim(1, "X");

        reply_len = 0;
        reply_buf[0] = '\0';
        feed("\033[6n");
        T_CHECK(strcmp(reply_buf, "\033[1;2R") == 0); /* 領域内での行番号 */
    }

    /* ---- スクロールバック ---- */

    T_CASE("履歴を付けなければ今までと同じ");
    {
        /* cdk_render（ホストの基準レンダラ）は set_history を呼ばない。
         * ここが崩れると shot --verify の比較が動く。 */
        setup();
        T_EQ_INT(cdk_term_hist_len(&T), 0);
        T_EQ_INT(cdk_term_view_offset(&T), 0);
        feed("1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7");
        T_EQ_INT(cdk_term_hist_len(&T), 0);
        eq_trim(0, "2");
        /* 遡ろうとしても動かない */
        T_EQ_INT(cdk_term_view_scroll(&T, 3), 0);
        eq_trim(0, "2");
    }

    T_CASE("最下行の改行で 1 行ずつ積まれる");
    {
        setup_hist(4);
        feed("1\r\n2\r\n3\r\n4\r\n5\r\n6"); /* ここまでで画面が埋まる */
        T_EQ_INT(cdk_term_hist_len(&T), 0);
        feed("\r\n7");
        T_EQ_INT(cdk_term_hist_len(&T), 1);
        feed("\r\n8");
        T_EQ_INT(cdk_term_hist_len(&T), 2);
    }

    T_CASE("環が一周しても壊れない");
    {
        setup_hist(3); /* 3 行しか持たない */
        feed("1\r\n2\r\n3\r\n4\r\n5\r\n6");
        for (int i = 7; i <= 20; i++) {
            char buf[16];
            snprintf(buf, sizeof buf, "\r\n%d", i);
            feed(buf);
        }
        T_EQ_INT(cdk_term_hist_len(&T), 3); /* 頭打ち */

        /* いちばん古い 3 行は 12/13/14（画面には 15..20 が残る） */
        cdk_term_view_scroll(&T, 3);
        T_EQ_INT(cdk_term_view_offset(&T), 3);
        eq_trim(0, "12");
        eq_trim(1, "13");
        eq_trim(2, "14");
        eq_trim(3, "15"); /* ここから画面 */
    }

    T_CASE("遡ったときの行の対応（つなぎ目）");
    {
        setup_hist(8);
        feed("1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7\r\n8\r\n9");
        /* 画面は 4..9、履歴は 1,2,3 */
        T_EQ_INT(cdk_term_hist_len(&T), 3);

        cdk_term_view_scroll(&T, 2);
        T_EQ_INT(cdk_term_view_offset(&T), 2);
        eq_trim(0, "2");          /* 履歴 */
        eq_trim(1, "3");          /* 履歴。ここがつなぎ目 */
        eq_trim(2, "4");          /* 画面の先頭 */
        eq_trim(ROWS - 1, "7");   /* 画面が 2 行ぶん上へずれて見える */
    }

    T_CASE("遡る量は履歴の長さで頭打ち");
    {
        setup_hist(8);
        feed("1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7"); /* 履歴は 1 行 */
        T_EQ_INT(cdk_term_hist_len(&T), 1);
        T_EQ_INT(cdk_term_view_scroll(&T, 100), 1); /* 動けたのは 1 行だけ */
        T_EQ_INT(cdk_term_view_offset(&T), 1);
        T_EQ_INT(cdk_term_view_scroll(&T, 100), 0); /* もう動けない */

        T_EQ_INT(cdk_term_view_scroll(&T, -100), -1);
        T_EQ_INT(cdk_term_view_offset(&T), 0);
        T_EQ_INT(cdk_term_view_scroll(&T, -100), 0);
    }

    T_CASE("遡っている最中に新着が来ても見ている行は動かない");
    {
        setup_hist(8);
        feed("1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7\r\n8");
        cdk_term_view_scroll(&T, 2);
        const char *before = trimmed(0);
        char keep[32];
        snprintf(keep, sizeof keep, "%s", before);

        feed("\r\n9"); /* 新しい行が積まれる */
        eq_trim(0, keep);            /* 見ている位置は同じ中身のまま */
        T_EQ_INT(cdk_term_view_offset(&T), 3); /* オフセットは 1 増える */
    }

    T_CASE("代替画面では積まれない");
    {
        setup_hist(8);
        feed("1\r\n2\r\n3\r\n4\r\n5\r\n6");
        feed("\033[?1049h"); /* 代替画面へ */
        const int before = cdk_term_hist_len(&T);
        feed("a\r\nb\r\nc\r\nd\r\ne\r\nf\r\ng\r\nh");
        T_EQ_INT(cdk_term_hist_len(&T), before); /* 増えていない */
        feed("\033[?1049l");
        T_EQ_INT(cdk_term_hist_len(&T), before);
    }

    T_CASE("代替画面では遡れない");
    {
        /* 履歴は主画面のもの。代替画面の上に描くと画面が壊れる。
         * **実機のエディタで踏んだ。** 0 を返すので呼び出し側はキーを
         * 消費せず、エディタの PgUp として流せる。 */
        setup_hist(8);
        feed("1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7\r\n8");
        T_CHECK(cdk_term_hist_len(&T) > 0);

        feed("\033[?1049h");
        T_EQ_INT(cdk_term_view_scroll(&T, 3), 0);
        T_EQ_INT(cdk_term_view_offset(&T), 0);

        /* 主画面へ戻れば遡れる */
        feed("\033[?1049l");
        T_CHECK(cdk_term_view_scroll(&T, 3) > 0);
    }

    T_CASE("代替画面へ入ると最下部へ戻る");
    {
        setup_hist(8);
        feed("1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7\r\n8");
        cdk_term_view_scroll(&T, 2);
        T_EQ_INT(cdk_term_view_offset(&T), 2);
        feed("\033[?1049h");
        T_EQ_INT(cdk_term_view_offset(&T), 0);
    }

    T_CASE("DL (CSI M) では積まれない");
    {
        /* DL は t->top を cy にすり替えて cdk__scroll_up を呼ぶ。
         * そこで積むと vi や less の行削除が履歴に流れ込む。 */
        setup_hist(8);
        feed("1\r\n2\r\n3\r\n4\r\n5\r\n6");
        const int before = cdk_term_hist_len(&T);
        feed("\033[1;1H");   /* 最上行へ */
        feed("\033[M");      /* 1 行削除 */
        T_EQ_INT(cdk_term_hist_len(&T), before);
        feed("\033[3;1H\033[2M"); /* 途中でも */
        T_EQ_INT(cdk_term_hist_len(&T), before);
    }

    T_CASE("スクロール領域が全画面でないときは積まれない");
    {
        setup_hist(8);
        feed("1\r\n2\r\n3\r\n4\r\n5\r\n6");
        const int before = cdk_term_hist_len(&T);

        /* 上端はそのままで下だけ狭める。top == 0 だけ見ていると誤って積む */
        feed("\033[1;4r");
        feed("\033[4;1H");   /* 領域の最下行 */
        feed("\r\na\r\nb\r\nc");
        T_EQ_INT(cdk_term_hist_len(&T), before);

        /* 領域を戻せばまた積む */
        feed("\033[r");
        feed("\033[6;1H\r\nx");
        T_CHECK(cdk_term_hist_len(&T) > before);
    }

    T_CASE("SU (CSI S) は積む");
    {
        setup_hist(8);
        feed("1\r\n2\r\n3\r\n4\r\n5\r\n6");
        const int before = cdk_term_hist_len(&T);
        feed("\033[2S");
        T_EQ_INT(cdk_term_hist_len(&T), before + 2);
        cdk_term_view_scroll(&T, 2);
        eq_trim(0, "1");
        eq_trim(1, "2");
    }

    T_CASE("遡っている最中の CSI 3J で履歴も view も消える");
    {
        /* 戻し忘れると cdk_term_row が環の外を読む。 */
        setup_hist(8);
        feed("1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7\r\n8");
        cdk_term_view_scroll(&T, 2);
        T_CHECK(cdk_term_hist_len(&T) > 0);
        T_EQ_INT(cdk_term_view_offset(&T), 2);

        feed("\033[3J");
        T_EQ_INT(cdk_term_hist_len(&T), 0);
        T_EQ_INT(cdk_term_view_offset(&T), 0);

        /* 2J は履歴を消さない */
        feed("a\r\nb\r\nc\r\nd\r\ne\r\nf\r\ng");
        const int n = cdk_term_hist_len(&T);
        T_CHECK(n > 0);
        feed("\033[2J");
        T_EQ_INT(cdk_term_hist_len(&T), n);
    }

    T_CASE("RIS は履歴も消す");
    {
        setup_hist(8);
        feed("1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7\r\n8");
        cdk_term_view_scroll(&T, 2);
        feed("\033c");
        T_EQ_INT(cdk_term_hist_len(&T), 0);
        T_EQ_INT(cdk_term_view_offset(&T), 0);
    }

    T_CASE("set_history を呼び直すと捨てる");
    {
        setup_hist(8);
        feed("1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7\r\n8");
        cdk_term_view_scroll(&T, 2);
        T_CHECK(cdk_term_hist_len(&T) > 0);

        /* 行数を変えると環の巻き方が変わる。古い view_off が残ると外を指す。 */
        T_CHECK(cdk_term_set_history(&T, histmem, sizeof histmem, 4));
        T_EQ_INT(cdk_term_hist_len(&T), 0);
        T_EQ_INT(cdk_term_view_offset(&T), 0);

        /* 外すこともできる */
        T_CHECK(cdk_term_set_history(&T, NULL, 0, 0));
        T_EQ_INT(cdk_term_hist_len(&T), 0);
        feed("\r\n9");
        T_EQ_INT(cdk_term_hist_len(&T), 0);
    }

    T_CASE("領域が足りなければ断る");
    {
        setup();
        T_EQ_U32((unsigned long)cdk_term_hist_mem_size(COLS, 4),
                 (unsigned long)(COLS * 4 * sizeof(cdk_cell_t)));
        T_EQ_U32((unsigned long)cdk_term_hist_mem_size(0, 4), 0ul);
        T_CHECK(!cdk_term_set_history(&T, histmem, 8, 4));
    }

    return t_report("test_term");
}
