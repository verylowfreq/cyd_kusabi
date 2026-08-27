#include "cdk_font.h"
#include "cdk_text.h"
#include "t.h"

#include <stdio.h>

static uint8_t *blob;
static size_t blob_len;

static bool load_blob(void)
{
    FILE *f = fopen(CDK_FONT_BLOB_PATH, "rb");
    if (f == NULL) {
        printf("   フォントブロブを開けません: %s\n", CDK_FONT_BLOB_PATH);
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    blob = (uint8_t *)malloc((size_t)n);
    blob_len = fread(blob, 1, (size_t)n, f);
    fclose(f);
    return blob_len == (size_t)n;
}

/* グリフの黒画素数。空白かどうかの判定に使う。 */
static int ink(const cdk_glyph_t *g, int cell_h)
{
    int n = 0;
    for (int y = 0; y < cell_h; y++) {
        for (int x = 0; x < g->width; x++) {
            if (cdk_glyph_pixel(g, x, y)) {
                n++;
            }
        }
    }
    return n;
}

int main(void)
{
    cdk_font_t f;

    T_CASE("ブロブを開く");
    {
        T_CHECK(load_blob());
        T_CHECK(cdk_font_open(&f, blob, blob_len));
        T_EQ_INT(f.cell_w, 6);
        T_EQ_INT(f.cell_h, 12);
        T_EQ_INT(f.baseline, 10);
        T_CHECK(f.count > 6000); /* JIS X 0208 が入っていること */
    }

    T_CASE("壊れたブロブは弾く");
    {
        cdk_font_t bad;
        T_CHECK(!cdk_font_open(&bad, blob, 8));           /* 短すぎる */
        T_CHECK(!cdk_font_open(&bad, "XXXX....", 8));     /* magic 違い */
        T_CHECK(!cdk_font_open(&bad, NULL, 100));
        /* 途中で切れたブロブ。セクションが範囲外になるので開けない。 */
        T_CHECK(!cdk_font_open(&bad, blob, blob_len / 2));
    }

    T_CASE("ASCII は半角グリフ");
    {
        cdk_glyph_t g;
        T_CHECK(cdk_font_glyph(&f, 'A', 1, &g));
        T_CHECK(!g.wide);
        T_EQ_INT(g.width, 6);
        T_EQ_INT(g.stride, 1);
        T_CHECK(ink(&g, f.cell_h) > 0);

        /* 空白は真っ白 */
        T_CHECK(cdk_font_glyph(&f, ' ', 1, &g));
        T_EQ_INT(ink(&g, f.cell_h), 0);
    }

    T_CASE("かな・漢字は全角グリフ");
    {
        cdk_glyph_t g;
        T_CHECK(cdk_font_glyph(&f, 0x3042, 2, &g)); /* あ */
        T_CHECK(g.wide);
        T_EQ_INT(g.width, 12);
        T_EQ_INT(g.stride, 2);
        T_CHECK(ink(&g, f.cell_h) > 0);

        T_CHECK(cdk_font_glyph(&f, 0x6F22, 2, &g)); /* 漢 */
        T_CHECK(ink(&g, f.cell_h) > 10);

        T_CHECK(cdk_font_glyph(&f, 0xFF21, 2, &g)); /* Ａ 全角英字 */
        T_CHECK(g.wide);
    }

    T_CASE("半角カナ");
    {
        cdk_glyph_t g;
        T_CHECK(cdk_font_glyph(&f, 0xFF71, 1, &g)); /* ｱ */
        T_CHECK(!g.wide);
        T_CHECK(ink(&g, f.cell_h) > 0);
        T_CHECK(cdk_font_glyph(&f, 0xFF9E, 1, &g)); /* ｾﾞ の濁点 */
    }

    T_CASE("罫線素片は半角で入っている");
    {
        /* ここが全角だと CLI の枠が必ず崩れる。M1 で一番効く検査。 */
        cdk_glyph_t g;
        static const uint32_t box[] = {
            0x2500, 0x2501, 0x2502, 0x2503, 0x250C, 0x2510, 0x2514, 0x2518,
            0x251C, 0x2524, 0x252C, 0x2534, 0x253C, 0x2550, 0x2551, 0x2554,
            0x2557, 0x255A, 0x255D, 0x256C, 0x2571, 0x2572, 0x257C,
        };
        for (size_t i = 0; i < sizeof(box) / sizeof(box[0]); i++) {
            const bool found = cdk_font_glyph(&f, box[i], 1, &g);
            if (!found) {
                printf("   U+%04X が半角で引けない\n", (unsigned)box[i]);
            }
            T_CHECK(found);
            T_CHECK(!g.wide);
            T_CHECK(ink(&g, f.cell_h) > 0);
        }
    }

    T_CASE("罫線は上下左右がセルの端まで届く");
    {
        /* 端まで描かれていないと、隣のセルと線が繋がらない。 */
        cdk_glyph_t g;
        T_CHECK(cdk_font_glyph(&f, 0x2502, 1, &g)); /* │ */
        T_CHECK(cdk_glyph_pixel(&g, 2, 0));
        T_CHECK(cdk_glyph_pixel(&g, 2, f.cell_h - 1));

        T_CHECK(cdk_font_glyph(&f, 0x2500, 1, &g)); /* ─ */
        T_CHECK(cdk_glyph_pixel(&g, 0, 5));
        T_CHECK(cdk_glyph_pixel(&g, g.width - 1, 5));

        T_CHECK(cdk_font_glyph(&f, 0x250C, 1, &g)); /* ┌ 右と下だけ */
        T_CHECK(!cdk_glyph_pixel(&g, 2, 0));            /* 上には出ない */
        T_CHECK(cdk_glyph_pixel(&g, 2, f.cell_h - 1));  /* 下には出る */
        T_CHECK(!cdk_glyph_pixel(&g, 0, 5));            /* 左には出ない */
        T_CHECK(cdk_glyph_pixel(&g, g.width - 1, 5));   /* 右には出る */
    }

    T_CASE("ブロック要素");
    {
        cdk_glyph_t g;
        T_CHECK(cdk_font_glyph(&f, 0x2588, 1, &g)); /* █ */
        T_EQ_INT(ink(&g, f.cell_h), 6 * 12);

        T_CHECK(cdk_font_glyph(&f, 0x2580, 1, &g)); /* ▀ 上半分 */
        T_EQ_INT(ink(&g, f.cell_h), 6 * 6);
        T_CHECK(cdk_glyph_pixel(&g, 0, 0));
        T_CHECK(!cdk_glyph_pixel(&g, 0, 11));

        T_CHECK(cdk_font_glyph(&f, 0x2584, 1, &g)); /* ▄ 下半分 */
        T_CHECK(!cdk_glyph_pixel(&g, 0, 0));
        T_CHECK(cdk_glyph_pixel(&g, 0, 11));

        T_CHECK(cdk_font_glyph(&f, 0x2592, 1, &g)); /* ▒ */
        T_EQ_INT(ink(&g, f.cell_h), 6 * 12 / 2);
    }

    T_CASE("CLI が使う記号が半角で入っている");
    {
        /* Ambiguous 幅の記号は端末が半角として数える。
         * 東雲は全角しか持たないので、自前で描いた半角版が要る。 */
        cdk_glyph_t g;
        static const uint32_t sym[] = {
            0x2010, 0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
            0x2026, 0x2190, 0x2191, 0x2192, 0x2193, 0x21D2, 0x2260, 0x2264,
            0x2265, 0x25A0, 0x25A1, 0x25B2, 0x25BC, 0x25C6, 0x25CB, 0x25CF,
            0x2605, 0x2713, 0x2717, 0x03C0,
            /* DEC 特殊グラフィクスが参照するもの */
            0x23BA, 0x23BB, 0x23BC, 0x23BD,
        };
        for (size_t i = 0; i < sizeof(sym) / sizeof(sym[0]); i++) {
            const bool found = cdk_font_glyph(&f, sym[i], 1, &g);
            if (!found) {
                printf("   U+%04X が半角で引けない\n", (unsigned)sym[i]);
            }
            T_CHECK(found);
            T_CHECK(!g.wide);
            T_CHECK(ink(&g, f.cell_h) > 0);
        }
    }

    T_CASE("DEC 特殊グラフィクスの写像先がすべて半角で引ける");
    {
        /* ESC ( 0 で ASCII が置き換わる先。ここが欠けると
         * ncurses アプリの枠が げた だらけになる。 */
        cdk_glyph_t g;
        static const uint32_t dec[] = {
            0x00A0, 0x25C6, 0x2592, 0x00B0, 0x00B1, 0x2518, 0x2510, 0x250C,
            0x2514, 0x253C, 0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C,
            0x2524, 0x2534, 0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260,
            0x00A3, 0x00B7,
        };
        for (size_t i = 0; i < sizeof(dec) / sizeof(dec[0]); i++) {
            const bool found = cdk_font_glyph(&f, dec[i], 1, &g);
            if (!found) {
                printf("   DEC graphics: U+%04X が半角で引けない\n", (unsigned)dec[i]);
            }
            T_CHECK(found);
            T_CHECK(!g.wide);
        }
    }

    T_CASE("セル数が食い違うと notdef を返す");
    {
        cdk_glyph_t g;
        /* あ は全角。半角として引こうとしても使わせない。 */
        T_CHECK(!cdk_font_glyph(&f, 0x3042, 1, &g));
        T_CHECK(!g.wide);
        T_EQ_INT(g.width, 6);
        T_CHECK(ink(&g, f.cell_h) > 0); /* notdef は空白ではない */

        /* A は半角。全角として引こうとしても使わせない。 */
        T_CHECK(!cdk_font_glyph(&f, 'A', 2, &g));
        T_CHECK(g.wide);
        T_EQ_INT(g.width, 12);
    }

    T_CASE("グリフが無い文字は notdef");
    {
        cdk_glyph_t g;
        T_CHECK(!cdk_font_glyph(&f, 0x1F600, 2, &g)); /* 絵文字 */
        T_CHECK(g.wide);
        T_CHECK(g.bits != NULL);
        T_CHECK(!cdk_font_glyph(&f, 0x0500, 1, &g)); /* キリル拡張 */
        T_CHECK(g.bits != NULL);
    }

    T_CASE("フォントの幅と cdk_char_width が矛盾しない");
    {
        /* 端末が数える幅とフォントが持つ幅がずれていたら、
         * その文字は必ず桁を壊す。全グリフを突き合わせる。 */
        int mismatch = 0;
        for (uint32_t cp = 0x20; cp <= 0xFFFF; cp++) {
            const int w = cdk_char_width(cp);
            if (w == 0) {
                continue;
            }
            cdk_glyph_t g;
            if (cdk_font_glyph(&f, cp, w, &g)) {
                /* 引けたなら幅は一致しているはず */
                T_CHECK(g.wide == (w == 2));
            } else {
                mismatch++;
            }
        }
        /* 引けない文字はたくさんある (絵文字・ハングルなど)。
         * 大事なのは「引けたグリフの幅が必ず正しい」こと。 */
        printf("   グリフが無い、または幅が合わない文字: %d\n", mismatch);
        T_CHECK(mismatch > 0);
    }

    T_CASE("ASCII 全域が半角で引ける");
    {
        cdk_glyph_t g;
        for (uint32_t cp = 0x20; cp < 0x7F; cp++) {
            if (!cdk_font_glyph(&f, cp, 1, &g)) {
                printf("   U+%04X (%c) が引けない\n", (unsigned)cp, (char)cp);
                T_CHECK(false);
            }
        }
    }

    free(blob);
    return t_report("test_font");
}
