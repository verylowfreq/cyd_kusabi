/* cdk_shell — 行編集つきのコマンドラインシェル
 *
 * ESP-IDF に依存しない純 C。ホスト側で全部テストする。
 *
 * 入力は cdk_key_t（キーイベント）で受ける。エスケープ列は
 * cdk_keys_decode が解いてくれるので、ここは一切見ない。
 * USB キーボード（M7）は最初からキーイベントで来るので、この境界がそのまま使える。
 *
 * 出力は端末へのバイト列。呼び出し側が cdk_term なり SSH なりへ流す。
 * ESP-IDF の console コンポーネントは端末を直接握るので使わない。
 */
#ifndef CDK_SHELL_H
#define CDK_SHELL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cdk_keys.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CDK_SHELL_LINE_MAX    256
#define CDK_SHELL_HISTORY_MAX 16
#define CDK_SHELL_ARGS_MAX    12

typedef void (*cdk_shell_out_fn)(void *user, const char *data, size_t len);

/* 戻り値は終了コード。0 が成功。 */
typedef int (*cdk_shell_cmd_fn)(int argc, char **argv, void *user);

typedef struct {
    const char      *name;
    const char      *usage; /* 1 行の説明。help が並べる */
    cdk_shell_cmd_fn fn;
} cdk_shell_cmd_t;

typedef struct {
    char line[CDK_SHELL_LINE_MAX];
    int  len;
    int  cursor;

    char hist[CDK_SHELL_HISTORY_MAX][CDK_SHELL_LINE_MAX];
    int  hist_count; /* 貯まっている数 */
    int  hist_pos;   /* -1 = 編集中。0 が最新 */
    char stash[CDK_SHELL_LINE_MAX]; /* 履歴を辿る前の編集内容 */

    const cdk_shell_cmd_t *cmds;
    size_t                 ncmds;
    void                  *cmd_user;

    cdk_shell_out_fn out;
    void            *out_user;

    const char *prompt;
    bool        active; /* false の間はキーを無視する（SSH に入っている等） */
    int         last_status;
} cdk_shell_t;

void cdk_shell_init(cdk_shell_t *sh, cdk_shell_out_fn out, void *out_user);

void cdk_shell_set_commands(cdk_shell_t *sh, const cdk_shell_cmd_t *cmds, size_t n,
                            void *cmd_user);

/* プロンプトを出して入力を受け付け始める。 */
void cdk_shell_begin(cdk_shell_t *sh);

/* シェルを止める（SSH に切り替えるときなど）。キーを無視するようになる。 */
void cdk_shell_end(cdk_shell_t *sh);

bool cdk_shell_active(const cdk_shell_t *sh);

/* キーを 1 つ食わせる。 */
void cdk_shell_key(cdk_shell_t *sh, const cdk_key_t *key);

/* コマンドから出力するときに使う。 */
void cdk_shell_write(cdk_shell_t *sh, const char *s);
void cdk_shell_printf(cdk_shell_t *sh, const char *fmt, ...);

/* ---- テストしやすいように公開している内部処理 ---- */

/* line を argv に分割する。引用符 " と ' を解釈し、line を書き換える。
 * 返り値は argc。argv は max 個まで。 */
int cdk_shell_split(char *line, char **argv, int max);

/* 画面上で何桁ぶんになるかを数える。
 * エスケープ列は 0 桁、全角は 2 桁。色付きプロンプトの幅計算に要る。 */
int cdk_shell_visible_width(const char *s, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* CDK_SHELL_H */
