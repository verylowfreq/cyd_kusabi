/* cdk_term — VT/ANSI ターミナルエミュレータ
 *
 * ESP-IDF に依存しない純 C。ホスト側で全部テストする。
 *
 * 対応するシーケンスは DESIGN.md §6.2 を参照。
 * 知らないシーケンスは黙って捨てる（画面を壊さないことが最優先）。
 *
 * メモリは呼び出し側が用意する。起動時に一度だけ確保して以後固定する方針。
 */
#ifndef CDK_TERM_H
#define CDK_TERM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cdk_text.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CDK_MAX_PARAMS  16
#define CDK_OSC_MAX     256
#define CDK_TITLE_MAX   64

/* ---- 色 ----
 * 既定色 / パレット番号 (0-255) / truecolor を 1 つの u32 に詰める。
 * 実際の RGB565 への変換は描画側 (cdk_screen) が持つ。 */
#define CDK_COLOR_DEFAULT    0u
#define CDK_COLOR_KIND_MASK  0xFF000000u
#define CDK_COLOR_KIND_IDX   0x01000000u
#define CDK_COLOR_KIND_RGB   0x02000000u

#define CDK_COLOR_IDX(i)     (CDK_COLOR_KIND_IDX | ((uint32_t)(i) & 0xFFu))
#define CDK_COLOR_RGB(r, g, b)                                                 \
    (CDK_COLOR_KIND_RGB | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) |       \
     (uint32_t)(b))

#define CDK_COLOR_IS_IDX(c) (((c) & CDK_COLOR_KIND_MASK) == CDK_COLOR_KIND_IDX)
#define CDK_COLOR_IS_RGB(c) (((c) & CDK_COLOR_KIND_MASK) == CDK_COLOR_KIND_RGB)
#define CDK_COLOR_GET_IDX(c) ((uint8_t)((c) & 0xFFu))

/* ---- 属性 ---- */
#define CDK_ATTR_BOLD      0x0001u
#define CDK_ATTR_DIM       0x0002u
#define CDK_ATTR_ITALIC    0x0004u /* 受理するが斜体フォントは持たない */
#define CDK_ATTR_UNDERLINE 0x0008u
#define CDK_ATTR_BLINK     0x0010u /* 受理するが点滅させない */
#define CDK_ATTR_REVERSE   0x0020u
#define CDK_ATTR_INVISIBLE 0x0040u
#define CDK_ATTR_STRIKE    0x0080u
#define CDK_ATTR_WIDE_LEAD 0x0100u /* 全角の左半分。cp が入っている */
#define CDK_ATTR_WIDE_TAIL 0x0200u /* 全角の右半分。cp は 0 */

typedef struct {
    uint32_t cp;   /* Unicode。0 = 空白 */
    uint32_t fg;
    uint32_t bg;
    uint16_t attr;
    uint16_t _pad;
} cdk_cell_t;

/* 端末からホスト側（SSH の入力側）へ返す応答。DA / DSR で使う。 */
typedef void (*cdk_term_reply_fn)(void *user, const uint8_t *data, size_t len);

typedef enum {
    CDK_ST_GROUND = 0,
    CDK_ST_ESC,
    CDK_ST_ESC_INT,
    CDK_ST_CSI_ENTRY,
    CDK_ST_CSI_PARAM,
    CDK_ST_CSI_INT,
    CDK_ST_CSI_IGNORE,
    CDK_ST_OSC,
    CDK_ST_STRING_IGNORE /* DCS / SOS / PM / APC はまとめて読み捨てる */
} cdk_term_state_t;

typedef struct {
    int cx, cy;
    uint32_t fg, bg;
    uint16_t attr;
    bool wrap_pending;
    bool origin_mode;
} cdk_term_saved_t;

typedef struct {
    int cols, rows;

    cdk_cell_t *primary;
    cdk_cell_t *alternate;
    cdk_cell_t *cells;   /* primary か alternate を指す */
    uint8_t    *dirty;   /* 行ごと。rows バイト */
    uint8_t    *tabs;    /* 桁ごとのタブストップ。cols バイト */

    int  cx, cy;
    int  top, bot;       /* スクロール領域 [top, bot] 0 始まり両端含む */
    bool wrap_pending;   /* 右端に達したが折り返しは保留 (DECAWM の遅延折返し) */

    /* 現在の SGR */
    uint32_t fg, bg;
    uint16_t attr;

    /* モード */
    bool autowrap;
    bool origin_mode;
    bool cursor_visible;
    bool alt_active;
    bool bracketed_paste;
    bool app_cursor_keys;
    bool app_keypad;
    bool insert_mode;
    bool mouse_tracking;   /* 受理するが報告はしない */

    cdk_term_saved_t saved;
    cdk_term_saved_t saved_alt;

    /* DEC 特殊グラフィクス文字集合 (ESC ( 0) */
    bool charset_g0_dec;
    bool charset_g1_dec;
    bool charset_shifted; /* SO/SI で G1 を選んでいるか */

    /* パーサ */
    cdk_term_state_t state;
    int      params[CDK_MAX_PARAMS];
    /* その引数が ':' で始まる部分引数かどうか。truecolor の
     * ESC[38:2::R:G:Bm 形式を ESC[38;2;R;G;Bm と区別するのに要る。 */
    uint8_t  param_colon[CDK_MAX_PARAMS];
    int      nparams;
    bool     param_seen;
    uint8_t  intermediate;
    uint8_t  private_marker;
    char     osc[CDK_OSC_MAX];
    int      osc_len;

    cdk_utf8_t utf8;

    char title[CDK_TITLE_MAX];

    cdk_term_reply_fn reply;
    void             *reply_user;

    /* 診断用 */
    uint32_t stat_unknown; /* 捨てたシーケンスの数 */
} cdk_term_t;

/* cols x rows に必要なメモリ量。cdk_term_init に渡すブロックの大きさ。 */
size_t cdk_term_mem_size(int cols, int rows);

/* mem は cdk_term_mem_size(cols, rows) 以上。t 自体は呼び側が持つ。 */
bool cdk_term_init(cdk_term_t *t, int cols, int rows, void *mem, size_t mem_size);

void cdk_term_set_reply(cdk_term_t *t, cdk_term_reply_fn fn, void *user);

/* RIS 相当。画面を消してモードを初期値に戻す。 */
void cdk_term_reset(cdk_term_t *t);

/* 出力バイト列を食わせる。分割されていてよい。 */
void cdk_term_feed(cdk_term_t *t, const void *data, size_t len);

/* ---- 参照 ---- */
static inline const cdk_cell_t *cdk_term_row(const cdk_term_t *t, int y)
{
    return t->cells + (size_t)y * (size_t)t->cols;
}

static inline const cdk_cell_t *cdk_term_cell(const cdk_term_t *t, int x, int y)
{
    return t->cells + (size_t)y * (size_t)t->cols + (size_t)x;
}

static inline bool cdk_term_line_dirty(const cdk_term_t *t, int y)
{
    return t->dirty[y] != 0u;
}

void cdk_term_mark_row(cdk_term_t *t, int y);
void cdk_term_mark_all_dirty(cdk_term_t *t);
void cdk_term_clear_dirty(cdk_term_t *t);

/* 1 行をテキストに落とす。テスト用。全角の右半分は飛ばす。
 * 末尾の空白は削らない。out は最低 cols*4+1 バイト。 */
size_t cdk_term_row_text(const cdk_term_t *t, int y, char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* CDK_TERM_H */
