#include "cdk_canvas.h"

#include <string.h>

bool cdk_canvas_init(cdk_canvas_t *c, uint16_t *pix, int w, int h)
{
    if (c == NULL || pix == NULL || w <= 0 || h <= 0) {
        return false;
    }
    c->pix = pix;
    c->w = w;
    c->h = h;
    c->dirty_y0 = h;
    c->dirty_y1 = 0;
    return true;
}

void cdk_canvas_mark_dirty(cdk_canvas_t *c, int y0, int y1)
{
    if (y0 < 0) {
        y0 = 0;
    }
    if (y1 > c->h) {
        y1 = c->h;
    }
    if (y0 >= y1) {
        return;
    }
    if (y0 < c->dirty_y0) {
        c->dirty_y0 = y0;
    }
    if (y1 > c->dirty_y1) {
        c->dirty_y1 = y1;
    }
}

void cdk_canvas_mark_all_dirty(cdk_canvas_t *c)
{
    c->dirty_y0 = 0;
    c->dirty_y1 = c->h;
}

bool cdk_canvas_dirty(const cdk_canvas_t *c, int *y0, int *y1)
{
    if (c->dirty_y0 >= c->dirty_y1) {
        return false;
    }
    if (y0 != NULL) {
        *y0 = c->dirty_y0;
    }
    if (y1 != NULL) {
        *y1 = c->dirty_y1;
    }
    return true;
}

void cdk_canvas_dirty_clear(cdk_canvas_t *c)
{
    c->dirty_y0 = c->h;
    c->dirty_y1 = 0;
}

void cdk_canvas_put(cdk_canvas_t *c, int x, int y, uint16_t color)
{
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) {
        return;
    }
    c->pix[(size_t)y * (size_t)c->w + (size_t)x] = color;
    cdk_canvas_mark_dirty(c, y, y + 1);
}

uint16_t cdk_canvas_get(const cdk_canvas_t *c, int x, int y)
{
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) {
        return 0;
    }
    return c->pix[(size_t)y * (size_t)c->w + (size_t)x];
}

void cdk_canvas_fill_rect(cdk_canvas_t *c, int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > c->w) {
        x1 = c->w;
    }
    if (y1 > c->h) {
        y1 = c->h;
    }
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    for (int yy = y0; yy < y1; yy++) {
        uint16_t *row = c->pix + (size_t)yy * (size_t)c->w;
        for (int xx = x0; xx < x1; xx++) {
            row[xx] = color;
        }
    }
    cdk_canvas_mark_dirty(c, y0, y1);
}

void cdk_canvas_clear(cdk_canvas_t *c, uint16_t color)
{
    cdk_canvas_fill_rect(c, 0, 0, c->w, c->h, color);
}

void cdk_canvas_hline(cdk_canvas_t *c, int x, int y, int w, uint16_t color)
{
    cdk_canvas_fill_rect(c, x, y, w, 1, color);
}

void cdk_canvas_vline(cdk_canvas_t *c, int x, int y, int h, uint16_t color)
{
    cdk_canvas_fill_rect(c, x, y, 1, h, color);
}

void cdk_canvas_rect(cdk_canvas_t *c, int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    cdk_canvas_hline(c, x, y, w, color);
    cdk_canvas_hline(c, x, y + h - 1, w, color);
    cdk_canvas_vline(c, x, y, h, color);
    cdk_canvas_vline(c, x + w - 1, y, h, color);
}

void cdk_canvas_blit_1bpp(cdk_canvas_t *c, int x, int y, int w, int h,
                          const uint8_t *bits, uint32_t fg, uint32_t bg)
{
    if (bits == NULL || w <= 0 || h <= 0) {
        return;
    }
    const int stride = (w + 7) / 8;
    int touched_y0 = c->h;
    int touched_y1 = 0;

    for (int row = 0; row < h; row++) {
        const int yy = y + row;
        if (yy < 0 || yy >= c->h) {
            continue;
        }
        const uint8_t *src = bits + (size_t)row * (size_t)stride;
        uint16_t *dst = c->pix + (size_t)yy * (size_t)c->w;
        for (int col = 0; col < w; col++) {
            const int xx = x + col;
            if (xx < 0 || xx >= c->w) {
                continue;
            }
            const bool on = (src[col >> 3] >> (7 - (col & 7))) & 1u;
            if (on) {
                dst[xx] = (uint16_t)fg;
            } else if (bg != CDK_CANVAS_NO_BG) {
                dst[xx] = (uint16_t)bg;
            } else {
                continue;
            }
            if (yy < touched_y0) {
                touched_y0 = yy;
            }
            if (yy + 1 > touched_y1) {
                touched_y1 = yy + 1;
            }
        }
    }
    cdk_canvas_mark_dirty(c, touched_y0, touched_y1);
}

/* ------------------------------------------------------------------ */
/* テストパターン                                                      */
/* ------------------------------------------------------------------ */

void cdk_canvas_testpattern(cdk_canvas_t *c)
{
    const int w = c->w;
    const int h = c->h;

    static const uint8_t bar_rgb[8][3] = {
        {0xFF, 0xFF, 0xFF}, /* 白 */
        {0xFF, 0xFF, 0x00}, /* 黄 */
        {0x00, 0xFF, 0xFF}, /* シアン */
        {0x00, 0xFF, 0x00}, /* 緑 */
        {0xFF, 0x00, 0xFF}, /* マゼンタ */
        {0xFF, 0x00, 0x00}, /* 赤 */
        {0x00, 0x00, 0xFF}, /* 青 */
        {0x00, 0x00, 0x00}, /* 黒 */
    };

    const int band = h / 3;

    cdk_canvas_clear(c, CDK_BLACK);

    /* 上段: カラーバー 8 本 */
    for (int i = 0; i < 8; i++) {
        const int x0 = (int)((long)w * i / 8);
        const int x1 = (int)((long)w * (i + 1) / 8);
        cdk_canvas_fill_rect(c, x0, 0, x1 - x0, band,
                             cdk_rgb565(bar_rgb[i][0], bar_rgb[i][1], bar_rgb[i][2]));
    }

    /* 中段: 横方向のグレースケール勾配。RGB565 の量子化が見える */
    for (int x = 0; x < w; x++) {
        const uint8_t v = (w > 1) ? (uint8_t)((long)x * 255 / (w - 1)) : 0;
        cdk_canvas_fill_rect(c, x, band, 1, band, cdk_rgb565(v, v, v));
    }

    /* 下段: 8x8 の市松模様。転送のストライドずれがあれば一目で分かる */
    for (int y = band * 2; y < h; y++) {
        uint16_t *row = c->pix + (size_t)y * (size_t)w;
        const int cy = (y - band * 2) / 8;
        for (int x = 0; x < w; x++) {
            const int cx = x / 8;
            row[x] = ((cx + cy) & 1) ? CDK_WHITE : cdk_rgb565(0x00, 0x00, 0x80);
        }
    }

    /* 左上から右下への対角線。上下左右の反転や 90 度回転を検出する */
    if (w > 1 && h > 1) {
        for (int x = 0; x < w; x++) {
            const int y = (int)((long)x * (h - 1) / (w - 1));
            c->pix[(size_t)y * (size_t)w + (size_t)x] = CDK_MAGENTA;
        }
    }

    /* 四隅のマーカー。色で向きが分かる */
    cdk_canvas_fill_rect(c, 1, 1, 8, 8, CDK_RED);              /* 左上 = 赤 */
    cdk_canvas_fill_rect(c, w - 9, 1, 8, 8, CDK_GREEN);        /* 右上 = 緑 */
    cdk_canvas_fill_rect(c, 1, h - 9, 8, 8, CDK_BLUE);         /* 左下 = 青 */
    cdk_canvas_fill_rect(c, w - 9, h - 9, 8, 8, CDK_YELLOW);   /* 右下 = 黄 */

    /* 外周 1px の白枠。画面の端が切れていないかを見る */
    cdk_canvas_rect(c, 0, 0, w, h, CDK_WHITE);

    cdk_canvas_mark_all_dirty(c);
}
