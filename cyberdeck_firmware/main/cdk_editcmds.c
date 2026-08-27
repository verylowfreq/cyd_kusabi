/* エディタのコマンドと、SD への読み書き。
 *
 * 編集そのものは components/cdk_edit（純 C）。ここは
 * 「SD からバイト列を渡す / 受け取る」だけを持つ。
 */
#include "app.h"

#include "cdk_edit.h"
#include "cdk_storage.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static app_ctx_t *ctx_of(void *user)
{
    return (app_ctx_t *)user;
}

void cdk_edit_resolve_path(const char *arg, char *out, size_t cap)
{
    if (arg == NULL || arg[0] == '\0') {
        snprintf(out, cap, "%s", CDK_SD_MOUNT_POINT);
    } else if (arg[0] == '/') {
        if (strncmp(arg, CDK_SD_MOUNT_POINT, strlen(CDK_SD_MOUNT_POINT)) == 0) {
            snprintf(out, cap, "%s", arg);
        } else {
            snprintf(out, cap, "%s%s", CDK_SD_MOUNT_POINT, arg);
        }
    } else {
        snprintf(out, cap, "%s/%s", CDK_SD_MOUNT_POINT, arg);
    }
}

/* ファイルを読み込む。無ければ新規として開く。 */
bool cdk_edit_file_load(cdk_edit_t *e, const char *path)
{
    if (!cdk_sd_mounted()) {
        cdk_edit_new(e, path);
        cdk_edit_message(e, "SD がありません。保存できません");
        e->readonly = true;
        return false;
    }

    cdk_sd_lock();
    FILE *f = fopen(path, "rb");
    long  size = 0;
    if (f != NULL) {
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fseek(f, 0, SEEK_SET);
    }
    cdk_sd_unlock();

    if (f == NULL) {
        cdk_edit_new(e, path);
        cdk_edit_message(e, "新しいファイル");
        return true;
    }
    if (size < 0 || (size_t)size > e->cap) {
        cdk_sd_lock();
        fclose(f);
        cdk_sd_unlock();
        /* 入りきらないものは読み込まない。途中まで読んで保存すると
         * その時点で後半が消える。 */
        (void)cdk_edit_load(e, path, NULL, (size_t)((size > 0) ? size : 0) + 1);
        return false;
    }

    cdk_sd_lock();
    const size_t got = fread(e->buf, 1, (size_t)size, f);
    fclose(f);
    cdk_sd_unlock();

    if (got != (size_t)size) {
        cdk_edit_new(e, path);
        cdk_edit_message(e, "読み込みに失敗しました");
        e->readonly = true;
        return false;
    }
    /* buf へ直接読んだので、長さだけ合わせる。cdk_edit_load に e->buf を
     * 渡すと memcpy の src と dst が重なる。 */
    (void)cdk_edit_adopt(e, path, got);
    return true;
}

/* 保存する。
 *
 * **いったん .tmp に書いてから rename する。** 元のファイルに直接書くと、
 * 途中で電源が落ちたり SD が抜けたりしたときに、書きかけのものが
 * 元の場所に残る。書き上がったものだけが元の名前になるようにする。 */
bool cdk_edit_file_save(cdk_edit_t *e)
{
    if (e->readonly) {
        cdk_edit_message(e, "読取専用です");
        return false;
    }
    if (!cdk_sd_mounted()) {
        cdk_edit_message(e, "SD がありません");
        return false;
    }
    if (e->path[0] == '\0') {
        cdk_edit_message(e, "ファイル名がありません");
        return false;
    }

    char tmp[CDK_EDIT_PATH_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", e->path);

    size_t      len = 0;
    const char *text = cdk_edit_text(e, &len);

    cdk_sd_lock();
    FILE  *f = fopen(tmp, "wb");
    size_t wrote = 0;
    bool   ok = false;
    if (f != NULL) {
        wrote = (len > 0) ? fwrite(text, 1, len, f) : 0;
        ok = (fclose(f) == 0) && (wrote == len);
    }
    if (ok) {
        /* FATFS の rename は上書きしないので、先に消す。
         * ここで落ちると元が無い状態になるが、.tmp は残っている。 */
        remove(e->path);
        ok = (rename(tmp, e->path) == 0);
    }
    if (!ok) {
        remove(tmp);
    }
    cdk_sd_unlock();

    if (!ok) {
        cdk_edit_message(e, "保存できません");
        return false;
    }
    cdk_edit_saved(e);
    char msg[CDK_EDIT_MSG_MAX];
    /* パスは長いと切る。メッセージ全体はどのみち画面幅で切られる。 */
    snprintf(msg, sizeof(msg), "%.48s に %u バイト保存しました", e->path,
             (unsigned)len);
    cdk_edit_message(e, msg);
    return true;
}

/* ------------------------------------------------------------------ */

static int cmd_edit(int argc, char **argv, void *user)
{
    app_ctx_t   *c = ctx_of(user);
    cdk_shell_t *sh = c->sh;

    if (argc < 2) {
        cdk_shell_write(sh, "使い方: edit <ファイル>\r\n"
                            "  Ctrl-S 保存 / Ctrl-Q 終了 / Ctrl-J かな入力\r\n");
        return 1;
    }
    if (!c->edit_open(argv[1])) {
        cdk_shell_write(sh, "\033[31mエディタを開けません\033[0m\r\n");
        return 1;
    }
    return 0;
}

const cdk_shell_cmd_t APP_EDIT_COMMANDS[] = {
    {"edit", "テキストを編集する (edit <file>)", cmd_edit},
};

const size_t APP_EDIT_COMMANDS_COUNT =
    sizeof(APP_EDIT_COMMANDS) / sizeof(APP_EDIT_COMMANDS[0]);
