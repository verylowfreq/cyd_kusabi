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

    /* 地の文は純白 (palette[15])。xterm の既定は palette[7] = 0xE5E5E5 で、
     * これは白ではなく 90% の灰色。実機の ILI9488 だと灰色にしか見えず、
     * 「白がやたらに暗い」と感じる原因になっていた。
     *
     * パレット自体は xterm 標準のまま触っていないので、**明示的な SGR 37 は
     * 今も 0xE5E5E5**。37 と 97（明るい白）の区別も残る。
     *
     * なお純白はこのパネルで出せる上限で、ドライバの RGB565→RGB666 変換は
     * 0xFFFF を 63/63/63 に写す（変換で落ちてはいない）。バックライトも
     * 起動時から 100%。これ以上明るくする余地はファーム側には無い。 */
    s->default_fg = s->palette[15];
    s->default_bg = s->palette[0];

    cdk_screen_set_gamma(s, 10u); /* 既定は無補正。基板ごとの値は呼び出し側が入れる */
    s->scale = 1;
}

void cdk_screen_set_scale(cdk_screen_t *s, uint8_t scale)
{
    s->scale = (scale >= 1u) ? scale : 1u;
}

int cdk_screen_cell_w(const cdk_screen_t *s)
{
    const int sc = (s->scale > 0) ? s->scale : 1;
    return s->font->cell_w * sc;
}

int cdk_screen_cell_h(const cdk_screen_t *s)
{
    const int sc = (s->scale > 0) ? s->scale : 1;
    return s->font->cell_h * sc;
}

int cdk_screen_cols(const cdk_screen_t *s, int canvas_w)
{
    const int cw = cdk_screen_cell_w(s);
    return (cw > 0) ? canvas_w / cw : 0;
}

int cdk_screen_rows(const cdk_screen_t *s, int canvas_h)
{
    const int ch = cdk_screen_cell_h(s);
    return (ch > 0) ? canvas_h / ch : 0;
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

/* ---- ガンマ補正 --------------------------------------------------
 *
 * このパネルは中間調が沈んで、少し暗いだけの色がまったく読めなくなる。
 * 0 と最大値を固定したまま中間だけ持ち上げる。
 *
 * **浮動小数を使わない。** `powf` の最終ビットはライブラリ実装によって
 * 違いうるので、ホストの基準画像と実機の画面が 1 ビット違う、という形で
 * `shot --verify` がたまに落ちることになる。整数だけで書けば、
 * 同じ C コードが必ず同じ値を出す。
 */

/* 16.16 固定小数の平方根 */
static uint32_t fx_sqrt(uint32_t x)
{
    uint64_t v = (uint64_t)x << 16; /* sqrt(x/2^16)*2^16 = sqrt(x*2^16) */
    uint64_t r = 0;
    uint64_t bit = (uint64_t)1 << 46;

    while (bit > v) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)r;
}

/* x^e。x, e とも 16.16 で 0..1 の範囲。
 * e を 2 進展開して、x の 2^-k 乗（k 回の平方根）を掛け合わせる。 */
static uint32_t fx_pow(uint32_t x, uint32_t e)
{
    uint32_t result = 1u << 16;
    uint32_t cur = x;

    for (int k = 0; k < 16; k++) {
        cur = fx_sqrt(cur);
        if (((e >> (15 - k)) & 1u) != 0u) {
            result = (uint32_t)(((uint64_t)result * (uint64_t)cur) >> 16);
        }
    }
    return result;
}

static void build_lut(uint8_t *lut, unsigned max, uint32_t e)
{
    for (unsigned i = 0; i <= max; i++) {
        const uint32_t x = (uint32_t)(((uint64_t)i << 16) / max);
        const uint32_t y = fx_pow(x, e);
        /* 四捨五入して同じビット幅へ戻す */
        lut[i] = (uint8_t)(((uint64_t)y * max + (1u << 15)) >> 16);
    }
}

void cdk_screen_set_gamma(cdk_screen_t *s, uint8_t gamma_x10)
{
    s->gamma_x10 = gamma_x10;

    if (gamma_x10 <= 10u) {
        /* 無補正。恒等の対応表にしておけば、描画側は分岐しなくてよい。 */
        for (unsigned i = 0; i < 32u; i++) {
            s->lut5[i] = (uint8_t)i;
        }
        for (unsigned i = 0; i < 64u; i++) {
            s->lut6[i] = (uint8_t)i;
        }
        return;
    }

    /* 指数は 1/gamma。gamma > 1 なので必ず 1.0 未満に収まる。 */
    const uint32_t e = (uint32_t)((10u << 16) / gamma_x10);
    build_lut(s->lut5, 31u, e);
    build_lut(s->lut6, 63u, e);
}

/* セルの色が全部決まったあとに掛ける。 */
static uint16_t gamma565(const cdk_screen_t *s, uint16_t c)
{
    const unsigned r = s->lut5[(c >> 11) & 0x1Fu];
    const unsigned g = s->lut6[(c >> 5) & 0x3Fu];
    const unsigned b = s->lut5[c & 0x1Fu];
    return (uint16_t)((r << 11) | (g << 5) | b);
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
    /* **セルの大きさは倍率込み。** グリフのビットマップは元の大きさのまま
     * なので、描くときだけ scale 倍に広げる（cdk_canvas_blit_1bpp_scaled）。 */
    const int sc = (s->scale > 0) ? s->scale : 1;
    const int cw = f->cell_w * sc;
    const int ch = f->cell_h * sc;
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

    /* ここで初めてパネル補正を掛ける。dim も反転もカーソルも済んだあとなので、
     * 「論理的な色を決めてから、パネルの都合で持ち上げる」順序になる。 */
    fg = gamma565(s, fg);
    bg = gamma565(s, bg);

    cdk_canvas_fill_rect(cv, x, y, cw * cells, ch, bg);

    const uint32_t cp = cell->cp;
    if (cp != 0u && cp != (uint32_t)' ') {
        cdk_glyph_t g;
        (void)cdk_font_glyph(f, cp, cells, &g);
        cdk_canvas_blit_1bpp_scaled(cv, x, y, g.width, f->cell_h, g.bits, fg,
                                    CDK_CANVAS_NO_BG, sc);
    }

    /* 下線と取り消し線も倍率ぶん太らせる。1 画素のままだと、
     * 字が 2 倍になったときに線だけ細くて浮いて見える。 */
    if ((cell->attr & CDK_ATTR_UNDERLINE) != 0u) {
        for (int i = 0; i < sc; i++) {
            cdk_canvas_hline(cv, x, y + ch - 2 * sc + i, cw * cells, fg);
        }
    }
    if ((cell->attr & CDK_ATTR_STRIKE) != 0u) {
        for (int i = 0; i < sc; i++) {
            cdk_canvas_hline(cv, x, y + ch / 2 + i, cw * cells, fg);
        }
    }
}

void cdk_screen_set_preedit(cdk_screen_t *s, const char *utf8)
{
    if (utf8 == NULL) {
        utf8 = "";
    }
    if (strcmp(s->preedit, utf8) == 0) {
        return;
    }
    size_t n = strlen(utf8);
    if (n >= sizeof(s->preedit)) {
        /* 入りきらないときは**後ろ**を残す。いま打っている端が見えないと
         * 何を入力しているのか分からなくなるため。UTF-8 の途中で切らない。 */
        size_t skip = n - (sizeof(s->preedit) - 1);
        while (skip < n && ((unsigned char)utf8[skip] & 0xC0u) == 0x80u) {
            skip++;
        }
        utf8 += skip;
        n -= skip;
    }
    memcpy(s->preedit, utf8, n);
    s->preedit[n] = '\0';
    s->preedit_changed = true;
}

/* 未確定文字列の桁数を数える。全角は 2 桁。 */
static int preedit_cells(const char *s)
{
    cdk_utf8_t dec = {0};
    int        cells = 0;
    for (const char *p = s; *p != '\0'; p++) {
        uint32_t cp = 0;
        bool     retry = false;
        if (cdk_utf8_feed(&dec, (uint8_t)*p, &cp, &retry) == CDK_UTF8_OK) {
            cells += cdk_char_width(cp);
        }
    }
    return cells;
}

/* カーソル位置から未確定文字列を重ねて描く。 */
static void draw_preedit(cdk_screen_t *s, cdk_canvas_t *cv, cdk_term_t *t)
{
    const int cols = cdk_screen_cols(s, cv->w);
    const int rows = cdk_screen_rows(s, cv->h);
    if (cols <= 0 || rows <= 0) {
        return;
    }

    const int width = preedit_cells(s->preedit);
    int       row = (t->cy < rows) ? t->cy : rows - 1;
    int       col = t->cx;

    /* 右端に収まらないなら左へ寄せる。行をまたいで折り返すと、
     * 下の行の内容を消したうえに復帰が面倒になる。 */
    if (col + width > cols) {
        col = cols - width;
    }
    if (col < 0) {
        col = 0;
    }

    cdk_utf8_t dec = {0};
    int        x = col;
    for (const char *p = s->preedit; *p != '\0'; p++) {
        uint32_t cp = 0;
        bool     retry = false;
        if (cdk_utf8_feed(&dec, (uint8_t)*p, &cp, &retry) != CDK_UTF8_OK) {
            continue;
        }
        const int w = cdk_char_width(cp);
        if (x + w > cols) {
            break;
        }
        cdk_cell_t cell = {0};
        cell.cp = cp;
        cell.fg = s->default_fg;
        cell.bg = s->default_bg;
        cell.attr = CDK_ATTR_REVERSE;
        if (w == 2) {
            cell.attr |= CDK_ATTR_WIDE_LEAD;
        }
        cdk_screen_draw_cell(s, cv, &cell, x, row, false);
        x += w;
    }

    s->preedit_row = row;
    s->preedit_cells = width;
    s->preedit_drawn = true;
}

void cdk_screen_draw(cdk_screen_t *s, cdk_canvas_t *cv, cdk_term_t *t, bool all)
{
    /* 未確定文字列が変わったら、前に描いた行と今の行を描き直す。
     * 前の行を消し忘れると、短くなったぶんの字が画面に残る。 */
    if (s->preedit_changed) {
        if (s->preedit_drawn && s->preedit_row >= 0 && s->preedit_row < t->rows) {
            cdk_term_mark_row(t, s->preedit_row);
        }
        if (t->cy >= 0 && t->cy < t->rows) {
            cdk_term_mark_row(t, t->cy);
        }
        s->preedit_changed = false;
        if (s->preedit[0] == '\0') {
            s->preedit_drawn = false;
        }
    }

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

    /* 未確定文字列は毎回いちばん上に描く。下の行が描き直されたときに
     * 消えてしまわないよう、条件を付けずに重ねる。 */
    if (s->preedit[0] != '\0') {
        draw_preedit(s, cv, t);
    }

    s->last_cx = t->cx;
    s->last_cy = t->cy;
    s->cursor_drawn = t->cursor_visible;
    cdk_term_clear_dirty(t);
}
