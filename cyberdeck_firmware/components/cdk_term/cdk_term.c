/* cdk_term — 画面バッファの操作。パーサは cdk_vt.c。 */
#include "cdk_term_priv.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* 初期化                                                              */
/* ------------------------------------------------------------------ */

static size_t align4(size_t v)
{
    return (v + 3u) & ~(size_t)3u;
}

size_t cdk_term_mem_size(int cols, int rows)
{
    if (cols <= 0 || rows <= 0) {
        return 0;
    }
    const size_t screen = (size_t)cols * (size_t)rows * sizeof(cdk_cell_t);
    return screen * 2u + align4((size_t)rows) + align4((size_t)cols);
}

static void reset_tabs(cdk_term_t *t)
{
    for (int x = 0; x < t->cols; x++) {
        t->tabs[x] = (x % 8 == 0) ? 1u : 0u;
    }
}

bool cdk_term_init(cdk_term_t *t, int cols, int rows, void *mem, size_t mem_size)
{
    if (t == NULL || mem == NULL || cols <= 0 || rows <= 0) {
        return false;
    }
    const size_t need = cdk_term_mem_size(cols, rows);
    if (mem_size < need) {
        return false;
    }

    memset(t, 0, sizeof(*t));
    t->cols = cols;
    t->rows = rows;

    uint8_t *p = (uint8_t *)mem;
    const size_t screen = (size_t)cols * (size_t)rows * sizeof(cdk_cell_t);
    t->primary = (cdk_cell_t *)(void *)p;
    p += screen;
    t->alternate = (cdk_cell_t *)(void *)p;
    p += screen;
    t->dirty = p;
    p += align4((size_t)rows);
    t->tabs = p;

    t->cells = t->primary;
    cdk_term_reset(t);
    return true;
}

void cdk_term_set_reply(cdk_term_t *t, cdk_term_reply_fn fn, void *user)
{
    t->reply = fn;
    t->reply_user = user;
}

void cdk_term_reset(cdk_term_t *t)
{
    t->cx = 0;
    t->cy = 0;
    t->top = 0;
    t->bot = t->rows - 1;
    t->wrap_pending = false;

    t->fg = CDK_COLOR_DEFAULT;
    t->bg = CDK_COLOR_DEFAULT;
    t->attr = 0;

    t->autowrap = true;
    t->origin_mode = false;
    t->cursor_visible = true;
    t->bracketed_paste = false;
    t->app_cursor_keys = false;
    t->app_keypad = false;
    t->insert_mode = false;
    t->mouse_tracking = false;

    t->charset_g0_dec = false;
    t->charset_g1_dec = true; /* 慣例: G1 に DEC 特殊グラフィクス */
    t->charset_shifted = false;

    memset(&t->saved, 0, sizeof(t->saved));
    memset(&t->saved_alt, 0, sizeof(t->saved_alt));

    t->state = CDK_ST_GROUND;
    t->nparams = 0;
    t->param_seen = false;
    t->intermediate = 0;
    t->private_marker = 0;
    t->osc_len = 0;
    cdk_utf8_reset(&t->utf8);
    t->title[0] = '\0';

    const cdk_cell_t blank = cdk__blank(t);
    const size_t n = (size_t)t->cols * (size_t)t->rows;
    for (size_t i = 0; i < n; i++) {
        t->primary[i] = blank;
        t->alternate[i] = blank;
    }
    t->alt_active = false;
    t->cells = t->primary;

    /* RIS は履歴も消す。領域そのものは呼び出し側のものなので外さない
     * （hist / hist_cap は触らない）。 */
    t->hist_len = 0;
    t->hist_head = 0;
    t->view_off = 0;

    reset_tabs(t);
    cdk_term_mark_all_dirty(t);
}

/* ------------------------------------------------------------------ */
/* ダーティ行                                                          */
/* ------------------------------------------------------------------ */

void cdk__mark(cdk_term_t *t, int y)
{
    if (y >= 0 && y < t->rows) {
        t->dirty[y] = 1u;
    }
}

void cdk__mark_range(cdk_term_t *t, int y0, int y1)
{
    if (y0 < 0) {
        y0 = 0;
    }
    if (y1 > t->rows - 1) {
        y1 = t->rows - 1;
    }
    for (int y = y0; y <= y1; y++) {
        t->dirty[y] = 1u;
    }
}

void cdk_term_mark_row(cdk_term_t *t, int y)
{
    cdk__mark(t, y);
}

void cdk_term_mark_all_dirty(cdk_term_t *t)
{
    memset(t->dirty, 1, (size_t)t->rows);
}

void cdk_term_clear_dirty(cdk_term_t *t)
{
    memset(t->dirty, 0, (size_t)t->rows);
}

/* ------------------------------------------------------------------ */
/* セル操作                                                            */
/* ------------------------------------------------------------------ */

cdk_cell_t cdk__blank(const cdk_term_t *t)
{
    cdk_cell_t c;
    c.cp = 0;
    c.fg = CDK_COLOR_DEFAULT;
    /* 消去は現在の背景色で塗る（modern terminal の慣例）。
     * これをしないと「画面全体を青にしてから clear」が白抜けになる。 */
    c.bg = t->bg;
    c.attr = 0;
    c._pad = 0;
    return c;
}

static cdk_cell_t *at(cdk_term_t *t, int x, int y)
{
    return t->cells + (size_t)y * (size_t)t->cols + (size_t)x;
}

void cdk__clear_cell(cdk_term_t *t, int x, int y)
{
    if (x < 0 || x >= t->cols || y < 0 || y >= t->rows) {
        return;
    }
    cdk_cell_t *c = at(t, x, y);
    const cdk_cell_t blank = cdk__blank(t);

    /* 全角ペアの片方だけを消すと孤児が残る。相方もまとめて空白にする。 */
    if ((c->attr & CDK_ATTR_WIDE_LEAD) != 0u && x + 1 < t->cols) {
        *at(t, x + 1, y) = blank;
    } else if ((c->attr & CDK_ATTR_WIDE_TAIL) != 0u && x > 0) {
        *at(t, x - 1, y) = blank;
    }
    *c = blank;
    cdk__mark(t, y);
}

void cdk__fill(cdk_term_t *t, int x0, int y0, int x1, int y1)
{
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > t->cols - 1) {
        x1 = t->cols - 1;
    }
    if (y1 > t->rows - 1) {
        y1 = t->rows - 1;
    }
    if (x0 > x1 || y0 > y1) {
        return;
    }
    const cdk_cell_t blank = cdk__blank(t);
    for (int y = y0; y <= y1; y++) {
        /* 範囲の端で全角ペアが割れる場合は相方も潰す */
        cdk__clear_cell(t, x0, y);
        cdk__clear_cell(t, x1, y);
        cdk_cell_t *row = at(t, 0, y);
        for (int x = x0; x <= x1; x++) {
            row[x] = blank;
        }
        cdk__mark(t, y);
    }
}

/* ------------------------------------------------------------------ */
/* スクロール                                                          */
/* ------------------------------------------------------------------ */

void cdk__scroll_up(cdk_term_t *t, int n)
{
    const int height = t->bot - t->top + 1;
    if (n <= 0 || height <= 0) {
        return;
    }
    if (n > height) {
        n = height;
    }
    const size_t stride = (size_t)t->cols;
    if (n < height) {
        memmove(at(t, 0, t->top), at(t, 0, t->top + n),
                (size_t)(height - n) * stride * sizeof(cdk_cell_t));
    }
    cdk__fill(t, 0, t->bot - n + 1, t->cols - 1, t->bot);
    cdk__mark_range(t, t->top, t->bot);
}

void cdk__scroll_down(cdk_term_t *t, int n)
{
    const int height = t->bot - t->top + 1;
    if (n <= 0 || height <= 0) {
        return;
    }
    if (n > height) {
        n = height;
    }
    const size_t stride = (size_t)t->cols;
    if (n < height) {
        memmove(at(t, 0, t->top + n), at(t, 0, t->top),
                (size_t)(height - n) * stride * sizeof(cdk_cell_t));
    }
    cdk__fill(t, 0, t->top, t->cols - 1, t->top + n - 1);
    cdk__mark_range(t, t->top, t->bot);
}

void cdk__index(cdk_term_t *t)
{
    if (t->cy == t->bot) {
        /* 画面から出ていく行を履歴へ。**scroll_up の memmove で潰れる前に。** */
        cdk__hist_push(t, 1);
        cdk__scroll_up(t, 1);
    } else if (t->cy < t->rows - 1) {
        t->cy++;
    }
}

void cdk__reverse_index(cdk_term_t *t)
{
    if (t->cy == t->top) {
        cdk__scroll_down(t, 1);
    } else if (t->cy > 0) {
        t->cy--;
    }
}

/* ------------------------------------------------------------------ */
/* カーソル                                                            */
/* ------------------------------------------------------------------ */

void cdk__move_to(cdk_term_t *t, int x, int y)
{
    if (t->origin_mode) {
        y += t->top;
        if (y < t->top) {
            y = t->top;
        }
        if (y > t->bot) {
            y = t->bot;
        }
    } else {
        if (y < 0) {
            y = 0;
        }
        if (y > t->rows - 1) {
            y = t->rows - 1;
        }
    }
    if (x < 0) {
        x = 0;
    }
    if (x > t->cols - 1) {
        x = t->cols - 1;
    }
    t->cx = x;
    t->cy = y;
    t->wrap_pending = false;
}

/* ------------------------------------------------------------------ */
/* 文字の書き込み                                                      */
/* ------------------------------------------------------------------ */

void cdk__put_char(cdk_term_t *t, uint32_t cp)
{
    int w = cdk_char_width(cp);
    if (w == 0) {
        /* 結合文字。M1 では前のセルに合成しないので捨てる。
         * 幅 0 なので桁はずれない。 */
        return;
    }

    /* 遅延折返し: 右端に文字を書いた時点では折り返さず、
     * 次の文字が来たときに初めて改行する。CLI の再描画がこれに依存している。 */
    if (t->wrap_pending) {
        if (t->autowrap) {
            t->cx = 0;
            cdk__index(t);
        }
        t->wrap_pending = false;
    }

    /* 全角が右端 1 セルに入りきらない場合は次の行へ送る */
    if (w == 2 && t->cx + 1 > t->cols - 1) {
        if (t->autowrap) {
            cdk__clear_cell(t, t->cx, t->cy);
            t->cx = 0;
            cdk__index(t);
        } else {
            return;
        }
    }

    if (t->insert_mode) {
        cdk__insert_chars(t, w);
    }

    /* 書き込み先に既存の全角ペアが掛かっていたら先に潰す */
    cdk__clear_cell(t, t->cx, t->cy);
    if (w == 2) {
        cdk__clear_cell(t, t->cx + 1, t->cy);
    }

    cdk_cell_t *c = at(t, t->cx, t->cy);
    c->cp = cp;
    c->fg = t->fg;
    c->bg = t->bg;
    c->attr = t->attr | (w == 2 ? CDK_ATTR_WIDE_LEAD : 0u);

    if (w == 2) {
        cdk_cell_t *c2 = at(t, t->cx + 1, t->cy);
        c2->cp = 0;
        c2->fg = t->fg;
        c2->bg = t->bg;
        c2->attr = t->attr | CDK_ATTR_WIDE_TAIL;
    }
    cdk__mark(t, t->cy);

    t->cx += w;
    if (t->cx > t->cols - 1) {
        t->cx = t->cols - 1;
        t->wrap_pending = true;
    }
}

/* ------------------------------------------------------------------ */
/* 挿入・削除                                                          */
/* ------------------------------------------------------------------ */

void cdk__insert_chars(cdk_term_t *t, int n)
{
    if (n <= 0) {
        return;
    }
    const int avail = t->cols - t->cx;
    if (n > avail) {
        n = avail;
    }
    cdk_cell_t *row = at(t, 0, t->cy);
    if (n < avail) {
        memmove(row + t->cx + n, row + t->cx,
                (size_t)(avail - n) * sizeof(cdk_cell_t));
    }
    const cdk_cell_t blank = cdk__blank(t);
    for (int x = t->cx; x < t->cx + n; x++) {
        row[x] = blank;
    }
    cdk__mark(t, t->cy);
}

void cdk__delete_chars(cdk_term_t *t, int n)
{
    if (n <= 0) {
        return;
    }
    const int avail = t->cols - t->cx;
    if (n > avail) {
        n = avail;
    }
    cdk_cell_t *row = at(t, 0, t->cy);
    if (n < avail) {
        memmove(row + t->cx, row + t->cx + n,
                (size_t)(avail - n) * sizeof(cdk_cell_t));
    }
    const cdk_cell_t blank = cdk__blank(t);
    for (int x = t->cols - n; x < t->cols; x++) {
        row[x] = blank;
    }
    cdk__mark(t, t->cy);
}

void cdk__erase_chars(cdk_term_t *t, int n)
{
    if (n <= 0) {
        return;
    }
    int x1 = t->cx + n - 1;
    if (x1 > t->cols - 1) {
        x1 = t->cols - 1;
    }
    cdk__fill(t, t->cx, t->cy, x1, t->cy);
}

void cdk__insert_lines(cdk_term_t *t, int n)
{
    if (t->cy < t->top || t->cy > t->bot || n <= 0) {
        return;
    }
    /* スクロール領域の上端を一時的にカーソル行にして下方向へ送る */
    const int save_top = t->top;
    t->top = t->cy;
    cdk__scroll_down(t, n);
    t->top = save_top;
    t->cx = 0;
    t->wrap_pending = false;
}

void cdk__delete_lines(cdk_term_t *t, int n)
{
    if (t->cy < t->top || t->cy > t->bot || n <= 0) {
        return;
    }
    const int save_top = t->top;
    t->top = t->cy;
    cdk__scroll_up(t, n);
    t->top = save_top;
    t->cx = 0;
    t->wrap_pending = false;
}

/* ------------------------------------------------------------------ */
/* 代替画面                                                            */
/* ------------------------------------------------------------------ */

void cdk__switch_screen(cdk_term_t *t, bool to_alt)
{
    if (to_alt == t->alt_active) {
        return;
    }
    t->alt_active = to_alt;
    t->cells = to_alt ? t->alternate : t->primary;
    /* **alt_active が変わる唯一の場所なので、view はここで戻す。**
     * 47 / 1047 / 1049 のどれで入っても取りこぼさない。
     * 代替画面では履歴を積まないので、戻るときも 0 のままでよい。 */
    t->view_off = 0;
    cdk_term_mark_all_dirty(t);
}

void cdk__reply(cdk_term_t *t, const char *s)
{
    if (t->reply != NULL && s != NULL) {
        t->reply(t->reply_user, (const uint8_t *)s, strlen(s));
    }
}

/* ------------------------------------------------------------------ */
/* 入力                                                                */
/* ------------------------------------------------------------------ */

void cdk_term_feed(cdk_term_t *t, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        cdk__vt_byte(t, p[i]);
    }
}

/* ------------------------------------------------------------------ */
/* テスト用                                                            */
/* ------------------------------------------------------------------ */

size_t cdk_term_row_text(const cdk_term_t *t, int y, char *out, size_t out_cap)
{
    if (out_cap == 0) {
        return 0;
    }
    if (y < 0 || y >= t->rows) {
        out[0] = '\0';
        return 0;
    }
    const cdk_cell_t *row = cdk_term_row(t, y);
    size_t n = 0;
    for (int x = 0; x < t->cols; x++) {
        if ((row[x].attr & CDK_ATTR_WIDE_TAIL) != 0u) {
            continue;
        }
        const uint32_t cp = (row[x].cp == 0) ? (uint32_t)' ' : row[x].cp;
        uint8_t buf[4];
        const size_t k = cdk_utf8_encode(cp, buf);
        if (n + k + 1 > out_cap) {
            break;
        }
        memcpy(out + n, buf, k);
        n += k;
    }
    out[n] = '\0';
    return n;
}
