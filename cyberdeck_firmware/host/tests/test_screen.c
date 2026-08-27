#include "cdk_screen.h"
#include "t.h"

#include <stdio.h>

#define COLS 20
#define ROWS 6
#define CW 6
#define CH 12

static cdk_term_t T;
static uint8_t termmem[64 * 1024];
static uint16_t pixels[COLS * CW * ROWS * CH];
static cdk_canvas_t CV;
static cdk_screen_t S;
static cdk_font_t F;
static uint8_t *blob;

static bool load_font(void)
{
    FILE *f = fopen(CDK_FONT_BLOB_PATH, "rb");
    if (f == NULL) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    blob = (uint8_t *)malloc((size_t)n);
    const size_t got = fread(blob, 1, (size_t)n, f);
    fclose(f);
    return cdk_font_open(&F, blob, got);
}

static void setup(void)
{
    T_CHECK(cdk_term_init(&T, COLS, ROWS, termmem, sizeof(termmem)));
    T_CHECK(cdk_canvas_init(&CV, pixels, COLS * CW, ROWS * CH));
    cdk_canvas_clear(&CV, 0x1234u); /* 描き残しが分かるよう変な色で埋める */
    cdk_screen_init(&S, &F);
}

static void feed(const char *s)
{
    cdk_term_feed(&T, s, strlen(s));
}

/* セル内の画素のうち color と一致する数 */
static int count_color(int col, int row, int cells, uint16_t color)
{
    int n = 0;
    for (int y = 0; y < CH; y++) {
        for (int x = 0; x < CW * cells; x++) {
            if (cdk_canvas_get(&CV, col * CW + x, row * CH + y) == color) {
                n++;
            }
        }
    }
    return n;
}

int main(void)
{
    T_CASE("フォントを読む");
    {
        T_CHECK(load_font());
    }

    T_CASE("パレット");
    {
        setup();
        T_EQ_U32(S.palette[0], cdk_rgb565(0x00, 0x00, 0x00));
        T_EQ_U32(S.palette[15], cdk_rgb565(0xFF, 0xFF, 0xFF));
        /* 216 色キューブの角 */
        T_EQ_U32(S.palette[16], cdk_rgb565(0, 0, 0));
        T_EQ_U32(S.palette[231], cdk_rgb565(255, 255, 255));
        /* グレースケール */
        T_EQ_U32(S.palette[232], cdk_rgb565(8, 8, 8));
        T_EQ_U32(S.palette[255], cdk_rgb565(238, 238, 238));
    }

    T_CASE("桁数と行数");
    {
        setup();
        T_EQ_INT(cdk_screen_cols(&S, 480), 80);
        T_EQ_INT(cdk_screen_rows(&S, 320), 26);
        T_EQ_INT(cdk_screen_cols(&S, 320), 53);
        T_EQ_INT(cdk_screen_rows(&S, 240), 20);
    }

    T_CASE("文字を描く");
    {
        setup();
        feed("A");
        T.cursor_visible = false;
        cdk_screen_draw(&S, &CV, &T, true);

        const int fg = count_color(0, 0, 1, S.default_fg);
        const int bg = count_color(0, 0, 1, S.default_bg);
        T_CHECK(fg > 0);            /* 字が描かれている */
        T_CHECK(bg > 0);            /* 背景も塗られている */
        T_EQ_INT(fg + bg, CW * CH); /* 初期値 0x1234 が残っていない */

        /* 隣のセルは空白（前景色なし） */
        T_EQ_INT(count_color(1, 0, 1, S.default_bg), CW * CH);
    }

    T_CASE("色と反転");
    {
        setup();
        feed("\033[31;44mA");
        T.cursor_visible = false;
        cdk_screen_draw(&S, &CV, &T, true);
        T_CHECK(count_color(0, 0, 1, S.palette[1]) > 0);  /* 赤い字 */
        T_CHECK(count_color(0, 0, 1, S.palette[4]) > 0);  /* 青い背景 */

        setup();
        feed("\033[31;44;7mA"); /* 反転 */
        T.cursor_visible = false;
        cdk_screen_draw(&S, &CV, &T, true);
        T_CHECK(count_color(0, 0, 1, S.palette[4]) > 0);  /* 青い字 */
        T_CHECK(count_color(0, 0, 1, S.palette[1]) > 0);  /* 赤い背景 */
    }

    T_CASE("太字は明るい色になる");
    {
        setup();
        feed("\033[31mA\033[1mB");
        T.cursor_visible = false;
        cdk_screen_draw(&S, &CV, &T, true);
        T_CHECK(count_color(0, 0, 1, S.palette[1]) > 0); /* 通常の赤 */
        T_CHECK(count_color(1, 0, 1, S.palette[9]) > 0); /* 明るい赤 */
        T_EQ_INT(count_color(1, 0, 1, S.palette[1]), 0);
    }

    T_CASE("truecolor");
    {
        setup();
        feed("\033[38;2;255;0;0mA");
        T.cursor_visible = false;
        cdk_screen_draw(&S, &CV, &T, true);
        T_CHECK(count_color(0, 0, 1, cdk_rgb565(255, 0, 0)) > 0);
    }

    T_CASE("全角は 2 セルにまたがる");
    {
        setup();
        feed("\033[44mあ");
        T.cursor_visible = false;
        cdk_screen_draw(&S, &CV, &T, true);
        /* 2 セルぶんが背景色 + 前景色で埋まっている */
        const int fg = count_color(0, 0, 2, S.default_fg);
        const int bg = count_color(0, 0, 2, S.palette[4]);
        T_CHECK(fg > 0);
        T_EQ_INT(fg + bg, CW * 2 * CH);
        /* 右半分にも字が掛かっている */
        T_CHECK(count_color(1, 0, 1, S.default_fg) > 0);
    }

    T_CASE("カーソルは反転する");
    {
        setup();
        feed("A");
        feed("\033[1;1H"); /* A の上へ戻す */
        cdk_screen_draw(&S, &CV, &T, true);
        /* 反転しているので背景が前景色になっている */
        T_CHECK(count_color(0, 0, 1, S.default_fg) > CW * CH / 2);
    }

    T_CASE("カーソルが動いたら前の行も描き直す");
    {
        setup();
        feed("\033[1;1H");
        cdk_screen_draw(&S, &CV, &T, true);
        cdk_term_clear_dirty(&T);

        /* 3 行目へ移動。1 行目のカーソル残像を消すため 1 行目も描き直される。 */
        feed("\033[3;1H");
        cdk_canvas_clear(&CV, 0x1234u);
        cdk_screen_draw(&S, &CV, &T, false);
        T_EQ_INT(count_color(0, 0, 1, 0x1234u), 0); /* 1 行目が描き直された */
        T_EQ_INT(count_color(0, 2, 1, 0x1234u), 0); /* 3 行目も */
        T_EQ_INT(count_color(0, 1, 1, 0x1234u), CW * CH); /* 2 行目は触っていない */
    }

    T_CASE("ダーティ行だけ描く");
    {
        setup();
        feed("\033[1;1Habc\r\ndef");
        T.cursor_visible = false;
        cdk_screen_draw(&S, &CV, &T, true);
        cdk_term_clear_dirty(&T);

        cdk_canvas_clear(&CV, 0x1234u);
        feed("\033[5;1HX");
        cdk_screen_draw(&S, &CV, &T, false);
        T_EQ_INT(count_color(0, 4, 1, 0x1234u), 0);       /* 5 行目は描いた */
        T_EQ_INT(count_color(0, 0, 1, 0x1234u), CW * CH); /* 1 行目は触っていない */
    }

    T_CASE("下線と取り消し線");
    {
        setup();
        feed("\033[4m ");  /* 下線付きの空白 */
        T.cursor_visible = false;
        cdk_screen_draw(&S, &CV, &T, true);
        T_EQ_INT(count_color(0, 0, 1, S.default_fg), CW); /* 1 行ぶん */

        setup();
        feed("\033[9m ");
        T.cursor_visible = false;
        cdk_screen_draw(&S, &CV, &T, true);
        T_EQ_INT(count_color(0, 0, 1, S.default_fg), CW);
    }

    T_CASE("原点をずらせる");
    {
        setup();
        S.origin_x = 3;
        S.origin_y = 2;
        feed("A");
        T.cursor_visible = false;
        cdk_screen_draw(&S, &CV, &T, true);
        /* 左上 3x2 は塗られていない */
        T_EQ_U32(cdk_canvas_get(&CV, 0, 0), 0x1234u);
        T_EQ_U32(cdk_canvas_get(&CV, 3, 2), S.default_bg);
    }

    T_CASE("キャンバスが小さくてもはみ出さない");
    {
        /* 端末より狭いキャンバスに描いても書き潰さない */
        static uint16_t small[10 * CW * 3 * CH];
        cdk_canvas_t sc;
        T_CHECK(cdk_canvas_init(&sc, small, 10 * CW, 3 * CH));
        cdk_canvas_clear(&sc, 0);
        T_CHECK(cdk_term_init(&T, COLS, ROWS, termmem, sizeof(termmem)));
        feed("0123456789ABCDEFGHIJ\r\nsecond\r\nthird\r\nfourth");
        cdk_screen_draw(&S, &sc, &T, true);
        /* 落ちなければよい */
        T_CHECK(true);
    }

    free(blob);
    return t_report("test_screen");
}
