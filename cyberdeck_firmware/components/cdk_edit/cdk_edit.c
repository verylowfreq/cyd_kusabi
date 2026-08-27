/* 簡易テキストエディタ。設計は cdk_edit.h の頭。 */
#include "cdk_edit.h"

#include "cdk_text.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* 出力                                                                */
/* ------------------------------------------------------------------ */

static void ed_write(cdk_edit_t *e, const char *s, size_t n)
{
    if (e->out != NULL && n > 0) {
        e->out(e->out_user, s, n);
    }
}

static void ed_str(cdk_edit_t *e, const char *s)
{
    ed_write(e, s, strlen(s));
}

static void ed_printf(cdk_edit_t *e, const char *fmt, ...)
{
    char    buf[256];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        const size_t m = (n < (int)sizeof(buf)) ? (size_t)n : sizeof(buf) - 1;
        ed_write(e, buf, m);
    }
}

/* ------------------------------------------------------------------ */
/* UTF-8 の小道具                                                      */
/* ------------------------------------------------------------------ */

static uint32_t char_at(const cdk_edit_t *e, size_t off, size_t *clen)
{
    const unsigned char *p = (const unsigned char *)e->buf + off;
    const size_t         avail = (off < e->len) ? (e->len - off) : 0;

    if (avail == 0) {
        *clen = 0;
        return 0;
    }
    if (p[0] < 0x80u) {
        *clen = 1;
        return p[0];
    }
    if ((p[0] & 0xE0u) == 0xC0u && avail >= 2) {
        *clen = 2;
        return (uint32_t)(((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu));
    }
    if ((p[0] & 0xF0u) == 0xE0u && avail >= 3) {
        *clen = 3;
        return (uint32_t)(((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) |
                          (p[2] & 0x3Fu));
    }
    if ((p[0] & 0xF8u) == 0xF0u && avail >= 4) {
        *clen = 4;
        return (uint32_t)(((p[0] & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) |
                          ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu));
    }
    *clen = 1;
    return 0xFFFDu;
}

static size_t prev_char(const cdk_edit_t *e, size_t off)
{
    if (off == 0) {
        return 0;
    }
    size_t i = off - 1;
    while (i > 0 && ((unsigned char)e->buf[i] & 0xC0u) == 0x80u) {
        i--;
    }
    return i;
}

static size_t line_start(const cdk_edit_t *e, size_t off)
{
    while (off > 0 && e->buf[off - 1] != '\n') {
        off--;
    }
    return off;
}

/* NUL 終端の UTF-8 を max_cols 桁で切って出す。全角は途中で切らない。 */
static void ed_clipped(cdk_edit_t *e, const char *s, int max_cols)
{
    const size_t slen = strlen(s);
    size_t       i = 0;
    int          col = 0;
    while (i < slen) {
        const unsigned char c = (unsigned char)s[i];
        size_t              clen = 1;
        uint32_t            cp = c;
        if (c >= 0xF0u) {
            clen = 4;
        } else if (c >= 0xE0u) {
            clen = 3;
        } else if (c >= 0xC0u) {
            clen = 2;
        }
        if (i + clen > slen) {
            break;
        }
        if (clen == 2) {
            cp = (uint32_t)(((c & 0x1Fu) << 6) | ((unsigned char)s[i + 1] & 0x3Fu));
        } else if (clen == 3) {
            cp = (uint32_t)(((c & 0x0Fu) << 12) |
                            (((unsigned char)s[i + 1] & 0x3Fu) << 6) |
                            ((unsigned char)s[i + 2] & 0x3Fu));
        } else if (clen == 4) {
            cp = 0x10000u; /* 幅の判定にしか使わない */
        }
        const int w = cdk_char_width(cp);
        if (col + w > max_cols) {
            break;
        }
        ed_write(e, s + i, clen);
        col += w;
        i += clen;
    }
}

/* ------------------------------------------------------------------ */
/* 表示行                                                              */
/* ------------------------------------------------------------------ */
/*
 * **行の切れ目を決めるのは wrap_break() だけ。** 描画も上下移動も
 * スクロールもこの 1 つの判断を通す。折り返し位置を別の場所でもう一度
 * 計算すると、全角が右端に来たときにカーソルだけ違う場所へ行く。
 */

/* いま col 桁まで埋まっているところへ幅 w のものを置くと折り返すか。
 *
 * 本文の末尾にいるカーソルも**幅 1 の居場所**として同じ判定を通す。
 * 行がちょうど右端まで埋まっているとき、カーソルは次の行の頭に出る。
 * ここを特別扱いすると、描画と上下移動で行の切れ目の判断が食い違う。 */
static bool wrap_break(int col, int w, int cols)
{
    return col > 0 && col + w > cols;
}

/* UTF-8 文字列を max_cols 桁で切って出す。
 *
 * ステータス行は端末の最下行に書く。**右端を超えると端末が改行し、
 * 画面全体が 1 行ぶんスクロールする**。長いメッセージを出した瞬間に
 * 本文が上へ流れて消えるので、必ず切ってから出すこと。 */
static void ed_clipped(cdk_edit_t *e, const char *s, int max_cols);

/* row_start から始まる表示行の中で、桁 want に最も近い位置。
 *
 * **この行から出ない。** 折り返した行がちょうど右端まで埋まっていると、
 * 「行の末尾」と「次の行の先頭」が同じオフセットになる。そこを返すと、
 * 上へ動いたつもりが元の行に戻ってきて**カーソルが動かなくなる**。
 * 行が埋まっているときは最後の文字の先頭に留める。 */
static size_t offset_within(const cdk_edit_t *e, size_t row_start, int want)
{
    size_t off = row_start;
    int    col = 0;
    for (;;) {
        if (off >= e->len) {
            /* 本文の末尾。行が埋まっているならこの位置は次の表示行。 */
            if (wrap_break(col, 1, e->cols) && off > row_start) {
                off = prev_char(e, off);
            }
            break;
        }
        size_t         clen = 0;
        const uint32_t cp = char_at(e, off, &clen);
        if (cp == (uint32_t)'\n') {
            break; /* 改行の手前は行内の正しい位置 */
        }
        const int w = cdk_char_width(cp);
        if (wrap_break(col, w, e->cols)) {
            /* この文字は次の表示行。いまの行に留まる。 */
            if (off > row_start) {
                off = prev_char(e, off);
            }
            break;
        }
        if (col + w > want) {
            break;
        }
        col += w;
        off += clen;
    }
    return off;
}

/* row_start から off までの桁数。 */
static int col_within(const cdk_edit_t *e, size_t row_start, size_t off)
{
    size_t i = row_start;
    int    col = 0;
    while (i < off && i < e->len) {
        size_t   clen = 0;
        uint32_t cp = char_at(e, i, &clen);
        if (cp == (uint32_t)'\n') {
            break;
        }
        const int w = cdk_char_width(cp);
        if (wrap_break(col, w, e->cols)) {
            break;
        }
        col += w;
        i += clen;
    }
    return col;
}

/* off を含む表示行の先頭。 */
static size_t row_start_of(const cdk_edit_t *e, size_t off)
{
    const size_t ls = line_start(e, off);
    size_t       best = ls;
    size_t       i = ls;
    int          col = 0;
    while (i < off && i < e->len) {
        size_t   clen = 0;
        uint32_t cp = char_at(e, i, &clen);
        if (cp == (uint32_t)'\n') {
            break;
        }
        const int w = cdk_char_width(cp);
        if (wrap_break(col, w, e->cols)) {
            col = 0;
            best = i;
        }
        col += w;
        i += clen;
    }
    /* 本文の末尾で、行がちょうど右端まで埋まっている場合。
     * カーソルの居場所が無いので、そこが次の表示行の先頭になる。 */
    if (off >= e->len && wrap_break(col, 1, e->cols)) {
        best = off;
    }
    return best;
}

/* rs の次の表示行の先頭。無ければ rs のまま返す。 */
static size_t next_row_start(const cdk_edit_t *e, size_t rs)
{
    size_t off = rs;
    int    col = 0;
    while (off < e->len) {
        size_t   clen = 0;
        uint32_t cp = char_at(e, off, &clen);
        if (cp == (uint32_t)'\n') {
            return off + 1;
        }
        const int w = cdk_char_width(cp);
        if (wrap_break(col, w, e->cols)) {
            return off;
        }
        col += w;
        off += clen;
    }
    /* 末尾で右端ちょうどなら、カーソルのための空の行がもう 1 つある。 */
    if (wrap_break(col, 1, e->cols)) {
        return e->len;
    }
    return rs;
}

/* rs の 1 つ前の表示行の先頭。無ければ rs のまま返す。 */
static size_t prev_row_start(const cdk_edit_t *e, size_t rs)
{
    if (rs == 0) {
        return 0;
    }
    return row_start_of(e, prev_char(e, rs));
}

/* ------------------------------------------------------------------ */
/* 走査（描画とカーソル位置）                                          */
/* ------------------------------------------------------------------ */

static void walk(cdk_edit_t *e, bool emit)
{
    const int max_rows = (e->text_rows < CDK_EDIT_ROWS_MAX) ? e->text_rows
                                                            : CDK_EDIT_ROWS_MAX;
    size_t off = e->top;
    int    row = 0;
    int    col = 0;

    e->nrows = 0;
    e->cur_row = -1;
    e->cur_col = 0;
    if (max_rows <= 0) {
        return;
    }
    e->rowstart[0] = off;
    e->nrows = 1;

    if (emit) {
        ed_str(e, "\033[1;1H\033[K");
    }

    for (;;) {
        size_t   clen = 0;
        uint32_t cp = 0;
        int      w = 0;

        bool need_wrap = false;
        if (off < e->len) {
            cp = char_at(e, off, &clen);
            if (cp != (uint32_t)'\n') {
                w = cdk_char_width(cp);
                need_wrap = wrap_break(col, w, e->cols);
            }
        } else {
            /* 末尾のカーソルにも 1 桁の居場所が要る。row_start_of と
             * next_row_start も同じ判定をしている。 */
            need_wrap = wrap_break(col, 1, e->cols);
        }
        if (need_wrap) {
            row++;
            col = 0;
            if (row >= max_rows) {
                break;
            }
            e->rowstart[row] = off;
            e->nrows = row + 1;
            if (emit) {
                ed_printf(e, "\033[%d;1H\033[K", row + 1);
            }
        }

        /* カーソルの記録は**折り返しを決めたあと**。先に記録すると、
         * 右端で折り返す文字の上にカーソルが残る。 */
        if (off == e->cursor) {
            e->cur_row = row;
            e->cur_col = col;
        }
        if (off >= e->len) {
            break;
        }

        if (cp == (uint32_t)'\n') {
            off += 1;
            row++;
            col = 0;
            if (row >= max_rows) {
                break;
            }
            e->rowstart[row] = off;
            e->nrows = row + 1;
            if (emit) {
                ed_printf(e, "\033[%d;1H\033[K", row + 1);
            }
            continue;
        }

        if (emit) {
            ed_write(e, e->buf + off, clen);
        }
        col += w;
        off += clen;
    }

    if (emit) {
        for (int r = e->nrows; r < max_rows; r++) {
            ed_printf(e, "\033[%d;1H\033[K", r + 1);
        }
    }
}

/* カーソルが画面に入るまで top を動かす。 */
static void ensure_visible(cdk_edit_t *e)
{
    const size_t crs = row_start_of(e, e->cursor);

    if (crs < e->top) {
        e->top = crs;
    }
    for (int guard = 0; guard < CDK_EDIT_ROWS_MAX * 64; guard++) {
        walk(e, false);
        if (e->cur_row >= 0) {
            return;
        }
        const size_t next = next_row_start(e, e->top);
        if (next == e->top) {
            return;
        }
        e->top = next;
    }
}

/* ------------------------------------------------------------------ */
/* 生成                                                                */
/* ------------------------------------------------------------------ */

void cdk_edit_init(cdk_edit_t *e, char *arena, size_t cap, cdk_edit_out_fn out,
                   void *user)
{
    memset(e, 0, sizeof(*e));
    e->buf = arena;
    e->cap = cap;
    e->out = out;
    e->out_user = user;
    e->mode_label = "";
    cdk_edit_set_size(e, 80, 25);
}

void cdk_edit_set_size(cdk_edit_t *e, int cols, int rows)
{
    e->cols = (cols > 1) ? cols : 2;
    e->rows = (rows > 1) ? rows : 2;
    e->text_rows = e->rows - 1; /* 最下行はステータス */
    if (e->text_rows > CDK_EDIT_ROWS_MAX) {
        e->text_rows = CDK_EDIT_ROWS_MAX;
    }
}

void cdk_edit_new(cdk_edit_t *e, const char *path)
{
    e->len = 0;
    e->cursor = 0;
    e->top = 0;
    e->modified = false;
    e->readonly = false;
    e->want_quit = false;
    e->want_save = false;
    e->quit_confirm = false;
    e->want_col = 0;
    e->msg[0] = '\0';
    snprintf(e->path, sizeof(e->path), "%s", (path != NULL) ? path : "");
}

/* 入りきらないものは**読み込まない**。途中まで読んで保存すると、その時点で
 * ファイルの後半が消える。切り詰めたものを黙って持つのはエディタではなく
 * シュレッダー。 */
static bool accept_len(cdk_edit_t *e, const char *path, size_t len)
{
    if (len > e->cap) {
        cdk_edit_new(e, path);
        e->readonly = true;
        snprintf(e->msg, sizeof(e->msg), "大きすぎます (%u > %u バイト)",
                 (unsigned)len, (unsigned)e->cap);
        return false;
    }
    return true;
}

/* 領域がほとんど残っていないなら、その場で言う。黙っていると
 * 「開けるのに 1 文字も打てない」という理由の分からない状態になる。 */
static void warn_if_full(cdk_edit_t *e)
{
    if (e->len + 1024 > e->cap) {
        snprintf(e->msg, sizeof(e->msg), "残り %u バイト。ほとんど書けません",
                 (unsigned)(e->cap - e->len));
    }
}

bool cdk_edit_load(cdk_edit_t *e, const char *path, const char *data, size_t len)
{
    if (!accept_len(e, path, len)) {
        return false;
    }
    cdk_edit_new(e, path);
    if (len > 0 && data != NULL) {
        memcpy(e->buf, data, len);
    }
    e->len = len;
    warn_if_full(e);
    return true;
}

bool cdk_edit_adopt(cdk_edit_t *e, const char *path, size_t len)
{
    if (!accept_len(e, path, len)) {
        return false;
    }
    /* cdk_edit_new は長さと状態を戻すだけで buf の中身は触らない。 */
    cdk_edit_new(e, path);
    e->len = len;
    warn_if_full(e);
    return true;
}

const char *cdk_edit_text(const cdk_edit_t *e, size_t *len)
{
    if (len != NULL) {
        *len = e->len;
    }
    return e->buf;
}

void cdk_edit_message(cdk_edit_t *e, const char *msg)
{
    snprintf(e->msg, sizeof(e->msg), "%s", (msg != NULL) ? msg : "");
}

void cdk_edit_saved(cdk_edit_t *e)
{
    e->modified = false;
    e->quit_confirm = false;
}

int cdk_edit_line_no(const cdk_edit_t *e)
{
    int n = 1;
    for (size_t i = 0; i < e->cursor && i < e->len; i++) {
        if (e->buf[i] == '\n') {
            n++;
        }
    }
    return n;
}

int cdk_edit_line_count(const cdk_edit_t *e)
{
    int n = 1;
    for (size_t i = 0; i < e->len; i++) {
        if (e->buf[i] == '\n') {
            n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* 描画                                                                */
/* ------------------------------------------------------------------ */

static void draw_status(cdk_edit_t *e)
{
    char line[CDK_EDIT_MSG_MAX + 128];
    if (e->msg[0] != '\0') {
        snprintf(line, sizeof(line), "%s", e->msg);
    } else {
        snprintf(line, sizeof(line), "%s%s%s %d/%d %s", e->modified ? "*" : " ",
                 (e->path[0] != '\0') ? e->path : "(無題)",
                 e->readonly ? " [読取専用]" : "", cdk_edit_line_no(e),
                 cdk_edit_line_count(e), e->mode_label);
    }
    ed_printf(e, "\033[%d;1H\033[7m\033[K", e->rows);
    ed_clipped(e, line, e->cols);
    ed_str(e, "\033[0m");
}

void cdk_edit_draw(cdk_edit_t *e)
{
    ensure_visible(e);
    ed_str(e, "\033[?25l"); /* 描いている間はカーソルを消す */
    walk(e, true);
    draw_status(e);
    if (e->cur_row >= 0) {
        ed_printf(e, "\033[%d;%dH", e->cur_row + 1, e->cur_col + 1);
    }
    ed_str(e, "\033[?25h");
}

/* ------------------------------------------------------------------ */
/* 編集                                                                */
/* ------------------------------------------------------------------ */

void cdk_edit_insert(cdk_edit_t *e, const char *utf8, size_t len)
{
    if (len == 0) {
        return;
    }
    if (e->readonly) {
        cdk_edit_message(e, "読取専用です");
        return;
    }
    if (e->len + len > e->cap) {
        cdk_edit_message(e, "これ以上入りません");
        return;
    }
    memmove(e->buf + e->cursor + len, e->buf + e->cursor, e->len - e->cursor);
    memcpy(e->buf + e->cursor, utf8, len);
    e->len += len;
    e->cursor += len;
    e->modified = true;
    e->quit_confirm = false;
    e->msg[0] = '\0';
}

static void delete_range(cdk_edit_t *e, size_t from, size_t to)
{
    if (e->readonly || from >= to || to > e->len) {
        return;
    }
    memmove(e->buf + from, e->buf + to, e->len - to);
    e->len -= (to - from);
    e->cursor = from;
    e->modified = true;
    e->quit_confirm = false;
    e->msg[0] = '\0';
}

static size_t encode_utf8(uint32_t cp, char *out)
{
    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

/* ------------------------------------------------------------------ */
/* 移動                                                                */
/* ------------------------------------------------------------------ */

/* 上下移動は**表示行の先頭オフセットだけ**で考える。画面のどこに
 * 見えているかは関係ない（見えていなければ ensure_visible が寄せる）。 */
static void move_vert(cdk_edit_t *e, int delta)
{
    size_t    rs = row_start_of(e, e->cursor);
    const int col = col_within(e, rs, e->cursor);
    const int want = (e->want_col > col) ? e->want_col : col;

    const int n = (delta < 0) ? -delta : delta;
    for (int i = 0; i < n; i++) {
        if (delta < 0) {
            const size_t prev = prev_row_start(e, rs);
            if (prev == rs) {
                break;
            }
            rs = prev;
        } else {
            /* next_row_start は次の行が無ければ rs をそのまま返す。
             * 「本文の末尾に空の表示行があるか」もそこで決まるので、
             * ここで改行の有無をもう一度見てはいけない（判断が 2 か所になる）。 */
            const size_t next = next_row_start(e, rs);
            if (next == rs) {
                break;
            }
            rs = next;
        }
    }
    e->cursor = offset_within(e, rs, want);
    e->want_col = want;
}

/* ------------------------------------------------------------------ */
/* キー                                                                */
/* ------------------------------------------------------------------ */

static bool is_ctrl(const cdk_key_t *k, char c)
{
    return k->keycode == 0 && (k->mods & CDK_MOD_CTRL_BIT) != 0 &&
           k->codepoint == (uint32_t)c;
}

bool cdk_edit_key(cdk_edit_t *e, const cdk_key_t *k)
{
    if (is_ctrl(k, 's')) {
        if (e->readonly) {
            cdk_edit_message(e, "読取専用です");
        } else {
            e->want_save = true;
        }
        return true;
    }
    if (is_ctrl(k, 'q')) {
        if (e->modified && !e->quit_confirm) {
            e->quit_confirm = true;
            cdk_edit_message(e, "変更があります。もう一度 Ctrl-Q で破棄");
            return true;
        }
        e->want_quit = true;
        return true;
    }
    if (is_ctrl(k, 'a') || k->keycode == CDK_K_HOME) {
        e->cursor = row_start_of(e, e->cursor);
        e->want_col = 0;
        return true;
    }
    if (is_ctrl(k, 'e') || k->keycode == CDK_K_END) {
        e->cursor = offset_within(e, row_start_of(e, e->cursor), e->cols);
        e->want_col = e->cols;
        return true;
    }

    switch (k->keycode) {
    case CDK_K_LEFT:
        if (e->cursor > 0) {
            e->cursor = prev_char(e, e->cursor);
        }
        e->want_col = 0;
        return true;

    case CDK_K_RIGHT:
        if (e->cursor < e->len) {
            size_t clen = 0;
            (void)char_at(e, e->cursor, &clen);
            e->cursor += (clen > 0) ? clen : 1;
        }
        e->want_col = 0;
        return true;

    case CDK_K_UP:
        move_vert(e, -1);
        return true;

    case CDK_K_DOWN:
        move_vert(e, +1);
        return true;

    case CDK_K_PGUP:
        move_vert(e, -(e->text_rows - 1));
        return true;

    case CDK_K_PGDN:
        move_vert(e, +(e->text_rows - 1));
        return true;

    case CDK_K_BS:
        if (e->cursor > 0) {
            delete_range(e, prev_char(e, e->cursor), e->cursor);
        }
        e->want_col = 0;
        return true;

    case CDK_K_DELETE:
        if (e->cursor < e->len) {
            size_t clen = 0;
            (void)char_at(e, e->cursor, &clen);
            delete_range(e, e->cursor, e->cursor + ((clen > 0) ? clen : 1));
        }
        e->want_col = 0;
        return true;

    case CDK_K_ENTER:
        cdk_edit_insert(e, "\n", 1);
        e->want_col = 0;
        return true;

    case CDK_K_TAB:
        /* タブ文字は入れない。端末のタブ位置と本文の桁数の数え方が
         * 食い違い、カーソルがずれる。空白 4 つで代える。 */
        cdk_edit_insert(e, "    ", 4);
        e->want_col = 0;
        return true;

    case CDK_K_SPACE:
        cdk_edit_insert(e, " ", 1);
        e->want_col = 0;
        return true;

    default:
        break;
    }

    if (k->keycode == 0 &&
        (k->mods & (CDK_MOD_CTRL_BIT | CDK_MOD_ALT_BIT | CDK_MOD_GUI_BIT)) == 0 &&
        k->codepoint >= 0x20u && k->codepoint != 0x7Fu) {
        char         utf8[4];
        const size_t n = encode_utf8(k->codepoint, utf8);
        cdk_edit_insert(e, utf8, n);
        e->want_col = 0;
        return true;
    }

    return false; /* 知らないキーは呼び出し側へ */
}
