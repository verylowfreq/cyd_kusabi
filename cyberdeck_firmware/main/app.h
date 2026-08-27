/* main.c と cdk_commands.c の間の取り決め。
 *
 * セッションの切り替え（シェル ⇄ SSH）は main.c が 1 か所で持つ。
 * コマンドはここ経由で頼むだけで、自分で状態を持たない。
 * 分散すると「このキーは誰が受け取るのか」が条件分岐の集合になって追えなくなる。
 */
#pragma once

#include "cdk_edit.h"
#include "cdk_shell.h"
#include "cdk_skk.h"
#include "cdk_term.h"

typedef struct {
    cdk_shell_t *sh;
    cdk_term_t  *term;

    /* NVS の設定を読み直して WiFi へ繋ぐ。 */
    void (*wifi_connect)(void);
    void (*wifi_status)(void);

    /* SSH を張ってセッションを切り替える。成功したら true。 */
    bool (*ssh_open)(void);
    void (*ssh_close)(void);
    bool (*ssh_is_open)(void);

    /* 日本語入力。状態は main.c が 1 つだけ持つ。 */
    void                  (*skk_set_mode)(cdk_skk_mode_t m);
    cdk_skk_mode_t        (*skk_get_mode)(void);
    const cdk_skk_dict_t *(*skk_dict)(void);

    /* エディタを開いてセッションを切り替える。 */
    bool (*edit_open)(const char *path);

    void (*reboot)(void);
} app_ctx_t;

/* main/cdk_editcmds.c。SD との読み書きだけを持つ。 */
void cdk_edit_resolve_path(const char *arg, char *out, size_t cap);
bool cdk_edit_file_load(cdk_edit_t *e, const char *path);
bool cdk_edit_file_save(cdk_edit_t *e);

/* コマンド表。main.c が 2 つを繋いでシェルに渡す。 */
extern const cdk_shell_cmd_t APP_COMMANDS[];
extern const size_t          APP_COMMANDS_COUNT;

extern const cdk_shell_cmd_t APP_FS_COMMANDS[];
extern const size_t          APP_FS_COMMANDS_COUNT;

extern const cdk_shell_cmd_t APP_SKK_COMMANDS[];
extern const size_t          APP_SKK_COMMANDS_COUNT;

extern const cdk_shell_cmd_t APP_EDIT_COMMANDS[];
extern const size_t          APP_EDIT_COMMANDS_COUNT;
