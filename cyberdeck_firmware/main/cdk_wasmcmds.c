/* `wasm` コマンド。ユーザーアプリの起動。止めるのは Ctrl-C（main.c）。
 *
 * ここは **セッションを切り替えて即 return する**。`edit` と同じ作法で、
 * コマンドの中で待たない。待つとメインループが止まり、画面もキーも死ぬ。
 */
#include "app.h"

#include "cdk_storage.h"
#include "cdk_wasm.h"
#include "cdk_wasm_run.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static void usage(cdk_shell_t *sh)
{
    cdk_shell_write(sh,
                    "使い方:\r\n"
                    "  wasm run <file> [引数...]  microSD の .wasm を走らせる\r\n"
                    "  wasm ls [dir]              置いてあるアプリを並べる（既定 apps）\r\n"
                    "  wasm info                  ABI と上限\r\n"
                    "\r\n"
                    "動いているアプリは Ctrl-C で止める。\r\n");
}

/* apps/ に何が置いてあるか。ls の代わりに .wasm だけ拾う。 */
static void list_apps(cdk_shell_t *sh, const char *dir)
{
    if (!cdk_sd_mounted()) {
        cdk_shell_write(sh, "\033[31mSD がマウントされていません (sd mount)\033[0m\r\n");
        return;
    }

    char abs[CDK_WASM_PATH_MAX];
    if (cdk_wasm_path_resolve(dir, (int)strlen(dir), abs, (int)sizeof abs) != CDK_E_OK) {
        cdk_shell_printf(sh, "\033[31m%s は microSD の外です\033[0m\r\n", dir);
        return;
    }

    cdk_sd_lock();
    DIR *d = opendir(abs);
    cdk_sd_unlock();
    if (!d) {
        cdk_shell_printf(sh, "\033[33m%s がありません\033[0m\r\n", abs);
        return;
    }

    int n = 0;
    for (;;) {
        cdk_sd_lock();
        const struct dirent *e = readdir(d);
        cdk_sd_unlock();
        if (!e) break;

        const size_t len = strlen(e->d_name);
        if (len < 6 || strcmp(e->d_name + len - 5, ".wasm") != 0) continue;

        /* FATFS の長いファイル名は 255 バイトまで来る。入らないものは
         * 大きさを出さずに名前だけ並べる（切り詰めた名前で stat しない）。 */
        char       full[CDK_WASM_PATH_MAX + 260];
        const int  want = snprintf(full, sizeof full, "%s/%s", abs, e->d_name);
        long       size = -1;
        if (want > 0 && want < (int)sizeof full) {
            struct stat st;
            cdk_sd_lock();
            const int ok = stat(full, &st);
            cdk_sd_unlock();
            if (ok == 0) size = (long)st.st_size;
        }

        if (size >= 0) {
            cdk_shell_printf(sh, "  %-28s %ld\r\n", e->d_name, size);
        } else {
            cdk_shell_printf(sh, "  %-28s ?\r\n", e->d_name);
        }
        n++;
    }
    cdk_sd_lock();
    closedir(d);
    cdk_sd_unlock();

    if (n == 0) cdk_shell_printf(sh, "  %s に .wasm はありません\r\n", abs);
}

static void show_info(cdk_shell_t *sh)
{
    cdk_shell_printf(sh,
                     "ランタイム  : wasm3 (インタプリタ)\r\n"
                     "ABI         : \"%s\" / エントリ %s\r\n"
                     "触れる場所  : %s の下だけ\r\n"
                     "上限        : モジュール %dKB / 同時 1 本\r\n"
                     "              ファイル %d / ディレクトリ %d / ソケット %d\r\n"
                     "状態        : %s\r\n",
                     CDK_WASM_IMPORT_MODULE, CDK_WASM_ENTRY, CDK_WASM_ROOT,
                     CDK_WASM_MODULE_MAX / 1024,
                     CDK_WASM_FILES_MAX, CDK_WASM_DIRS_MAX, CDK_WASM_SOCKS_MAX,
                     cdk_wasm_run_busy() ? "動いています" : "待機");
}

static int cmd_wasm(int argc, char **argv, void *user)
{
    app_ctx_t   *ctx = (app_ctx_t *)user;
    cdk_shell_t *sh = ctx->sh;

    if (argc < 2) { usage(sh); return 1; }

    if (strcmp(argv[1], "ls") == 0) {
        list_apps(sh, (argc > 2) ? argv[2] : "apps");
        return 0;
    }

    if (strcmp(argv[1], "info") == 0) {
        show_info(sh);
        return 0;
    }

    /* `wasm stop` は置かない。**アプリが走っている間はシェルが止まって
     * いる**（enter_wasm が cdk_shell_end を呼ぶ）ので、この表に入る道が
     * 無く、書いても「動いているアプリはありません」としか言えない。
     * 止める手段は Ctrl-C だけで、main.c の route_key が横取りする。 */

    if (strcmp(argv[1], "run") != 0) { usage(sh); return 1; }
    if (argc < 3) { usage(sh); return 1; }

    if (!cdk_sd_mounted()) {
        cdk_shell_write(sh, "\033[31mSD がマウントされていません (sd mount)\033[0m\r\n");
        return 1;
    }

    /* **argv はここから先へ持ち越せない。** cdk_shell_split が返す
     * ポインタは execute() のスタックを指していて、この関数が return した
     * 時点で無効になる。cdk_wasm_run_start が中で複製する。 */
    if (!ctx->wasm_open(argv[2], argc - 2, argv + 2)) return 1;

    return 0;
}

const cdk_shell_cmd_t APP_WASM_COMMANDS[] = {
    {"wasm", "wasm run <file> [引数...] | ls | info（止めるのは Ctrl-C）", cmd_wasm},
};
const size_t APP_WASM_COMMANDS_COUNT =
    sizeof(APP_WASM_COMMANDS) / sizeof(APP_WASM_COMMANDS[0]);
