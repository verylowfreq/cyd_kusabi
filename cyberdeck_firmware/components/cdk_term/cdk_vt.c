/* cdk_vt — VT/ANSI エスケープシーケンスの状態機械
 *
 * Paul Williams の DEC ANSI パーサをそのまま実装している。
 * 知らないシーケンスは黙って捨てる。画面を壊さないことが最優先。
 *
 * C1 制御文字 (0x80-0x9F) は解釈しない。UTF-8 の継続バイトと衝突するため。
 */
#include "cdk_term_priv.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* DEC 特殊グラフィクス文字集合 (ESC ( 0)                              */
/* ------------------------------------------------------------------ */

static uint32_t dec_graphic(uint8_t c)
{
    static const uint16_t map[] = {
        /* 0x5F */ 0x00A0, /* ` */ 0x25C6, /* a */ 0x2592, /* b */ 0x2409,
        /* c */ 0x240C, /* d */ 0x240D, /* e */ 0x240A, /* f */ 0x00B0,
        /* g */ 0x00B1, /* h */ 0x2424, /* i */ 0x240B, /* j */ 0x2518,
        /* k */ 0x2510, /* l */ 0x250C, /* m */ 0x2514, /* n */ 0x253C,
        /* o */ 0x23BA, /* p */ 0x23BB, /* q */ 0x2500, /* r */ 0x23BC,
        /* s */ 0x23BD, /* t */ 0x251C, /* u */ 0x2524, /* v */ 0x2534,
        /* w */ 0x252C, /* x */ 0x2502, /* y */ 0x2264, /* z */ 0x2265,
        /* { */ 0x03C0, /* | */ 0x2260, /* } */ 0x00A3, /* ~ */ 0x00B7,
    };
    if (c < 0x5Fu || c > 0x7Eu) {
        return c;
    }
    return map[c - 0x5Fu];
}

/* ------------------------------------------------------------------ */
/* 引数                                                                */
/* ------------------------------------------------------------------ */

static void clear_params(cdk_term_t *t)
{
    t->nparams = 0;
    t->param_seen = false;
    t->intermediate = 0;
    t->private_marker = 0;
    memset(t->params, 0, sizeof(t->params));
    memset(t->param_colon, 0, sizeof(t->param_colon));
}

static void push_param(cdk_term_t *t, bool colon)
{
    if (t->nparams < CDK_MAX_PARAMS) {
        t->nparams++;
        if (t->nparams < CDK_MAX_PARAMS) {
            t->params[t->nparams] = 0;
            t->param_colon[t->nparams] = colon ? 1u : 0u;
        }
    }
    t->param_seen = false;
}

static void param_digit(cdk_term_t *t, uint8_t d)
{
    if (t->nparams >= CDK_MAX_PARAMS) {
        return;
    }
    if (!t->param_seen) {
        t->param_seen = true;
    }
    int v = t->params[t->nparams];
    if (v < 100000) { /* 桁あふれで暴走させない */
        v = v * 10 + (int)(d - '0');
    }
    t->params[t->nparams] = v;
}

/* 引数の個数。最後の引数は push されていないので +1 する。 */
static int param_count(const cdk_term_t *t)
{
    if (t->nparams == 0 && !t->param_seen) {
        return 0;
    }
    return t->nparams + 1;
}

static int p_at(const cdk_term_t *t, int i, int dflt)
{
    if (i >= param_count(t) || i >= CDK_MAX_PARAMS) {
        return dflt;
    }
    const int v = t->params[i];
    return (v == 0) ? dflt : v;
}

/* 0 を 0 のまま返す版。SGR やモード番号で使う。 */
static int p_raw(const cdk_term_t *t, int i)
{
    if (i >= param_count(t) || i >= CDK_MAX_PARAMS) {
        return 0;
    }
    return t->params[i];
}

/* ------------------------------------------------------------------ */
/* SGR                                                                 */
/* ------------------------------------------------------------------ */

/* 拡張色を読む。i は 38/48 の位置。消費した引数の数を返す。 */
static int parse_ext_color(const cdk_term_t *t, int i, uint32_t *out)
{
    const int n = param_count(t);
    if (i + 1 >= n) {
        return 1;
    }
    const int kind = p_raw(t, i + 1);

    if (kind == 5) { /* 256 色 */
        if (i + 2 >= n) {
            return 2;
        }
        *out = CDK_COLOR_IDX(p_raw(t, i + 2));
        return 3;
    }
    if (kind == 2) { /* truecolor */
        /* ';' 形式:  38;2;R;G;B
         * ':' 形式:  38:2:R:G:B もしくは 38:2:<colorspace>:R:G:B
         * 後者は引数が 1 つ多い。部分引数フラグで見分ける。 */
        int base = i + 2;
        const bool colon_form = (i + 2 < CDK_MAX_PARAMS) && t->param_colon[i + 1];
        if (colon_form && (i + 5) < n) {
            base = i + 3; /* colorspace を読み飛ばす */
        }
        if (base + 2 >= n) {
            return 2;
        }
        const int r = p_raw(t, base);
        const int g = p_raw(t, base + 1);
        const int b = p_raw(t, base + 2);
        *out = CDK_COLOR_RGB(r & 0xFF, g & 0xFF, b & 0xFF);
        return (base - i) + 3;
    }
    return 2;
}

static void do_sgr(cdk_term_t *t)
{
    const int n = param_count(t);
    if (n == 0) {
        t->fg = CDK_COLOR_DEFAULT;
        t->bg = CDK_COLOR_DEFAULT;
        t->attr = 0;
        return;
    }

    for (int i = 0; i < n; i++) {
        const int v = p_raw(t, i);
        switch (v) {
        case 0:
            t->fg = CDK_COLOR_DEFAULT;
            t->bg = CDK_COLOR_DEFAULT;
            t->attr = 0;
            break;
        case 1: t->attr |= CDK_ATTR_BOLD; break;
        case 2: t->attr |= CDK_ATTR_DIM; break;
        case 3: t->attr |= CDK_ATTR_ITALIC; break;
        case 4: t->attr |= CDK_ATTR_UNDERLINE; break;
        case 5:
        case 6: t->attr |= CDK_ATTR_BLINK; break;
        case 7: t->attr |= CDK_ATTR_REVERSE; break;
        case 8: t->attr |= CDK_ATTR_INVISIBLE; break;
        case 9: t->attr |= CDK_ATTR_STRIKE; break;
        case 21:
        case 22: t->attr &= (uint16_t)~(CDK_ATTR_BOLD | CDK_ATTR_DIM); break;
        case 23: t->attr &= (uint16_t)~CDK_ATTR_ITALIC; break;
        case 24: t->attr &= (uint16_t)~CDK_ATTR_UNDERLINE; break;
        case 25: t->attr &= (uint16_t)~CDK_ATTR_BLINK; break;
        case 27: t->attr &= (uint16_t)~CDK_ATTR_REVERSE; break;
        case 28: t->attr &= (uint16_t)~CDK_ATTR_INVISIBLE; break;
        case 29: t->attr &= (uint16_t)~CDK_ATTR_STRIKE; break;
        case 39: t->fg = CDK_COLOR_DEFAULT; break;
        case 49: t->bg = CDK_COLOR_DEFAULT; break;
        case 38: i += parse_ext_color(t, i, &t->fg) - 1; break;
        case 48: i += parse_ext_color(t, i, &t->bg) - 1; break;
        default:
            if (v >= 30 && v <= 37) {
                t->fg = CDK_COLOR_IDX(v - 30);
            } else if (v >= 40 && v <= 47) {
                t->bg = CDK_COLOR_IDX(v - 40);
            } else if (v >= 90 && v <= 97) {
                t->fg = CDK_COLOR_IDX(v - 90 + 8);
            } else if (v >= 100 && v <= 107) {
                t->bg = CDK_COLOR_IDX(v - 100 + 8);
            } else {
                t->stat_unknown++;
            }
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* モード                                                              */
/* ------------------------------------------------------------------ */

static void save_cursor(cdk_term_t *t, cdk_term_saved_t *s)
{
    s->cx = t->cx;
    s->cy = t->cy;
    s->fg = t->fg;
    s->bg = t->bg;
    s->attr = t->attr;
    s->wrap_pending = t->wrap_pending;
    s->origin_mode = t->origin_mode;
}

static void restore_cursor(cdk_term_t *t, const cdk_term_saved_t *s)
{
    t->origin_mode = s->origin_mode;
    t->cx = (s->cx < t->cols) ? s->cx : t->cols - 1;
    t->cy = (s->cy < t->rows) ? s->cy : t->rows - 1;
    t->fg = s->fg;
    t->bg = s->bg;
    t->attr = s->attr;
    t->wrap_pending = s->wrap_pending;
}

static void set_private_mode(cdk_term_t *t, int mode, bool on)
{
    switch (mode) {
    case 1: t->app_cursor_keys = on; break;
    case 6:
        t->origin_mode = on;
        cdk__move_to(t, 0, 0);
        break;
    case 7: t->autowrap = on; break;
    case 12: break; /* カーソル点滅。持たない */
    case 25: t->cursor_visible = on; break;
    case 1000:
    case 1002:
    case 1003:
    case 1005:
    case 1006:
    case 1015:
        /* マウス報告。受理はするが報告は返さない。
         * 受理しないと CLI 側がシーケンスを画面に垂れ流すことがある。 */
        t->mouse_tracking = on;
        break;
    case 1048:
        if (on) {
            save_cursor(t, &t->saved);
        } else {
            restore_cursor(t, &t->saved);
        }
        break;
    case 47:
    case 1047:
    case 1049:
        if (on) {
            if (mode == 1049) {
                save_cursor(t, &t->saved);
            }
            cdk__switch_screen(t, true);
            if (mode == 1049) {
                cdk__fill(t, 0, 0, t->cols - 1, t->rows - 1);
                cdk__move_to(t, 0, 0);
            }
        } else {
            if (mode == 1047 || mode == 1049) {
                cdk__fill(t, 0, 0, t->cols - 1, t->rows - 1);
            }
            cdk__switch_screen(t, false);
            if (mode == 1049) {
                restore_cursor(t, &t->saved);
            }
        }
        break;
    case 2004: t->bracketed_paste = on; break;
    default: t->stat_unknown++; break;
    }
}

static void set_mode(cdk_term_t *t, int mode, bool on)
{
    switch (mode) {
    case 4: t->insert_mode = on; break;
    case 20: break; /* LNM。pty 側で面倒を見る */
    default: t->stat_unknown++; break;
    }
}

/* ------------------------------------------------------------------ */
/* タブ                                                                */
/* ------------------------------------------------------------------ */

static void tab_forward(cdk_term_t *t, int n)
{
    for (int k = 0; k < n; k++) {
        int x = t->cx + 1;
        while (x < t->cols - 1 && t->tabs[x] == 0u) {
            x++;
        }
        t->cx = (x > t->cols - 1) ? t->cols - 1 : x;
    }
    t->wrap_pending = false;
}

static void tab_backward(cdk_term_t *t, int n)
{
    for (int k = 0; k < n; k++) {
        int x = t->cx - 1;
        while (x > 0 && t->tabs[x] == 0u) {
            x--;
        }
        t->cx = (x < 0) ? 0 : x;
    }
    t->wrap_pending = false;
}

/* ------------------------------------------------------------------ */
/* C0 制御文字                                                         */
/* ------------------------------------------------------------------ */

static void execute_c0(cdk_term_t *t, uint8_t b)
{
    switch (b) {
    case 0x07: /* BEL */
        break;
    case 0x08: /* BS */
        if (t->wrap_pending) {
            t->wrap_pending = false;
        } else if (t->cx > 0) {
            t->cx--;
        }
        break;
    case 0x09: /* HT */
        tab_forward(t, 1);
        break;
    case 0x0A: /* LF */
    case 0x0B: /* VT */
    case 0x0C: /* FF */
        t->wrap_pending = false;
        cdk__index(t);
        break;
    case 0x0D: /* CR */
        t->cx = 0;
        t->wrap_pending = false;
        break;
    case 0x0E: /* SO */
        t->charset_shifted = true;
        break;
    case 0x0F: /* SI */
        t->charset_shifted = false;
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* ESC dispatch                                                        */
/* ------------------------------------------------------------------ */

static void decaln(cdk_term_t *t)
{
    for (int y = 0; y < t->rows; y++) {
        for (int x = 0; x < t->cols; x++) {
            cdk_cell_t *c = t->cells + (size_t)y * (size_t)t->cols + (size_t)x;
            c->cp = 'E';
            c->fg = CDK_COLOR_DEFAULT;
            c->bg = CDK_COLOR_DEFAULT;
            c->attr = 0;
        }
    }
    cdk_term_mark_all_dirty(t);
    cdk__move_to(t, 0, 0);
}

static void esc_dispatch(cdk_term_t *t, uint8_t b)
{
    if (t->intermediate == '(' || t->intermediate == ')') {
        const bool dec = (b == '0');
        if (t->intermediate == '(') {
            t->charset_g0_dec = dec;
        } else {
            t->charset_g1_dec = dec;
        }
        return;
    }
    if (t->intermediate == '#') {
        if (b == '8') {
            decaln(t);
        }
        return;
    }
    if (t->intermediate != 0) {
        t->stat_unknown++;
        return;
    }

    switch (b) {
    case '7': save_cursor(t, &t->saved); break;
    case '8': restore_cursor(t, &t->saved); break;
    case 'D': t->wrap_pending = false; cdk__index(t); break;
    case 'E': t->wrap_pending = false; t->cx = 0; cdk__index(t); break;
    case 'M': t->wrap_pending = false; cdk__reverse_index(t); break;
    case 'c': cdk_term_reset(t); break;
    case '=': t->app_keypad = true; break;
    case '>': t->app_keypad = false; break;
    case '\\': break; /* ST。OSC の終端で来る */
    default: t->stat_unknown++; break;
    }
}

/* ------------------------------------------------------------------ */
/* CSI dispatch                                                        */
/* ------------------------------------------------------------------ */

static void erase_display(cdk_term_t *t)
{
    switch (p_raw(t, 0)) {
    case 0: /* カーソルから画面末尾まで */
        cdk__fill(t, t->cx, t->cy, t->cols - 1, t->cy);
        if (t->cy + 1 <= t->rows - 1) {
            cdk__fill(t, 0, t->cy + 1, t->cols - 1, t->rows - 1);
        }
        break;
    case 1: /* 画面先頭からカーソルまで */
        if (t->cy > 0) {
            cdk__fill(t, 0, 0, t->cols - 1, t->cy - 1);
        }
        cdk__fill(t, 0, t->cy, t->cx, t->cy);
        break;
    case 2:
        cdk__fill(t, 0, 0, t->cols - 1, t->rows - 1);
        break;
    case 3:
        /* 3J は「画面 + スクロールバック」。履歴を持つようになったので
         * 2J とは別の意味になった。遡っている最中にも来る。 */
        cdk__fill(t, 0, 0, t->cols - 1, t->rows - 1);
        cdk_term_hist_clear(t);
        break;
    default:
        t->stat_unknown++;
        break;
    }
}

static void erase_line(cdk_term_t *t)
{
    switch (p_raw(t, 0)) {
    case 0: cdk__fill(t, t->cx, t->cy, t->cols - 1, t->cy); break;
    case 1: cdk__fill(t, 0, t->cy, t->cx, t->cy); break;
    case 2: cdk__fill(t, 0, t->cy, t->cols - 1, t->cy); break;
    default: t->stat_unknown++; break;
    }
}

static void set_scroll_region(cdk_term_t *t)
{
    int top = p_at(t, 0, 1) - 1;
    int bot = p_at(t, 1, t->rows) - 1;
    if (top < 0) {
        top = 0;
    }
    if (bot > t->rows - 1) {
        bot = t->rows - 1;
    }
    if (top >= bot) { /* 不正な指定は全画面に戻す */
        top = 0;
        bot = t->rows - 1;
    }
    t->top = top;
    t->bot = bot;
    cdk__move_to(t, 0, 0);
}

/* 符号なし 10 進を書く。newlib の printf は重いので使わない。 */
static int put_uint(char *out, int v)
{
    char tmp[8];
    int n = 0;
    if (v <= 0) {
        out[0] = '0';
        return 1;
    }
    while (v > 0 && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (int i = 0; i < n; i++) {
        out[i] = tmp[n - 1 - i];
    }
    return n;
}

static void device_status(cdk_term_t *t)
{
    switch (p_raw(t, 0)) {
    case 5:
        cdk__reply(t, "[0n");
        break;
    case 6: {
        int row = t->cy + 1;
        const int col = t->cx + 1;
        if (t->origin_mode) {
            row = t->cy - t->top + 1;
        }
        char buf[24];
        int n = 0;
        buf[n++] = '';
        buf[n++] = '[';
        n += put_uint(buf + n, row);
        buf[n++] = ';';
        n += put_uint(buf + n, col);
        buf[n++] = 'R';
        if (t->reply != NULL) {
            t->reply(t->reply_user, (const uint8_t *)buf, (size_t)n);
        }
        break;
    }
    default:
        t->stat_unknown++;
        break;
    }
}

static void csi_dispatch(cdk_term_t *t, uint8_t b)
{
    /* 私用マーカー付きは限られたものだけ扱う */
    if (t->private_marker == '?') {
        if (b == 'h' || b == 'l') {
            const int n = param_count(t);
            for (int i = 0; i < n; i++) {
                set_private_mode(t, p_raw(t, i), b == 'h');
            }
        } else {
            t->stat_unknown++;
        }
        return;
    }
    if (t->private_marker != 0) {
        t->stat_unknown++;
        return;
    }

    if (t->intermediate == '!' && b == 'p') { /* DECSTR ソフトリセット */
        t->top = 0;
        t->bot = t->rows - 1;
        t->origin_mode = false;
        t->autowrap = true;
        t->insert_mode = false;
        t->cursor_visible = true;
        t->fg = CDK_COLOR_DEFAULT;
        t->bg = CDK_COLOR_DEFAULT;
        t->attr = 0;
        cdk__move_to(t, 0, 0);
        return;
    }
    if (t->intermediate == ' ' && b == 'q') { /* DECSCUSR カーソル形状 */
        return;
    }
    if (t->intermediate != 0) {
        t->stat_unknown++;
        return;
    }

    switch (b) {
    case 'A': { /* CUU */
        int y = t->cy - p_at(t, 0, 1);
        const int limit = (t->cy >= t->top) ? t->top : 0;
        t->cy = (y < limit) ? limit : y;
        t->wrap_pending = false;
        break;
    }
    case 'B': /* CUD */
    case 'e': {
        int y = t->cy + p_at(t, 0, 1);
        const int limit = (t->cy <= t->bot) ? t->bot : t->rows - 1;
        t->cy = (y > limit) ? limit : y;
        t->wrap_pending = false;
        break;
    }
    case 'C': /* CUF */
    case 'a': {
        int x = t->cx + p_at(t, 0, 1);
        t->cx = (x > t->cols - 1) ? t->cols - 1 : x;
        t->wrap_pending = false;
        break;
    }
    case 'D': { /* CUB */
        int x = t->cx - p_at(t, 0, 1);
        t->cx = (x < 0) ? 0 : x;
        t->wrap_pending = false;
        break;
    }
    case 'E': { /* CNL */
        int y = t->cy + p_at(t, 0, 1);
        const int limit = (t->cy <= t->bot) ? t->bot : t->rows - 1;
        t->cy = (y > limit) ? limit : y;
        t->cx = 0;
        t->wrap_pending = false;
        break;
    }
    case 'F': { /* CPL */
        int y = t->cy - p_at(t, 0, 1);
        const int limit = (t->cy >= t->top) ? t->top : 0;
        t->cy = (y < limit) ? limit : y;
        t->cx = 0;
        t->wrap_pending = false;
        break;
    }
    case 'G': /* CHA */
    case '`':
        cdk__move_to(t, p_at(t, 0, 1) - 1, t->cy);
        break;
    case 'd': { /* VPA */
        const int y = p_at(t, 0, 1) - 1;
        const int x = t->cx;
        cdk__move_to(t, x, y);
        break;
    }
    case 'H': /* CUP */
    case 'f':
        cdk__move_to(t, p_at(t, 1, 1) - 1, p_at(t, 0, 1) - 1);
        break;
    case 'I': tab_forward(t, p_at(t, 0, 1)); break;
    case 'Z': tab_backward(t, p_at(t, 0, 1)); break;
    case 'J': erase_display(t); break;
    case 'K': erase_line(t); break;
    case 'L': cdk__insert_lines(t, p_at(t, 0, 1)); break;
    case 'M': cdk__delete_lines(t, p_at(t, 0, 1)); break;
    case 'P': cdk__delete_chars(t, p_at(t, 0, 1)); break;
    case 'X': cdk__erase_chars(t, p_at(t, 0, 1)); break;
    case '@': cdk__insert_chars(t, p_at(t, 0, 1)); break;
    case 'S': {
        /* SU も画面から行を追い出す。LF と同じく履歴へ写してから。 */
        const int n = p_at(t, 0, 1);
        cdk__hist_push(t, n);
        cdk__scroll_up(t, n);
        break;
    }
    case 'T': cdk__scroll_down(t, p_at(t, 0, 1)); break;
    case 'h': /* SM */
    case 'l': { /* RM */
        const int n = param_count(t);
        for (int i = 0; i < n; i++) {
            set_mode(t, p_raw(t, i), b == 'h');
        }
        break;
    }
    case 'm': do_sgr(t); break;
    case 'r': set_scroll_region(t); break;
    case 's': save_cursor(t, &t->saved); break;
    case 'u': restore_cursor(t, &t->saved); break;
    case 'n': device_status(t); break;
    case 'c': /* DA。VT220 相当を名乗る */
        cdk__reply(t, "\033[?62;22c");
        break;
    case 'g': /* TBC タブ消去 */
        if (p_raw(t, 0) == 3) {
            memset(t->tabs, 0, (size_t)t->cols);
        } else if (t->cx < t->cols) {
            t->tabs[t->cx] = 0u;
        }
        break;
    case 't': /* ウィンドウ操作。無視する */
        break;
    default:
        t->stat_unknown++;
        break;
    }
}

/* ------------------------------------------------------------------ */
/* OSC                                                                 */
/* ------------------------------------------------------------------ */

static void osc_dispatch(cdk_term_t *t)
{
    t->osc[(t->osc_len < CDK_OSC_MAX) ? t->osc_len : CDK_OSC_MAX - 1] = '\0';

    /* "0;title" または "2;title" だけ拾う */
    const char *s = t->osc;
    if ((s[0] == '0' || s[0] == '2') && s[1] == ';') {
        const char *title = s + 2;
        size_t n = strlen(title);
        if (n >= CDK_TITLE_MAX) {
            n = CDK_TITLE_MAX - 1;
        }
        memcpy(t->title, title, n);
        t->title[n] = '\0';
    }
    t->osc_len = 0;
}

/* ------------------------------------------------------------------ */
/* 状態機械                                                            */
/* ------------------------------------------------------------------ */

static void print_char(cdk_term_t *t, uint32_t cp)
{
    const bool dec = t->charset_shifted ? t->charset_g1_dec : t->charset_g0_dec;
    if (dec && cp < 0x80u) {
        cp = dec_graphic((uint8_t)cp);
    }
    cdk__put_char(t, cp);
}

static void ground_byte(cdk_term_t *t, uint8_t b)
{
    uint32_t cp = 0;
    bool retry = false;
    const cdk_utf8_status_t st = cdk_utf8_feed(&t->utf8, b, &cp, &retry);
    if (st == CDK_UTF8_NEED) {
        return;
    }
    if (st == CDK_UTF8_INVALID) {
        print_char(t, CDK_REPLACEMENT);
        if (retry) {
            /* このバイトは新しい文字の先頭。もう一度食わせる。 */
            bool retry2 = false;
            if (cdk_utf8_feed(&t->utf8, b, &cp, &retry2) == CDK_UTF8_OK) {
                print_char(t, cp);
            }
        }
        return;
    }
    print_char(t, cp);
}

void cdk__vt_byte(cdk_term_t *t, uint8_t b)
{
    /* どの状態からでも効くもの */
    if (b == 0x1Bu) {
        if (t->state == CDK_ST_OSC) {
            osc_dispatch(t);
        }
        cdk_utf8_reset(&t->utf8);
        clear_params(t);
        t->state = CDK_ST_ESC;
        return;
    }
    if (b == 0x18u || b == 0x1Au) { /* CAN / SUB */
        cdk_utf8_reset(&t->utf8);
        t->state = CDK_ST_GROUND;
        return;
    }

    switch (t->state) {
    case CDK_ST_GROUND:
        if (b < 0x20u) {
            execute_c0(t, b);
        } else if (b == 0x7Fu) {
            /* DEL は捨てる */
        } else {
            ground_byte(t, b);
        }
        break;

    case CDK_ST_ESC:
        if (b < 0x20u) {
            execute_c0(t, b);
        } else if (b >= 0x20u && b <= 0x2Fu) {
            t->intermediate = b;
            t->state = CDK_ST_ESC_INT;
        } else if (b == '[') {
            clear_params(t);
            t->state = CDK_ST_CSI_ENTRY;
        } else if (b == ']') {
            t->osc_len = 0;
            t->state = CDK_ST_OSC;
        } else if (b == 'P' || b == 'X' || b == '^' || b == '_') {
            t->state = CDK_ST_STRING_IGNORE;
        } else {
            esc_dispatch(t, b);
            t->state = CDK_ST_GROUND;
        }
        break;

    case CDK_ST_ESC_INT:
        if (b < 0x20u) {
            execute_c0(t, b);
        } else if (b >= 0x20u && b <= 0x2Fu) {
            t->intermediate = b; /* 複数中間文字は最後だけ覚える */
        } else {
            esc_dispatch(t, b);
            t->state = CDK_ST_GROUND;
        }
        break;

    case CDK_ST_CSI_ENTRY:
        if (b < 0x20u) {
            execute_c0(t, b);
        } else if (b >= '0' && b <= '9') {
            param_digit(t, b);
            t->state = CDK_ST_CSI_PARAM;
        } else if (b == ';' || b == ':') {
            push_param(t, b == ':');
            t->state = CDK_ST_CSI_PARAM;
        } else if (b >= 0x3Cu && b <= 0x3Fu) {
            t->private_marker = b;
            t->state = CDK_ST_CSI_PARAM;
        } else if (b >= 0x20u && b <= 0x2Fu) {
            t->intermediate = b;
            t->state = CDK_ST_CSI_INT;
        } else if (b >= 0x40u && b <= 0x7Eu) {
            csi_dispatch(t, b);
            t->state = CDK_ST_GROUND;
        } else {
            t->state = CDK_ST_CSI_IGNORE;
        }
        break;

    case CDK_ST_CSI_PARAM:
        if (b < 0x20u) {
            execute_c0(t, b);
        } else if (b >= '0' && b <= '9') {
            param_digit(t, b);
        } else if (b == ';' || b == ':') {
            push_param(t, b == ':');
        } else if (b >= 0x20u && b <= 0x2Fu) {
            t->intermediate = b;
            t->state = CDK_ST_CSI_INT;
        } else if (b >= 0x40u && b <= 0x7Eu) {
            csi_dispatch(t, b);
            t->state = CDK_ST_GROUND;
        } else {
            t->state = CDK_ST_CSI_IGNORE;
        }
        break;

    case CDK_ST_CSI_INT:
        if (b < 0x20u) {
            execute_c0(t, b);
        } else if (b >= 0x20u && b <= 0x2Fu) {
            t->intermediate = b;
        } else if (b >= 0x40u && b <= 0x7Eu) {
            csi_dispatch(t, b);
            t->state = CDK_ST_GROUND;
        } else {
            t->state = CDK_ST_CSI_IGNORE;
        }
        break;

    case CDK_ST_CSI_IGNORE:
        if (b >= 0x40u && b <= 0x7Eu) {
            t->stat_unknown++;
            t->state = CDK_ST_GROUND;
        }
        break;

    case CDK_ST_OSC:
        if (b == 0x07u) { /* BEL 終端 */
            osc_dispatch(t);
            t->state = CDK_ST_GROUND;
        } else if (b >= 0x20u) {
            if (t->osc_len < CDK_OSC_MAX - 1) {
                t->osc[t->osc_len++] = (char)b;
            }
        }
        break;

    case CDK_ST_STRING_IGNORE:
        if (b == 0x07u) {
            t->state = CDK_ST_GROUND;
        }
        break;

    default:
        t->state = CDK_ST_GROUND;
        break;
    }
}
