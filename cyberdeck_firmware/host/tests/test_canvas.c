#include "cdk_canvas.h"
#include "t.h"

#define W 64
#define H 48

static uint16_t pixbuf[W * H];
static cdk_canvas_t cv;

static void fresh(void)
{
    memset(pixbuf, 0xAA, sizeof(pixbuf));
    T_CHECK(cdk_canvas_init(&cv, pixbuf, W, H));
}

int main(void)
{
    T_CASE("init と不正引数");
    {
        cdk_canvas_t c;
        T_CHECK(!cdk_canvas_init(&c, NULL, W, H));
        T_CHECK(!cdk_canvas_init(&c, pixbuf, 0, H));
        T_CHECK(!cdk_canvas_init(&c, pixbuf, W, -1));
        T_CHECK(cdk_canvas_init(&c, pixbuf, W, H));
        T_EQ_INT(c.w, W);
        T_EQ_INT(c.h, H);
        /* 初期状態はダーティ無し */
        T_CHECK(!cdk_canvas_dirty(&c, NULL, NULL));
        T_EQ_INT(cdk_canvas_bytes(W, H), W * H * 2);
    }

    T_CASE("rgb565 の詰め方");
    {
        T_EQ_U32(cdk_rgb565(0, 0, 0), 0x0000u);
        T_EQ_U32(cdk_rgb565(0xFF, 0xFF, 0xFF), 0xFFFFu);
        T_EQ_U32(cdk_rgb565(0xFF, 0, 0), 0xF800u);
        T_EQ_U32(cdk_rgb565(0, 0xFF, 0), 0x07E0u);
        T_EQ_U32(cdk_rgb565(0, 0, 0xFF), 0x001Fu);
    }

    T_CASE("clear と get");
    {
        fresh();
        cdk_canvas_clear(&cv, CDK_BLUE);
        T_EQ_U32(cdk_canvas_get(&cv, 0, 0), CDK_BLUE);
        T_EQ_U32(cdk_canvas_get(&cv, W - 1, H - 1), CDK_BLUE);
        /* 範囲外は 0 を返し、書いても落ちない */
        T_EQ_U32(cdk_canvas_get(&cv, -1, 0), 0u);
        T_EQ_U32(cdk_canvas_get(&cv, W, 0), 0u);
        cdk_canvas_put(&cv, -5, -5, CDK_RED);
        cdk_canvas_put(&cv, W + 5, H + 5, CDK_RED);
    }

    T_CASE("fill_rect のクリップ");
    {
        fresh();
        cdk_canvas_clear(&cv, CDK_BLACK);
        /* 左上にはみ出す矩形 */
        cdk_canvas_fill_rect(&cv, -4, -4, 8, 8, CDK_RED);
        T_EQ_U32(cdk_canvas_get(&cv, 0, 0), CDK_RED);
        T_EQ_U32(cdk_canvas_get(&cv, 3, 3), CDK_RED);
        T_EQ_U32(cdk_canvas_get(&cv, 4, 4), CDK_BLACK);
        /* 右下にはみ出す矩形 */
        cdk_canvas_fill_rect(&cv, W - 4, H - 4, 100, 100, CDK_GREEN);
        T_EQ_U32(cdk_canvas_get(&cv, W - 1, H - 1), CDK_GREEN);
        T_EQ_U32(cdk_canvas_get(&cv, W - 5, H - 5), CDK_BLACK);
        /* 完全に外 */
        cdk_canvas_fill_rect(&cv, 1000, 1000, 10, 10, CDK_YELLOW);
        /* 幅ゼロ */
        cdk_canvas_fill_rect(&cv, 10, 10, 0, 5, CDK_YELLOW);
        T_EQ_U32(cdk_canvas_get(&cv, 10, 10), CDK_BLACK);
    }

    T_CASE("行はストライドを跨がない");
    {
        fresh();
        cdk_canvas_clear(&cv, CDK_BLACK);
        cdk_canvas_hline(&cv, 0, 5, W, CDK_WHITE);
        for (int x = 0; x < W; x++) {
            T_EQ_U32(cdk_canvas_get(&cv, x, 5), CDK_WHITE);
        }
        T_EQ_U32(cdk_canvas_get(&cv, 0, 4), CDK_BLACK);
        T_EQ_U32(cdk_canvas_get(&cv, 0, 6), CDK_BLACK);
    }

    T_CASE("rect は枠だけ");
    {
        fresh();
        cdk_canvas_clear(&cv, CDK_BLACK);
        cdk_canvas_rect(&cv, 2, 2, 10, 10, CDK_WHITE);
        T_EQ_U32(cdk_canvas_get(&cv, 2, 2), CDK_WHITE);
        T_EQ_U32(cdk_canvas_get(&cv, 11, 11), CDK_WHITE);
        T_EQ_U32(cdk_canvas_get(&cv, 11, 2), CDK_WHITE);
        T_EQ_U32(cdk_canvas_get(&cv, 6, 6), CDK_BLACK);
    }

    T_CASE("ダーティ行の追跡");
    {
        int y0 = -1, y1 = -1;
        fresh();
        cdk_canvas_dirty_clear(&cv);
        T_CHECK(!cdk_canvas_dirty(&cv, &y0, &y1));

        cdk_canvas_fill_rect(&cv, 0, 10, 4, 4, CDK_RED);
        T_CHECK(cdk_canvas_dirty(&cv, &y0, &y1));
        T_EQ_INT(y0, 10);
        T_EQ_INT(y1, 14);

        /* 離れた行を触ると範囲は結合される */
        cdk_canvas_fill_rect(&cv, 0, 30, 2, 2, CDK_RED);
        T_CHECK(cdk_canvas_dirty(&cv, &y0, &y1));
        T_EQ_INT(y0, 10);
        T_EQ_INT(y1, 32);

        cdk_canvas_dirty_clear(&cv);
        T_CHECK(!cdk_canvas_dirty(&cv, &y0, &y1));

        /* クリップされた分は範囲に入らない */
        cdk_canvas_fill_rect(&cv, 0, -10, 4, 12, CDK_RED);
        T_CHECK(cdk_canvas_dirty(&cv, &y0, &y1));
        T_EQ_INT(y0, 0);
        T_EQ_INT(y1, 2);

        cdk_canvas_mark_all_dirty(&cv);
        T_CHECK(cdk_canvas_dirty(&cv, &y0, &y1));
        T_EQ_INT(y0, 0);
        T_EQ_INT(y1, H);
    }

    T_CASE("blit_1bpp");
    {
        /* 8x2: 10101010 / 01010101 */
        static const uint8_t bits[] = {0xAAu, 0x55u};
        fresh();
        cdk_canvas_clear(&cv, CDK_BLACK);
        cdk_canvas_blit_1bpp(&cv, 4, 4, 8, 2, bits, CDK_WHITE, CDK_BLUE);
        T_EQ_U32(cdk_canvas_get(&cv, 4, 4), CDK_WHITE);
        T_EQ_U32(cdk_canvas_get(&cv, 5, 4), CDK_BLUE);
        T_EQ_U32(cdk_canvas_get(&cv, 4, 5), CDK_BLUE);
        T_EQ_U32(cdk_canvas_get(&cv, 5, 5), CDK_WHITE);
        T_EQ_U32(cdk_canvas_get(&cv, 11, 4), CDK_BLUE);
        T_EQ_U32(cdk_canvas_get(&cv, 12, 4), CDK_BLACK);

        /* 背景透過 */
        cdk_canvas_clear(&cv, CDK_GREEN);
        cdk_canvas_blit_1bpp(&cv, 4, 4, 8, 2, bits, CDK_RED, CDK_CANVAS_NO_BG);
        T_EQ_U32(cdk_canvas_get(&cv, 4, 4), CDK_RED);
        T_EQ_U32(cdk_canvas_get(&cv, 5, 4), CDK_GREEN);

        /* はみ出しても落ちない */
        cdk_canvas_blit_1bpp(&cv, -3, -1, 8, 2, bits, CDK_RED, CDK_CANVAS_NO_BG);
        cdk_canvas_blit_1bpp(&cv, W - 2, H - 1, 8, 2, bits, CDK_RED, CDK_CANVAS_NO_BG);
    }

    T_CASE("テストパターンの構造");
    {
        fresh();
        cdk_canvas_testpattern(&cv);

        /* 外周は白 */
        T_EQ_U32(cdk_canvas_get(&cv, 0, 0), CDK_WHITE);
        T_EQ_U32(cdk_canvas_get(&cv, W - 1, 0), CDK_WHITE);
        T_EQ_U32(cdk_canvas_get(&cv, 0, H - 1), CDK_WHITE);
        T_EQ_U32(cdk_canvas_get(&cv, W - 1, H - 1), CDK_WHITE);

        /* 四隅マーカーで向きが判定できる */
        T_EQ_U32(cdk_canvas_get(&cv, 4, 4), CDK_RED);
        T_EQ_U32(cdk_canvas_get(&cv, W - 5, 4), CDK_GREEN);
        T_EQ_U32(cdk_canvas_get(&cv, 4, H - 5), CDK_BLUE);
        T_EQ_U32(cdk_canvas_get(&cv, W - 5, H - 5), CDK_YELLOW);

        /* 上段のカラーバー: 2 本目は黄、6 本目は赤 */
        T_EQ_U32(cdk_canvas_get(&cv, W / 8 + 2, 12), CDK_YELLOW);
        T_EQ_U32(cdk_canvas_get(&cv, W * 5 / 8 + 2, 12), CDK_RED);

        /* 中段の勾配は単調非減少（対角線が乗っている画素だけ除く） */
        {
            int band = H / 3;
            int y = band + 2;
            uint16_t prev = 0;
            int monotonic = 1;
            for (int x = 1; x < W - 1; x++) {
                uint16_t v = cdk_canvas_get(&cv, x, y);
                if (v == CDK_MAGENTA) {
                    continue;
                }
                if (v < prev) {
                    monotonic = 0;
                }
                prev = v;
            }
            T_CHECK(monotonic);
            T_CHECK(cdk_canvas_get(&cv, 1, y) < cdk_canvas_get(&cv, W - 2, y));
        }

        /* 全面ダーティになっている */
        {
            int y0, y1;
            T_CHECK(cdk_canvas_dirty(&cv, &y0, &y1));
            T_EQ_INT(y0, 0);
            T_EQ_INT(y1, H);
        }
    }

    T_CASE("テストパターンは決定的");
    {
        static uint16_t a[W * H];
        static uint16_t b[W * H];
        cdk_canvas_t ca, cb;
        cdk_canvas_init(&ca, a, W, H);
        cdk_canvas_init(&cb, b, W, H);
        memset(a, 0x00, sizeof(a));
        memset(b, 0xFF, sizeof(b));
        cdk_canvas_testpattern(&ca);
        cdk_canvas_testpattern(&cb);
        T_EQ_MEM(a, b, sizeof(a));
    }

    T_CASE("実機の解像度でも破綻しない");
    {
        /* CoreS3 と最終ハードの両方 */
        static uint16_t big[480 * 320];
        cdk_canvas_t c;
        cdk_canvas_init(&c, big, 320, 240);
        cdk_canvas_testpattern(&c);
        T_EQ_U32(cdk_canvas_get(&c, 0, 0), CDK_WHITE);
        T_EQ_U32(cdk_canvas_get(&c, 4, 4), CDK_RED);

        cdk_canvas_init(&c, big, 480, 320);
        cdk_canvas_testpattern(&c);
        T_EQ_U32(cdk_canvas_get(&c, 479, 319), CDK_WHITE);
        T_EQ_U32(cdk_canvas_get(&c, 475, 315), CDK_YELLOW);
    }

    return t_report("test_canvas");
}
