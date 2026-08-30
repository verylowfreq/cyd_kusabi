/* スクロールバック。画面から出ていく行を環状バッファに積む。
 *
 * ## 積むのは 2 か所からだけ
 *
 * `cdk__scroll_up` の中では積まない。あの関数は 3 経路から呼ばれ、
 * そのうち `cdk__delete_lines` (DL, `CSI M`) は **`t->top` を一時的に
 * `t->cy` にすり替えて**呼ぶ。中で積むと `vi` や `less` の行削除が
 * そのまま履歴に流れ込む。
 *
 * 呼ぶのは「画面から永久に消える」経路だけ:
 *   - `cdk__index`（LF / 遅延折返し）が最下行で
 *   - `CSI S` (SU)
 *
 * ## 積む条件は 3 つとも要る
 *
 *   !alt_active && top == 0 && bot == rows - 1
 *
 * `top == 0` だけでは足りない。`ESC[1;20r` のように**上端はそのままで
 * 下だけ狭めた**領域だと `top == 0` は真になるが、row 0 から出ていく行は
 * 画面から消えていない（下の行はそのまま残っている）。
 *
 * ## 表示オフセット
 *
 * `view_off` が 0 なら今までと同じ。n なら n 行遡っている。
 * 表示行と中身の対応は `cdk_term_row`（ヘッダの inline）にある。
 */
#include "cdk_term.h"
#include "cdk_term_priv.h"

#include <string.h>

/* 環の中で「古いほうから i 行目」の位置。 */
static cdk_cell_t *hist_at(const cdk_term_t *t, int i)
{
    const int oldest = (t->hist_head - t->hist_len + t->hist_cap) % t->hist_cap;
    const int idx = (oldest + i) % t->hist_cap;
    return t->hist + (size_t)idx * (size_t)t->cols;
}

size_t cdk_term_hist_mem_size(int cols, int lines)
{
    if (cols <= 0 || lines <= 0) {
        return 0;
    }
    return (size_t)cols * (size_t)lines * sizeof(cdk_cell_t);
}

bool cdk_term_set_history(cdk_term_t *t, void *mem, size_t mem_size, int lines)
{
    if (t == NULL) {
        return false;
    }

    /* 履歴を外す。呼ばなければ今までと同じ挙動、という約束を保つため、
     * NULL / 0 行での呼び出しも受け付ける。 */
    if (mem == NULL || lines <= 0) {
        t->hist = NULL;
        t->hist_cap = 0;
        t->hist_len = 0;
        t->hist_head = 0;
        t->view_off = 0;
        return true;
    }

    if (mem_size < cdk_term_hist_mem_size(t->cols, lines)) {
        return false;
    }

    /* **2 回目の呼び出しでは必ず捨てる。** 行数が変わると環の巻き方が
     * 変わるので、前の中身は解釈できない。古い view_off が残ると
     * 環の外を指す。 */
    t->hist = (cdk_cell_t *)mem;
    t->hist_cap = lines;
    t->hist_len = 0;
    t->hist_head = 0;
    t->view_off = 0;
    return true;
}

void cdk__hist_push(cdk_term_t *t, int n)
{
    if (t->hist == NULL || t->hist_cap <= 0 || n <= 0) {
        return;
    }
    /* 代替画面の中身は履歴に入れない。部分スクロール領域も入れない
     * （画面から消えていないので）。 */
    if (t->alt_active || t->top != 0 || t->bot != t->rows - 1) {
        return;
    }

    const int height = t->bot - t->top + 1;
    if (n > height) {
        n = height;
    }

    for (int i = 0; i < n; i++) {
        cdk_cell_t *dst = t->hist + (size_t)t->hist_head * (size_t)t->cols;
        memcpy(dst, t->cells + (size_t)(t->top + i) * (size_t)t->cols,
               (size_t)t->cols * sizeof(cdk_cell_t));
        t->hist_head = (t->hist_head + 1) % t->hist_cap;
        if (t->hist_len < t->hist_cap) {
            t->hist_len++;
        }
    }

    /* 遡っている最中に新しい行が積まれたら、見ている位置を保つために
     * オフセットも 1 つずらす。**そうしないと画面が勝手に流れる。**
     * 古い行が押し出されて見失うところまで来たら、そこで頭打ちにする。 */
    if (t->view_off > 0) {
        t->view_off += n;
        if (t->view_off > t->hist_len) {
            t->view_off = t->hist_len;
        }
    }
}

void cdk_term_hist_clear(cdk_term_t *t)
{
    t->hist_len = 0;
    t->hist_head = 0;
    /* **遡っている最中に来る**（`CSI 3J`）。戻し忘れると環の外を読む。 */
    t->view_off = 0;
    cdk_term_mark_all_dirty(t);
}

int cdk_term_hist_len(const cdk_term_t *t)
{
    return t->hist_len;
}

int cdk_term_view_offset(const cdk_term_t *t)
{
    return t->view_off;
}

int cdk_term_view_scroll(cdk_term_t *t, int delta)
{
    /* **代替画面では遡らない。** 履歴は主画面のもので、`vi` や `less` や
     * エディタが出している絵とは何の関係も無い。ここを見ないと、
     * 代替画面の上に主画面の履歴が描かれて画面が壊れる（実機で踏んだ）。
     *
     * 0 を返すので、呼び出し側はキーを消費せず下へ流せる
     * （エディタでは PgUp として扱われる）。 */
    if (t->alt_active) {
        return 0;
    }

    const int before = t->view_off;
    int want = before + delta;

    if (want < 0) {
        want = 0;
    }
    if (want > t->hist_len) {
        want = t->hist_len;
    }
    /* 画面より深くは遡らない意味が無いわけではないが、履歴の長さで
     * 頭打ちにしておけば `cdk_term_row` の対応が常に成り立つ。 */
    if (want != before) {
        t->view_off = want;
        cdk_term_mark_all_dirty(t);
    }
    return t->view_off - before;
}

void cdk_term_view_reset(cdk_term_t *t)
{
    if (t->view_off != 0) {
        t->view_off = 0;
        cdk_term_mark_all_dirty(t);
    }
}

const cdk_cell_t *cdk__hist_row(const cdk_term_t *t, int i)
{
    return hist_at(t, i);
}
