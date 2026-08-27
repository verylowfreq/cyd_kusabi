/* cdk_edit — microSD 上のプレーンテキストを編集する簡易エディタ
 *
 * ESP-IDF に依存しない純 C。ホストで全部テストする。
 *
 * 出力は**端末へのバイト列**（cdk_shell と同じ）。エスケープ列を組んで
 * cdk_term に流すので、描画・フォント・桁計算は既存の経路をそのまま通る。
 * 画面を自前でセル単位に描くと、ホストの基準レンダラと突き合わせられなくなる。
 *
 * 本文は**平たいバイト配列 + memmove**。ギャップバッファや piece table は
 * 使わない。128KB の memmove は 1ms 未満で、1 打鍵あたりの処理としては
 * 十分に速い。速さのために正しさを危うくする理由がない。
 *
 * ## 折り返し
 *
 * 日本語の文章を書くので**折り返す**（横スクロールしない）。
 * 折り返し位置とカーソル位置は**同じ 1 回の走査**で決める。
 * ここを 2 か所で計算すると、全角が右端に来たときにカーソルだけ
 * 別の場所に行く。上下移動を足すときにその誘惑が出るので、
 * 「行の切れ目を決めるのはこの 1 か所だけ」を守ること。
 */
#ifndef CDK_EDIT_H
#define CDK_EDIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cdk_keys.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CDK_EDIT_ROWS_MAX 64
#define CDK_EDIT_PATH_MAX 128
#define CDK_EDIT_MSG_MAX  96

typedef void (*cdk_edit_out_fn)(void *user, const char *data, size_t len);

typedef struct {
    char  *buf; /* 本文 (UTF-8)。NUL 終端しない */
    size_t cap;
    size_t len;

    size_t cursor; /* バイトオフセット */
    size_t top;    /* 画面最上行の先頭バイトオフセット */

    int cols;      /* 画面の桁数 */
    int rows;      /* 画面の行数（ステータス行を含む） */
    int text_rows; /* 本文に使える行数 = rows - 1 */

    bool modified;
    bool readonly; /* 大きすぎて全部読めなかったファイル。保存を禁じる */
    char path[CDK_EDIT_PATH_MAX];

    cdk_edit_out_fn out;
    void           *out_user;

    /* 直近の走査の結果。上下移動とスクロールが使う。 */
    size_t rowstart[CDK_EDIT_ROWS_MAX];
    int    nrows;
    int    cur_row; /* 画面上の行。見えていなければ -1 */
    int    cur_col;

    /* 上下移動でできるだけ保ちたい桁。 */
    int want_col;

    char msg[CDK_EDIT_MSG_MAX]; /* ステータス行に出す一時メッセージ */
    const char *mode_label;     /* 日本語入力のモード。呼び出し側が差す */

    bool quit_confirm; /* 変更があるまま Ctrl-Q を 1 回押した */
    bool want_quit;
    bool want_save;
} cdk_edit_t;

/* arena は本文を置く領域。呼び出し側が持つ（起動時に 1 回だけ確保する）。 */
void cdk_edit_init(cdk_edit_t *e, char *arena, size_t cap, cdk_edit_out_fn out,
                   void *user);

void cdk_edit_set_size(cdk_edit_t *e, int cols, int rows);

/* ファイルの中身を読み込む。
 *
 * **cap を超える中身は受け付けない。** 途中まで読んで保存すると、
 * その時点でファイルの後半が消える。切り詰めたものを黙って持つのは
 * エディタではなくシュレッダー。false を返し、読み込みは行わない。 */
bool cdk_edit_load(cdk_edit_t *e, const char *path, const char *data, size_t len);

/* すでに arena へ直接読み込んである場合。長さだけ受け取る。
 *
 * cdk_edit_load に arena 自身を渡すと memcpy の src と dst が重なる。
 * いまは同じ番地なので動くが、規格上は未定義で、実装を変えた瞬間に壊れる。 */
bool cdk_edit_adopt(cdk_edit_t *e, const char *path, size_t len);

/* 新規（空）で開く。 */
void cdk_edit_new(cdk_edit_t *e, const char *path);

/* 画面全体を描き直してカーソルを合わせる。 */
void cdk_edit_draw(cdk_edit_t *e);

/* キーを 1 つ処理する。true を返したらエディタが消費した。 */
bool cdk_edit_key(cdk_edit_t *e, const cdk_key_t *k);

/* 確定した文字列を挿入する（日本語入力から来る）。 */
void cdk_edit_insert(cdk_edit_t *e, const char *utf8, size_t len);

/* ステータス行に 1 行出す（保存結果など）。 */
void cdk_edit_message(cdk_edit_t *e, const char *msg);

/* 保存が済んだことを伝える。 */
void cdk_edit_saved(cdk_edit_t *e);

/* 本文の先頭と長さ。保存する側が使う。 */
const char *cdk_edit_text(const cdk_edit_t *e, size_t *len);

/* いま何行目か（1 始まり）。ステータス表示とテスト用。 */
int cdk_edit_line_no(const cdk_edit_t *e);
int cdk_edit_line_count(const cdk_edit_t *e);

#ifdef __cplusplus
}
#endif

#endif /* CDK_EDIT_H */
