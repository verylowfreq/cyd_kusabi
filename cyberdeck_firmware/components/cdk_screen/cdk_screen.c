#include "cdk_screen.h"

#include <string.h>

/* xterm の標準 16 色 */
static const uint8_t k_ansi[16][3] = {
    {0x00, 0x00, 0x00}, {0xCD, 0x00, 0x00}, {0x00, 0xCD, 0x00}, {0xCD, 0xCD, 0x00},
    {0x00, 0x00, 0xEE}, {0xCD, 0x00, 0xCD}, {0x00, 0xCD, 0xCD}, {0xE5, 0xE5, 0xE5},
    {0x7F, 0x7F, 0x7F}, {0xFF, 0x00, 0x00}, {0x00, 0xFF, 0x00}, {0xFF, 0xFF, 0x00},
    {0x5C, 0x5C, 0xFF}, {0xFF, 0x00, 0xFF}, {0x00, 0xFF, 0xFF}, {0xFF, 0xFF, 0xFF},
};

void cdk_screen_init(cdk_screen_t *s, const cdk_font_t *font)
{
    memset(s, 0, sizeof(*s));
    s->font = font;
    s->bold_is_bright = true;
    s->last_cx = -1;
    s->last_cy = -1;

    for (int i = 0; i < 16; i++) {
        s->palette[i] = cdk_rgb565(k_ansi[i][0], k_ansi[i][1], k_ansi[i][2]);
    }
    /* 16-231: 6x6x6 のカラーキューブ */
    static const uint8_t lv[6] = {0, 95, 135, 175, 215, 255};
    for (int r = 0; r < 6; r++) {
        for (int g = 0; g < 6; g++) {
            for (int b = 0; b < 6; b++) {
                s->palette[16 + r * 36 + g * 6 + b] = cdk_rgb565(lv[r], lv[g], lv[b]);
            }
        }
    }
    /* 232-255: グレースケール */
    for (int i = 0; i < 24; i++) {
        const uint8_t v = (uint8_t)(8 + i * 10);
        s->palette[232 + i] = cdk_rgb565(v, v, v);
    }

    s->default_fg = s->palette[7];
    s->default_bg = s->palette[0];
}

int cdk_screen_cols(const cdk_screen_t *s, int canvas_w)
{
    return (s->font->cell_w > 0) ? canvas_w / s->font->cell_w : 0;
}

int cdk_screen_rows(const cdk_screen_t *s, int canvas_h)
{
    return (s->font->cell_h > 0) ? canvas_h / s->font->cell_h : 0;
}

static uint16_t resolve(const cdk_screen_t *s, uint32_t color, bool is_fg)
{
    if (CDK_COLOR_IS_RGB(color)) {
        return cdk_rgb565((uint8_t)(color >> 16), (uint8_t)(color >> 8),
                          (uint8_t)color);
    }
    if (CDK_COLOR_IS_IDX(color)) {
        return s->palette[CDK_COLOR_GET_IDX(color)];
    }
    return is_fg ? s->default_fg : s->default_bg;
}

/* RGB565 のまま各成分を半分にする。DIM 用。 */
static uint16_t dim565(uint16_t c)
{
    return (uint16_t)(((c & 0xF800u) >> 1 & 0xF800u) | ((c & 0x07E0u) >> 1 & 0x07E0u) |
                      ((c & 0x001Fu) >> 1));
}

void cdk_screen_draw_cell(const cdk_screen_t *s, cdk_canvas_t *cv,
                          const cdk_cell_t *cell, int col, int row, bool cursor)
{
    const cdk_font_t *f = s->font;
    const int cw = f->cell_w;
    const int ch = f->cell_h;
    const int x = s->origin_x + col * cw;
    const int y = s->origin_y + row * ch;

    /* 全角の右半分は左半分を描いたときに一緒に塗られている */
    if ((cell->attr & CDK_ATTR_WIDE_TAIL) != 0u) {
        return;
    }
    const bool wide = (cell->attr & CDK_ATTR_WIDE_LEAD) != 0u;
    const int cells = wide ? 2 : 1;

    uint32_t fg_src = cell->fg;
    /* 太字は明るい色で表す。6px 幅では字を太らせても潰れる。 */
    if (s->bold_is_bright && (cell->attr & CDK_ATTR_BOLD) != 0u &&
        CDK_COLOR_IS_IDX(fg_src)) {
        const uint8_t i = CDK_COLOR_GET_IDX(fg_src);
        if (i < 8u) {
            fg_src = CDK_COLOR_IDX(i + 8u);
        }
    }

    uint16_t fg = resolve(s, fg_src, true);
    uint16_t bg = resolve(s, cell->bg, false);

    if ((cell->attr & CDK_ATTR_DIM) != 0u) {
        fg = dim565(fg);
    }
    if ((cell->attr & CDK_ATTR_REVERSE) != 0u) {
        const uint16_t tmp = fg;
        fg = bg;
        bg = tmp;
    }
    if (cursor) {
        const uint16_t tmp = fg;
        fg = bg;
        bg = tmp;
    }
    if ((cell->attr & CDK_ATTR_INVISIBLE) != 0u) {
        fg = bg;
    }

    cdk_canvas_fill_rect(cv, x, y, cw * cells, ch, bg);

    const uint32_t cp = cell->cp;
    if (cp != 0u && cp != (uint32_t)' ') {
        cdk_glyph_t g;
        (void)cdk_font_glyph(f, cp, cells, &g);
        cdk_canvas_blit_1bpp(cv, x, y, g.width, ch, g.bits, fg, CDK_CANVAS_NO_BG);
    }

    if ((cell->attr & CDK_ATTR_UNDERLINE) != 0u) {
        cdk_canvas_hline(cv, x, y + ch - 2, cw * cells, fg);
    }
    if ((cell->attr & CDK_ATTR_STRIKE) != 0u) {
        cdk_canvas_hline(cv, x, y + ch / 2, cw * cells, fg);
    }
}

void cdk_screen_draw(cdk_screen_t *s, cdk_canvas_t *cv, cdk_term_t *t, bool all)
{
    /* カーソルが動いたら、前にいた行（残像を消す）と今いる行（新しく描く）の
     * 両方をダーティにする。移動先が他の理由でダーティでないと描かれない。 */
    if (s->last_cx != t->cx || s->last_cy != t->cy ||
        s->cursor_drawn != t->cursor_visible) {
        if (s->cursor_drawn && s->last_cy >= 0) {
            cdk_term_mark_row(t, s->last_cy);
        }
        if (t->cursor_visible) {
            cdk_term_mark_row(t, t->cy);
        }
    }

    const int cols = (t->cols < cdk_screen_cols(s, cv->w)) ? t->cols
                                                           : cdk_screen_cols(s, cv->w);
    const int rows = (t->rows < cdk_screen_rows(s, cv->h)) ? t->rows
                                                           : cdk_screen_rows(s, cv->h);

    for (int y = 0; y < rows; y++) {
        if (!all && !cdk_term_line_dirty(t, y)) {
            continue;
        }
        const cdk_cell_t *line = cdk_term_row(t, y);
        for (int x = 0; x < cols; x++) {
            const bool cursor = t->cursor_visible && x == t->cx && y == t->cy;
            cdk_screen_draw_cell(s, cv, &line[x], x, y, cursor);
        }
    }

    s->last_cx = t->cx;
    s->last_cy = t->cy;
    s->cursor_drawn = t->cursor_visible;
    cdk_term_clear_dirty(t);
}
