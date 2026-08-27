/* エディタのテスト。
 *
 * **出力バイト列そのものは見ない。** エディタが吐いたエスケープ列を
 * cdk_term に流し込み、画面のセルの中身とカーソル位置で確かめる。
 * 描き方は実装の都合で変わってよいが、「画面がどう見えるか」は仕様だから。
 * ついでに、信用できる VT パーサを検証に使い回せる。
 */
#include "cdk_edit.h"
#include "cdk_term.h"
#include "t.h"

#include <stdio.h>
#include <stdlib.h>

#define COLS 20
#define ROWS 6 /* 本文 5 行 + ステータス 1 行 */

static cdk_term_t T;
static uint8_t    TERMMEM[64 * 1024];
static char       ARENA[4096];
static cdk_edit_t E;

static void on_out(void *user, const char *data, size_t len)
{
    (void)user;
    cdk_term_feed(&T, data, len);
}

static void setup(int cols, int rows)
{
    T_CHECK(cdk_term_init(&T, cols, rows, TERMMEM, sizeof(TERMMEM)));
    cdk_edit_init(&E, ARENA, sizeof(ARENA), on_out, NULL);
    cdk_edit_set_size(&E, cols, rows);
}

/* 画面 1 行を UTF-8 の文字列にして返す（末尾の空白は落とす）。 */
static const char *row_text(int row)
{
    static char out[512];
    size_t      used = 0;
    out[0] = '\0';

    const cdk_cell_t *line = cdk_term_row(&T, row);
    int               last = -1;
    for (int x = 0; x < T.cols; x++) {
        if (line[x].cp != 0 && line[x].cp != (uint32_t)' ') {
            last = x;
        }
    }
    for (int x = 0; x <= last; x++) {
        uint32_t cp = line[x].cp;
        if ((line[x].attr & CDK_ATTR_WIDE_TAIL) != 0u) {
            continue;
        }
        if (cp == 0) {
            cp = ' ';
        }
        /* UTF-8 に戻す */
        if (cp < 0x80u) {
            out[used++] = (char)cp;
        } else if (cp < 0x800u) {
            out[used++] = (char)(0xC0u | (cp >> 6));
            out[used++] = (char)(0x80u | (cp & 0x3Fu));
        } else {
            out[used++] = (char)(0xE0u | (cp >> 12));
            out[used++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            out[used++] = (char)(0x80u | (cp & 0x3Fu));
        }
        out[used] = '\0';
    }
    return out;
}

static void key(uint16_t code, uint8_t mods, uint32_t cp)
{
    const cdk_key_t k = {code, mods, cp};
    (void)cdk_edit_key(&E, &k);
    cdk_edit_draw(&E);
}

/* 文字列を 1 文字ずつ打つ（ASCII のみ）。 */
static void type(const char *s)
{
    for (const char *p = s; *p != '\0'; p++) {
        if (*p == '\n') {
            key(CDK_K_ENTER, 0, 0);
        } else if (*p == ' ') {
            key(CDK_K_SPACE, 0, 0);
        } else {
            key(0, 0, (uint32_t)(unsigned char)*p);
        }
    }
}

/* ------------------------------------------------------------------ */

static void test_basic(void)
{
    T_CASE("打った文字が画面に出る");
    setup(COLS, ROWS);
    cdk_edit_new(&E, "a.txt");
    type("hello");
    T_EQ_STR(row_text(0), "hello");
    T_EQ_INT(T.cx, 5);
    T_EQ_INT(T.cy, 0);

    T_CASE("改行で次の行へ");
    type("\nworld");
    T_EQ_STR(row_text(0), "hello");
    T_EQ_STR(row_text(1), "world");
    T_EQ_INT(T.cy, 1);
    T_EQ_INT(T.cx, 5);

    T_CASE("本文が取り出せる");
    size_t      n = 0;
    const char *txt = cdk_edit_text(&E, &n);
    T_EQ_INT(n, 11);
    T_CHECK(memcmp(txt, "hello\nworld", 11) == 0);

    T_CASE("BS は 1 文字消す");
    key(CDK_K_BS, 0, 0);
    T_EQ_STR(row_text(1), "worl");
    cdk_edit_text(&E, &n);
    T_EQ_INT(n, 10);

    T_CASE("ステータス行にファイル名と行番号");
    T_CHECK(strstr(row_text(ROWS - 1), "a.txt") != NULL);
    T_CHECK(strstr(row_text(ROWS - 1), "2/2") != NULL);
    T_CHECK(strstr(row_text(ROWS - 1), "*") != NULL); /* 変更あり */
}

static void test_move(void)
{
    T_CASE("左右の移動");
    setup(COLS, ROWS);
    cdk_edit_new(&E, "a.txt");
    type("abc");
    key(CDK_K_LEFT, 0, 0);
    T_EQ_INT(T.cx, 2);
    key(CDK_K_LEFT, 0, 0);
    key(CDK_K_LEFT, 0, 0);
    T_EQ_INT(T.cx, 0);
    key(CDK_K_LEFT, 0, 0); /* これ以上は動かない */
    T_EQ_INT(T.cx, 0);
    key(CDK_K_RIGHT, 0, 0);
    T_EQ_INT(T.cx, 1);

    T_CASE("上下の移動と桁の維持");
    setup(COLS, ROWS);
    cdk_edit_new(&E, "a.txt");
    type("abcdef\nxy\nlongerline");
    /* いま 3 行目の末尾 */
    T_EQ_INT(T.cy, 2);
    T_EQ_INT(T.cx, 10);

    key(CDK_K_UP, 0, 0);
    T_EQ_INT(T.cy, 1);
    T_EQ_INT(T.cx, 2); /* "xy" は 2 桁しかない */

    /* もう一度上がると、覚えていた桁に戻る */
    key(CDK_K_UP, 0, 0);
    T_EQ_INT(T.cy, 0);
    T_EQ_INT(T.cx, 6); /* "abcdef" の末尾まで */

    T_CASE("Home / End");
    key(CDK_K_HOME, 0, 0);
    T_EQ_INT(T.cx, 0);
    key(CDK_K_END, 0, 0);
    T_EQ_INT(T.cx, 6);
}

static void test_wrap(void)
{
    T_CASE("桁を超えたら折り返す");
    setup(10, ROWS);
    cdk_edit_new(&E, "a.txt");
    type("0123456789abc");
    T_EQ_STR(row_text(0), "0123456789");
    T_EQ_STR(row_text(1), "abc");
    T_EQ_INT(T.cy, 1);
    T_EQ_INT(T.cx, 3);

    T_CASE("折り返した先から上へ戻れる");
    key(CDK_K_UP, 0, 0);
    T_EQ_INT(T.cy, 0);
    T_EQ_INT(T.cx, 3);
    key(CDK_K_DOWN, 0, 0);
    T_EQ_INT(T.cy, 1);
    T_EQ_INT(T.cx, 3);

    T_CASE("折り返し行の Home は折り返し位置");
    key(CDK_K_HOME, 0, 0);
    T_EQ_INT(T.cy, 1);
    T_EQ_INT(T.cx, 0);
    /* 本文の位置としては 10 バイト目 */
    key(CDK_K_LEFT, 0, 0);
    T_EQ_INT(T.cy, 0);
    T_EQ_INT(T.cx, 9);
}

static void test_wide(void)
{
    /* 全角が右端に来たときが、折り返しとカーソルの計算がずれる場所。
     * 実際に「桁が 1 つ余って次の行へ送られる」ことを見る。 */
    T_CASE("全角は 2 桁。右端に 1 桁余ると次の行へ送る");
    setup(9, ROWS);
    cdk_edit_new(&E, "a.txt");
    cdk_edit_insert(&E, "abcdefg", 7); /* 7 桁 */
    cdk_edit_insert(&E, "あ", strlen("あ"));
    cdk_edit_draw(&E);
    /* 7 + 2 = 9 でちょうど収まる。行が右端まで埋まったので、
     * カーソルの居場所は次の行の頭になる。 */
    T_EQ_STR(row_text(0), "abcdefgあ");
    T_EQ_INT(T.cy, 1);
    T_EQ_INT(T.cx, 0);

    setup(9, ROWS);
    cdk_edit_new(&E, "a.txt");
    cdk_edit_insert(&E, "abcdefgh", 8); /* 8 桁 */
    cdk_edit_insert(&E, "あ", strlen("あ"));
    cdk_edit_draw(&E);
    /* 8 + 2 = 10 で入らない。「あ」は次の行 */
    T_EQ_STR(row_text(0), "abcdefgh");
    T_EQ_STR(row_text(1), "あ");
    T_EQ_INT(T.cy, 1);
    T_EQ_INT(T.cx, 2);

    T_CASE("全角の混じった行を上下に動く");
    setup(10, ROWS);
    cdk_edit_new(&E, "a.txt");
    cdk_edit_insert(&E, "あいう\nかきく", strlen("あいう\nかきく"));
    cdk_edit_draw(&E);
    T_EQ_STR(row_text(0), "あいう");
    T_EQ_STR(row_text(1), "かきく");
    T_EQ_INT(T.cy, 1);
    T_EQ_INT(T.cx, 6);

    key(CDK_K_UP, 0, 0);
    T_EQ_INT(T.cy, 0);
    T_EQ_INT(T.cx, 6); /* 「あいう」は 6 桁 */
    key(CDK_K_DOWN, 0, 0);
    T_EQ_INT(T.cy, 1);
    T_EQ_INT(T.cx, 6);

    T_CASE("全角の途中に飛び込まない");
    /* 3 行目の 5 桁目（「く」の右半分）に相当する位置へ下りると、
     * 「く」の左端に落ちる。 */
    setup(10, ROWS);
    cdk_edit_new(&E, "a.txt");
    cdk_edit_insert(&E, "abcde\nかきくけこ", strlen("abcde\nかきくけこ"));
    E.cursor = 5; /* 1 行目の末尾 */
    cdk_edit_draw(&E);
    T_EQ_INT(T.cx, 5);
    key(CDK_K_DOWN, 0, 0);
    T_EQ_INT(T.cy, 1);
    T_EQ_INT(T.cx, 4); /* 「かき」= 4 桁。「く」の左端 */
}

static void test_scroll(void)
{
    T_CASE("画面より下へ行くとスクロールする");
    setup(COLS, ROWS); /* 本文 5 行 */
    cdk_edit_new(&E, "a.txt");
    type("l1\nl2\nl3\nl4\nl5\nl6\nl7");
    /* 最後の行にいるので、上 4 行は l3..l7 */
    T_EQ_STR(row_text(0), "l3");
    T_EQ_STR(row_text(4), "l7");
    T_EQ_INT(T.cy, 4);

    T_CASE("上へ戻るとスクロールが戻る");
    for (int i = 0; i < 6; i++) {
        key(CDK_K_UP, 0, 0);
    }
    T_EQ_STR(row_text(0), "l1");
    T_EQ_INT(T.cy, 0);

    T_CASE("PgDn / PgUp");
    key(CDK_K_PGDN, 0, 0);
    T_EQ_INT(cdk_edit_line_no(&E), 5);
    key(CDK_K_PGUP, 0, 0);
    T_EQ_INT(cdk_edit_line_no(&E), 1);

    T_CASE("最終行で Down しても動かない");
    /* 「次の表示行があるか」の判断は next_row_start に一本化してある。
     * ここが緩むと、末尾に改行が無い行で Down が本文の末尾へ飛ぶ。
     * 見た目は End を押したのと同じなので気づきにくい。 */
    setup(COLS, ROWS);
    cdk_edit_new(&E, "a.txt");
    type("abc"); /* 末尾に改行なし */
    key(CDK_K_HOME, 0, 0);
    T_EQ_INT((int)E.cursor, 0);
    key(CDK_K_DOWN, 0, 0);
    T_EQ_INT((int)E.cursor, 0); /* 動かない */
    key(CDK_K_UP, 0, 0);
    T_EQ_INT((int)E.cursor, 0);

    T_CASE("末尾が改行なら空の最終行へ行ける");
    setup(COLS, ROWS);
    cdk_edit_new(&E, "a.txt");
    type("abc\n");
    key(CDK_K_UP, 0, 0);
    T_EQ_INT((int)E.cursor, 0);
    key(CDK_K_DOWN, 0, 0);
    T_EQ_INT((int)E.cursor, 4); /* 改行の次 = 空の行 */
    key(CDK_K_DOWN, 0, 0);
    T_EQ_INT((int)E.cursor, 4); /* その先は無い */
}

static void test_wrap_scroll(void)
{
    /* 実機で走るのはこの形。1 つの論理行が画面より深く折り返す。
     * ensure_visible が収束しないと「画面がずれる」ではなく**固まる**ので、
     * 画素の話になる前に落としておきたい。 */
    T_CASE("折り返しが画面より深い行をスクロールする");
    setup(10, ROWS); /* 本文 5 行 */
    cdk_edit_new(&E, "a.txt");
    /* 全角 20 字 = 40 桁 = 4 行ぶん。さらに続けて 8 行ぶんにする */
    char many[512];
    many[0] = '\0';
    for (int i = 0; i < 40; i++) {
        strcat(many, "あ");
    }
    cdk_edit_insert(&E, many, strlen(many));
    cdk_edit_draw(&E);

    /* 40 字 x 2 桁 / 10 桁 = 16 行ぶん。最後の行が見えている */
    T_EQ_INT(E.cur_row, ROWS - 2); /* 本文の最下行 */
    T_CHECK(E.top > 0);

    T_CASE("PgUp / PgDn が折り返しの中でも動く");
    const size_t at_end = E.cursor;
    key(CDK_K_PGUP, 0, 0);
    T_CHECK(E.cursor < at_end);
    const size_t after_pgup = E.cursor;
    key(CDK_K_PGDN, 0, 0);
    T_CHECK(E.cursor > after_pgup);

    T_CASE("上へ押し続けると先頭の行に着く");
    /* 桁は保たれるので本文の先頭 (0) ではなく「1 行目の同じ桁」。 */
    for (int i = 0; i < 40; i++) {
        key(CDK_K_UP, 0, 0);
    }
    T_EQ_INT((int)E.top, 0);
    T_EQ_INT(E.cur_row, 0);
    T_CHECK(E.cursor < 15); /* 1 行目 = 先頭 5 文字 = 15 バイト */

    T_CASE("Home で本文の先頭へ");
    key(CDK_K_HOME, 0, 0);
    T_EQ_INT((int)E.cursor, 0);

    T_CASE("下へ押し続けると最後の行に着く");
    for (int i = 0; i < 40; i++) {
        key(CDK_K_DOWN, 0, 0);
    }
    T_EQ_INT(E.cur_row, ROWS - 2);
    T_CHECK(E.cursor >= 105); /* 最後の表示行 */

    T_CASE("End と Down で本文の末尾に届く");
    key(CDK_K_END, 0, 0);
    key(CDK_K_DOWN, 0, 0);
    T_EQ_INT((int)E.cursor, (int)at_end);
}

static void test_load_guard(void)
{
    T_CASE("入りきらないファイルは読み込まない");
    /* 途中まで読んで保存すると、その時点で後半が消える。 */
    setup(COLS, ROWS);
    static char big[sizeof(ARENA) + 100];
    memset(big, 'x', sizeof(big));
    T_CHECK(!cdk_edit_load(&E, "big.txt", big, sizeof(big)));
    size_t n = 0;
    (void)cdk_edit_text(&E, &n);
    T_EQ_INT(n, 0); /* 何も持っていない */
    T_CHECK(E.readonly);

    cdk_edit_draw(&E);
    T_CHECK(strstr(row_text(ROWS - 1), "大きすぎます") != NULL);

    T_CASE("読取専用では書き換わらない");
    type("abc");
    (void)cdk_edit_text(&E, &n);
    T_EQ_INT(n, 0);
    T_CHECK(!E.modified);

    T_CASE("ちょうど収まるファイルは読める");
    setup(COLS, ROWS);
    T_CHECK(cdk_edit_load(&E, "fit.txt", big, sizeof(ARENA)));
    (void)cdk_edit_text(&E, &n);
    T_EQ_INT(n, sizeof(ARENA));
    T_CHECK(!E.readonly);

    T_CASE("いっぱいなら挿入しない");
    cdk_edit_insert(&E, "a", 1);
    (void)cdk_edit_text(&E, &n);
    T_EQ_INT(n, sizeof(ARENA));
}

static void test_quit_save(void)
{
    T_CASE("変更が無ければ Ctrl-Q ですぐ終わる");
    setup(COLS, ROWS);
    cdk_edit_new(&E, "a.txt");
    key(0, CDK_MOD_CTRL_BIT, 'q');
    T_CHECK(E.want_quit);

    T_CASE("変更があると 1 回目は聞き返す");
    setup(COLS, ROWS);
    cdk_edit_new(&E, "a.txt");
    type("x");
    key(0, CDK_MOD_CTRL_BIT, 'q');
    T_CHECK(!E.want_quit);
    /* 20 桁しかないので後半は切れる。切れること自体が正しい。 */
    T_CHECK(strstr(row_text(ROWS - 1), "変更") != NULL);
    key(0, CDK_MOD_CTRL_BIT, 'q');
    T_CHECK(E.want_quit);

    T_CASE("何か打つと聞き返しは取り消される");
    setup(COLS, ROWS);
    cdk_edit_new(&E, "a.txt");
    type("x");
    key(0, CDK_MOD_CTRL_BIT, 'q');
    type("y");
    key(0, CDK_MOD_CTRL_BIT, 'q');
    T_CHECK(!E.want_quit); /* また聞き返す */

    T_CASE("Ctrl-S で保存を頼む");
    setup(COLS, ROWS);
    cdk_edit_new(&E, "a.txt");
    type("x");
    key(0, CDK_MOD_CTRL_BIT, 's');
    T_CHECK(E.want_save);
    cdk_edit_saved(&E);
    T_CHECK(!E.modified);
    cdk_edit_draw(&E);
    T_CHECK(strstr(row_text(ROWS - 1), "*") == NULL);
}

static void test_status_clip(void)
{
    /* ステータス行は端末の最下行に書く。右端を超えると端末が改行し、
     * **画面全体が 1 行ぶんスクロールして本文が消える**。
     * 長いメッセージを出した瞬間にエディタが壊れて見えるので、
     * ここは切れていることを確かめておく。 */
    T_CASE("長いメッセージでも画面がずれない");
    setup(COLS, ROWS);
    cdk_edit_new(&E, "a.txt");
    type("line1\nline2\nline3");
    T_EQ_STR(row_text(0), "line1");
    T_EQ_STR(row_text(2), "line3");

    cdk_edit_message(&E,
                     "とても長いメッセージ。画面の幅をはるかに超えるので、"
                     "切らないと下から押し出されて本文が上へ流れてしまう。");
    cdk_edit_draw(&E);

    /* 本文はそのまま */
    T_EQ_STR(row_text(0), "line1");
    T_EQ_STR(row_text(2), "line3");
    /* ステータス行は 1 行に収まっている */
    T_CHECK(strstr(row_text(ROWS - 1), "とても長い") != NULL);

    T_CASE("全角を途中で切らない");
    setup(9, ROWS); /* 奇数桁 */
    cdk_edit_new(&E, "a.txt");
    cdk_edit_message(&E, "あいうえおかきくけこ");
    cdk_edit_draw(&E);
    /* 9 桁に全角 4 つ (8 桁) まで。5 つ目は入らない */
    T_EQ_STR(row_text(ROWS - 1), "あいうえ");
}

int main(void)
{
    test_basic();
    test_status_clip();
    test_move();
    test_wrap();
    test_wide();
    test_scroll();
    test_wrap_scroll();
    test_load_guard();
    test_quit_save();
    return t_report("edit");
}
