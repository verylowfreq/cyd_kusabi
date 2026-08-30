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

    /* ユーザーアプリ (.wasm) を走らせてセッションを切り替える。
     * argv は中で複製される（cdk_shell_split の argv はコマンドが
     * return した時点で無効になるため）。 */
    bool (*wasm_open)(const char *path, int argc, char **argv);

    void (*reboot)(void);
} app_ctx_t;

/* main/cdk_editcmds.c。SD との読み書きだけを持つ。 */
void cdk_edit_resolve_path(const char *arg, char *out, size_t cap);

/* NVS の "gamma"（無ければ基板の既定値）を画面に反映し、全面を描き直す。
 * `set gamma <値>` から呼ぶので、焼き直さずに実機で追い込める。 */
void cdk_apply_gamma_setting(void);

/* NVS の "keymap"（us / jis）を USB キーボードに反映する。
 * `set keymap jis` から呼ぶので、焼き直さずに切り替えられる。 */
void cdk_apply_keymap_setting(void);

/* SSH の秘密鍵のパス（NVS の ssh.key、無ければ SD の既定）。
 * **唯一の出どころ。** `ssh key` が見る場所と認証で使う場所を
 * 一致させるため、両方ここを呼ぶ。 */
const char *cdk_ssh_key_path(void);

/* NVS の ssh.recon を反映する。`set ssh.recon off` から呼ぶ。 */
void cdk_apply_reconnect_setting(void);
/* main/cdk_pushfile.c。ホストから microSD へファイルを書く（cdk.py push）。
 * 書ける場所は cdk_wasm と同じサンドボックス（/sdcard の下だけ）。 */
void cdk_push_begin(const uint8_t *payload, uint32_t len);
void cdk_push_data(const uint8_t *payload, uint32_t len);
void cdk_push_end(const uint8_t *payload, uint32_t len);

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

extern const cdk_shell_cmd_t APP_WASM_COMMANDS[];
extern const size_t          APP_WASM_COMMANDS_COUNT;
