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

    /* ---- 未確定文字列（日本語入力のプリエディット） ---- */

    T_CASE("未確定文字列をカーソル位置に重ねる");
    {
        setup();
        feed("ab");
        T.cursor_visible = false;
        cdk_screen_set_preedit(&S, "▽");
        cdk_screen_draw(&S, &CV, &T, true);

        /* 反転表示なので、地の色と字の色が入れ替わる */
        T_CHECK(count_color(2, 0, 1, S.default_fg) > 0); /* 反転した背景 */
        /* 手前の 2 桁は元のまま */
        T_CHECK(count_color(0, 0, 1, S.default_fg) > 0);
        T_EQ_INT(count_color(0, 0, 1, S.default_fg) +
                     count_color(0, 0, 1, S.default_bg),
                 CW * CH);
    }

    T_CASE("全角は 2 桁を占める");
    {
        /* ▽ は半角、かなは全角。桁数を取り違えると
         * 未確定文字列の右にある字を巻き込んで消す。 */
        setup();
        T.cursor_visible = false;
        cdk_screen_set_preedit(&S, "▽に");
        cdk_screen_draw(&S, &CV, &T, true);

        /* 0 桁目 = ▽（半角・反転） 1..2 桁目 = に（全角・反転） */
        T_CHECK(count_color(0, 0, 1, S.default_fg) > 0);
        T_CHECK(count_color(1, 0, 2, S.default_fg) > 0);
        /* 3 桁目からは手つかず = 背景のまま */
        T_EQ_INT(count_color(3, 0, 1, S.default_bg), CW * CH);
    }

    T_CASE("短くなったら余りが消える");
    {
        setup();
        T.cursor_visible = false;
        cdk_screen_set_preedit(&S, "▽にほん");
        cdk_screen_draw(&S, &CV, &T, true);
        T_CHECK(count_color(5, 0, 2, S.default_fg) > 0); /* 「ん」があった */

        cdk_screen_set_preedit(&S, "▽に");
        cdk_screen_draw(&S, &CV, &T, false); /* ダーティ行だけ描いても消えること */
        T_EQ_INT(count_color(5, 0, 2, S.default_bg), CW * CH * 2);
    }

    T_CASE("空にすると跡形も無くなる");
    {
        setup();
        T.cursor_visible = false;
        feed("hello");
        cdk_screen_set_preedit(&S, "▽に");
        cdk_screen_draw(&S, &CV, &T, true);
        cdk_screen_set_preedit(&S, "");
        cdk_screen_draw(&S, &CV, &T, false);

        /* 元の "hello" が戻っている（5 桁目 'o' の位置に字がある） */
        T_CHECK(count_color(4, 0, 1, S.default_fg) > 0);
        T_CHECK(count_color(4, 0, 1, S.default_bg) > 0);
    }

    T_CASE("右端に収まらないときは左へ寄せる");
    {
        /* 折り返して下の行を潰すより、左へ寄せて全部見せる。
         * 何を入力しているのか見えないほうが困る。 */
        setup();
        T.cursor_visible = false;
        T.cx = COLS - 2; /* 残り 2 桁しかない */
        cdk_screen_set_preedit(&S, "▽にほ");
        cdk_screen_draw(&S, &CV, &T, true);

        /* 右端 5 桁 (▽ 1 + かな 2 + かな 2) が使われている */
        T_CHECK(count_color(COLS - 5, 0, 1, S.default_fg) > 0);
        T_CHECK(count_color(COLS - 1, 0, 1, S.default_fg) > 0);
        /* その左は手つかず */
        T_EQ_INT(count_color(COLS - 6, 0, 1, S.default_bg), CW * CH);
    }

    T_CASE("長すぎる未確定文字列は後ろを残す");
    {
        setup();
        char longstr[512];
        longstr[0] = '\0';
        for (int i = 0; i < 60; i++) {
            strcat(longstr, "あ");
        }
        strcat(longstr, "END");
        cdk_screen_set_preedit(&S, longstr);
        /* いま打っている端（末尾）が残っていること */
        const char *p = S.preedit;
        T_CHECK(strlen(p) < sizeof(S.preedit));
        T_CHECK(strcmp(p + strlen(p) - 3, "END") == 0);
        /* UTF-8 の途中で切っていないこと */
        T_CHECK(((unsigned char)p[0] & 0xC0u) != 0x80u);
    }

    T_CASE("2 倍にすると桁数と行数が半分になる");
    {
        setup();
        T_EQ_INT(cdk_screen_cols(&S, COLS * CW), COLS);
        T_EQ_INT(cdk_screen_rows(&S, ROWS * CH), ROWS);
        T_EQ_INT(cdk_screen_cell_w(&S), CW);
        T_EQ_INT(cdk_screen_cell_h(&S), CH);

        cdk_screen_set_scale(&S, 2);
        T_EQ_INT(cdk_screen_cell_w(&S), CW * 2);
        T_EQ_INT(cdk_screen_cell_h(&S), CH * 2);
        T_EQ_INT(cdk_screen_cols(&S, COLS * CW), COLS / 2);
        T_EQ_INT(cdk_screen_rows(&S, ROWS * CH), ROWS / 2);

        /* 実機の寸法でも確かめる。480x320 は両方とも割り切れて、
         * 余りが 8px（ステータス帯の予約分）になる。 */
        cdk_screen_set_scale(&S, 1);
        T_EQ_INT(cdk_screen_cols(&S, 480), 80);
        T_EQ_INT(cdk_screen_rows(&S, 320), 26);
        cdk_screen_set_scale(&S, 2);
        T_EQ_INT(cdk_screen_cols(&S, 480), 40);
        T_EQ_INT(cdk_screen_rows(&S, 320), 13);
        T_EQ_INT(320 - 13 * 24, 8);
        T_EQ_INT(320 - 26 * 12, 8);

        cdk_screen_set_scale(&S, 1);
    }

    T_CASE("2 倍のセルは 2 倍の場所に出る");
    {
        setup();
        cdk_screen_set_scale(&S, 2);
        feed("\033[44mA"); /* 青背景 */
        T.cursor_visible = false;
        cdk_screen_draw(&S, &CV, &T, true);

        /* 1 セルが 12x24 を占める */
        T_EQ_U32(cdk_canvas_get(&CV, 0, 0), S.palette[4]);
        T_EQ_U32(cdk_canvas_get(&CV, CW * 2 - 1, CH * 2 - 1), S.palette[4]);
        /* その外は塗られていない */
        T_EQ_U32(cdk_canvas_get(&CV, CW * 2, 0), S.default_bg);
    }

    T_CASE("2 倍の字は等倍の字を 2x2 に広げたものと一致する");
    {
        /* **これが「倍にする」の定義。** 別のフォントに差し替えるのではなく、
         * 同じグリフを画素ごと広げる。ずれると字形が変わる。 */
        static uint16_t small[COLS * CW * ROWS * CH];
        static uint16_t big[COLS * CW * ROWS * CH];
        cdk_canvas_t cs, cb;

        setup();
        cdk_canvas_init(&cs, small, COLS * CW, ROWS * CH);
        cdk_canvas_clear(&cs, CDK_BLACK);
        feed("A");
        T.cursor_visible = false;
        cdk_screen_draw(&S, &cs, &T, true);

        setup();
        cdk_canvas_init(&cb, big, COLS * CW, ROWS * CH);
        cdk_canvas_clear(&cb, CDK_BLACK);
        cdk_screen_set_scale(&S, 2);
        feed("A");
        T.cursor_visible = false;
        cdk_screen_draw(&S, &cb, &T, true);

        for (int y = 0; y < CH; y++) {
            for (int x = 0; x < CW; x++) {
                const uint16_t want = cdk_canvas_get(&cs, x, y);
                /* 元の 1 画素が 2x2 に広がっている */
                T_EQ_U32(cdk_canvas_get(&cb, x * 2 + 0, y * 2 + 0), want);
                T_EQ_U32(cdk_canvas_get(&cb, x * 2 + 1, y * 2 + 0), want);
                T_EQ_U32(cdk_canvas_get(&cb, x * 2 + 0, y * 2 + 1), want);
                T_EQ_U32(cdk_canvas_get(&cb, x * 2 + 1, y * 2 + 1), want);
            }
        }
    }

    T_CASE("2 倍でも全角は 2 桁を占める");
    {
        setup();
        cdk_screen_set_scale(&S, 2);
        feed("\033[41m漢");
        T.cursor_visible = false;
        cdk_screen_draw(&S, &CV, &T, true);

        /* 全角 1 文字で 24x24。**角の画素で判定しない**（字の墨が乗っている
         * ことがある）。背景色が範囲内にはあって範囲外には無い、で見る。 */
        int inside = 0, outside = 0;
        for (int y = 0; y < CH * 2; y++) {
            for (int x = 0; x < CW * 4; x++) {
                if (cdk_canvas_get(&CV, x, y) == S.palette[1]) {
                    inside++;
                }
            }
            for (int x = CW * 4; x < CW * 6; x++) {
                if (cdk_canvas_get(&CV, x, y) == S.palette[1]) {
                    outside++;
                }
            }
        }
        T_CHECK(inside > 0);
        T_EQ_INT(outside, 0);
    }

    T_CASE("ガンマ補正");
    {
        /* 1.0 以下は恒等。描画側が分岐しなくて済むことの担保。 */
        cdk_screen_set_gamma(&S, 10);
        for (int i = 0; i < 32; i++) {
            T_EQ_INT(S.lut5[i], i);
        }
        for (int i = 0; i < 64; i++) {
            T_EQ_INT(S.lut6[i], i);
        }

        cdk_screen_set_gamma(&S, 18);
        /* **両端は動かない。** 黒が浮くと背景が灰色になり、
         * 白が下がると地の文が暗くなる。どちらも起きてはいけない。 */
        T_EQ_INT(S.lut5[0], 0);
        T_EQ_INT(S.lut5[31], 31);
        T_EQ_INT(S.lut6[0], 0);
        T_EQ_INT(S.lut6[63], 63);

        /* 単調非減少。ここが崩れると色の順序が入れ替わる。 */
        for (int i = 1; i < 32; i++) {
            T_CHECK(S.lut5[i] >= S.lut5[i - 1]);
        }
        for (int i = 1; i < 64; i++) {
            T_CHECK(S.lut6[i] >= S.lut6[i - 1]);
        }

        /* 中間は必ず持ち上がる（沈んだら補正の向きが逆） */
        T_CHECK(S.lut5[16] > 16);
        T_CHECK(S.lut6[32] > 32);

        /* ガンマを上げるほど強く持ち上がる */
        {
            const uint8_t mid18 = S.lut6[32];
            cdk_screen_set_gamma(&S, 30);
            T_CHECK(S.lut6[32] > mid18);
            T_EQ_INT(S.lut6[0], 0);
            T_EQ_INT(S.lut6[63], 63);
        }

        /* 同じ入力なら必ず同じ表になる（実機とホストで一致する前提） */
        {
            uint8_t a5[32], a6[64];
            cdk_screen_set_gamma(&S, 22);
            memcpy(a5, S.lut5, sizeof(a5));
            memcpy(a6, S.lut6, sizeof(a6));
            cdk_screen_set_gamma(&S, 10);
            cdk_screen_set_gamma(&S, 22);
            T_EQ_MEM(a5, S.lut5, sizeof(a5));
            T_EQ_MEM(a6, S.lut6, sizeof(a6));
        }

        /* 描画にも効くこと。無補正なら素のパレット値で塗られる。 */
        setup();
        feed("\033[31mA");
        T.cursor_visible = false;
        cdk_screen_draw(&S, &CV, &T, true);
        T_CHECK(count_color(0, 0, 1, S.palette[1]) > 0);

        /* 補正を入れると、同じ赤がもう素のパレット値では出てこない。 */
        setup();
        cdk_screen_set_gamma(&S, 22);
        feed("\033[31mA");
        T.cursor_visible = false;
        cdk_screen_draw(&S, &CV, &T, true);
        T_CHECK(count_color(0, 0, 1, S.palette[1]) == 0);
        {
            const uint16_t raw = S.palette[1];
            const uint16_t want =
                (uint16_t)((S.lut5[(raw >> 11) & 0x1Fu] << 11) |
                           (S.lut6[(raw >> 5) & 0x3Fu] << 5) | S.lut5[raw & 0x1Fu]);
            T_CHECK(count_color(0, 0, 1, want) > 0);
            T_CHECK(((want >> 11) & 0x1Fu) > ((raw >> 11) & 0x1Fu)); /* 赤が明るく */
        }

        /* **dim は補正の前に掛かる。** 逆順だと「補正後の半分」になり、
         * このパネルで狙っている明るさより沈む。 */
        setup();
        cdk_screen_set_gamma(&S, 22);
        feed("\033[2;37mA"); /* dim + 白 */
        T.cursor_visible = false;
        cdk_screen_draw(&S, &CV, &T, true);
        {
            const uint16_t half = (uint16_t)(((S.palette[7] & 0xF800u) >> 1 & 0xF800u) |
                                             ((S.palette[7] & 0x07E0u) >> 1 & 0x07E0u) |
                                             ((S.palette[7] & 0x001Fu) >> 1));
            const uint16_t want =
                (uint16_t)((S.lut5[(half >> 11) & 0x1Fu] << 11) |
                           (S.lut6[(half >> 5) & 0x3Fu] << 5) | S.lut5[half & 0x1Fu]);
            T_CHECK(count_color(0, 0, 1, want) > 0);
        }

        setup();
    }

    free(blob);
    return t_report("test_screen");
}
