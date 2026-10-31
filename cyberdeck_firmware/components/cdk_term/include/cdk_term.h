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
    /* マウス報告。クリックや移動は返さない。**ホイールだけ**、Shift+PgUp / PgDn の
     * 代わりに返す（cdk_term_wheel_report）。
     * mouse_mode: 0 = 報告なし / 1000 / 1002 / 1003（最後に入れたもの）
     * mouse_enc:  0 = 既定（ESC [ M + 3 バイト）/ 1005 / 1006 / 1015
     * **形式 (1006 など) だけでは報告は始まらない。** */
    uint16_t mouse_mode;
    uint16_t mouse_enc;

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

    /* ---- スクロールバック ----
     * hist_cap が 0 なら履歴なし。**呼ばなければ今までとまったく同じ挙動**で、
     * ホストの基準レンダラ (cdk_render) はそのまま使う。 */
    cdk_cell_t *hist;      /* 環状。cols * hist_cap セル */
    int         hist_cap;  /* 0 = 履歴なし */
    int         hist_len;  /* 貯まっている行数 (<= hist_cap) */
    int         hist_head; /* 次に書く位置 */
    int         view_off;  /* 0 = 最新。n = n 行遡っている */

    /* 診断用 */
    uint32_t stat_unknown; /* 捨てたシーケンスの数 */
    uint32_t stat_clear_hist; /* ESC [ 3 J（画面 + 履歴を消す）を受けた数 */
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

/* ---- スクロールバック ----
 *
 * 履歴は呼び出し側が用意した領域に置く。**cdk_term_init の形は変えない**ので、
 * set_history を呼ばない使い方（cdk_render・多くのテスト）は今までと同じ。
 */

/* cols x lines に要るバイト数。 */
size_t cdk_term_hist_mem_size(int cols, int lines);

/* 履歴を付ける。mem == NULL / lines <= 0 なら外す。
 * **2 回目の呼び出しは中身を捨てる**（行数が変わると環の巻き方が変わる）。 */
bool cdk_term_set_history(cdk_term_t *t, void *mem, size_t mem_size, int lines);

int  cdk_term_hist_len(const cdk_term_t *t);
int  cdk_term_view_offset(const cdk_term_t *t);
/* 遡る (delta > 0) / 戻る (delta < 0)。実際に動いた量を返す。 */
int  cdk_term_view_scroll(cdk_term_t *t, int delta);
void cdk_term_view_reset(cdk_term_t *t);
void cdk_term_hist_clear(cdk_term_t *t);

/* ---- Shift+PgUp / PgDn の行き先 ----
 *
 * 相手のアプリの状態で決める。判断だけで、送るのは呼び出し側。
 *   WHEEL : マウス報告を頼まれている。ホイールを回したことにして送る
 *           （スクロールを自分で処理したいと言っているのと同じ）
 *   KEY   : 代替画面だが報告は頼まれていない。Shift なしの PgUp / PgDn を送る。
 *           ↑ / ↓ にしないのは、入力欄で履歴の呼び出しになって打ちかけが消えるから
 *   LOCAL : 通常の画面。こちらのスクロールバックを動かす */
typedef enum {
    CDK_PAGE_LOCAL = 0,
    CDK_PAGE_WHEEL,
    CDK_PAGE_KEY,
} cdk_page_route_t;

cdk_page_route_t cdk_term_page_route(const cdk_term_t *t);

/* 1 回押したときに回すホイールの段数。行数 / 3（最低 1）。 */
int cdk_term_wheel_steps(const cdk_term_t *t);

/* ホイール 1 段ぶんの報告を out に書く。位置は横が中央、縦が上から 1/4（入力欄を避ける）。
 * 形式は mouse_enc に合わせる。報告を頼まれていない・入らないときは 0。 */
size_t cdk_term_wheel_report(const cdk_term_t *t, bool up, uint8_t *out, size_t cap);

/* 履歴の古いほうから i 行目。cdk_term_row が使う。 */
const cdk_cell_t *cdk__hist_row(const cdk_term_t *t, int i);

/* ---- 参照 ---- */

/* 表示行 y の中身。
 *
 * 遡っている (view_off = k > 0) ときの対応:
 *   y <  k : 履歴の (hist_len - k + y) 行目
 *   y >= k : 画面の (y - k) 行目
 *
 * **k は hist_len で頭打ちにする。** 不変条件を信じて素通しすると、
 * どこかで戻し忘れたときに落ちずに環の外のゴミを読む。 */
static inline const cdk_cell_t *cdk_term_row(const cdk_term_t *t, int y)
{
    int k = t->view_off;
    if (k > t->hist_len) {
        k = t->hist_len;
    }
    if (k > 0 && y < k) {
        return cdk__hist_row(t, t->hist_len - k + y);
    }
    return t->cells + (size_t)(y - k) * (size_t)t->cols;
}

static inline const cdk_cell_t *cdk_term_cell(const cdk_term_t *t, int x, int y)
{
    return cdk_term_row(t, y) + (size_t)x;
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
