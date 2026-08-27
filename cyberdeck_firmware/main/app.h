/* main.c と cdk_commands.c の間の取り決め。
 *
 * セッションの切り替え（シェル ⇄ SSH）は main.c が 1 か所で持つ。
 * コマンドはここ経由で頼むだけで、自分で状態を持たない。
 * 分散すると「このキーは誰が受け取るのか」が条件分岐の集合になって追えなくなる。
 */
#pragma once

#include "cdk_shell.h"
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

    void (*reboot)(void);
} app_ctx_t;

extern const cdk_shell_cmd_t APP_COMMANDS[];
extern const size_t          APP_COMMANDS_COUNT;
