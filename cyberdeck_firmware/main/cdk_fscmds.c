/* microSD 上のファイル操作コマンド。
 *
 * 実際の読み書きは普通の stdio / dirent。LCD と SPI を共有しているので、
 * どれも cdk_sd_lock() で囲む。囲み忘れると読み書きが化ける。
 */
#include "app.h"

#include "cdk_storage.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* 相対パスは /sdcard からの相対として扱う。カレントディレクトリは持たない。 */
static void resolve(const char *arg, char *out, size_t cap)
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

static bool need_sd(cdk_shell_t *sh)
{
    if (cdk_sd_mounted()) {
        return true;
    }
    cdk_shell_write(sh, "\033[31mSD がマウントされていません (sd mount)\033[0m\r\n");
    return false;
}

static void human(uint64_t bytes, char *out, size_t cap)
{
    if (bytes >= 1024ULL * 1024 * 1024) {
        snprintf(out, cap, "%.1fG", (double)bytes / (1024.0 * 1024 * 1024));
    } else if (bytes >= 1024ULL * 1024) {
        snprintf(out, cap, "%.1fM", (double)bytes / (1024.0 * 1024));
    } else if (bytes >= 1024) {
        snprintf(out, cap, "%.1fK", (double)bytes / 1024.0);
    } else {
        snprintf(out, cap, "%uB", (unsigned)bytes);
    }
}

/* ------------------------------------------------------------------ */

static int cmd_sd(int argc, char **argv, void *user)
{
    cdk_shell_t *sh = ((app_ctx_t *)user)->sh;

    if (argc >= 2 && strcmp(argv[1], "mount") == 0) {
        const esp_err_t err = cdk_sd_mount();
        if (err != ESP_OK) {
            cdk_shell_printf(sh, "\033[31mマウントできません: %s\033[0m\r\n",
                             esp_err_to_name(err));
            return 1;
        }
        cdk_shell_write(sh, "マウントしました\r\n");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "unmount") == 0) {
        cdk_sd_unmount();
        cdk_shell_write(sh, "アンマウントしました\r\n");
        return 0;
    }

    if (!cdk_sd_mounted()) {
        cdk_shell_write(sh, "未マウント (sd mount)\r\n");
        return 0;
    }

    char     name[24] = {0};
    uint64_t cap = 0;
    uint32_t khz = 0;
    cdk_sd_info(name, sizeof(name), &cap, &khz);

    char capbuf[16];
    human(cap, capbuf, sizeof(capbuf));
    cdk_shell_printf(sh, "カード  : %s  %s  %u kHz\r\n", name, capbuf, (unsigned)khz);

    uint64_t total = 0, freeb = 0;
    if (cdk_sd_usage(&total, &freeb) == ESP_OK) {
        char t[16], f[16];
        human(total, t, sizeof(t));
        human(freeb, f, sizeof(f));
        cdk_shell_printf(sh, "容量    : %s 中 %s 空き\r\n", t, f);
    }
    return 0;
}

static int cmd_ls(int argc, char **argv, void *user)
{
    cdk_shell_t *sh = ((app_ctx_t *)user)->sh;
    if (!need_sd(sh)) {
        return 1;
    }

    char path[256];
    resolve((argc >= 2) ? argv[1] : NULL, path, sizeof(path));

    cdk_sd_lock();
    DIR *dir = opendir(path);
    if (dir == NULL) {
        cdk_sd_unlock();
        cdk_shell_printf(sh, "\033[31m%s を開けません\033[0m\r\n", path);
        return 1;
    }

    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);

        struct stat st;
        const bool ok = stat(full, &st) == 0;
        const bool isdir = ok && S_ISDIR(st.st_mode);

        if (isdir) {
            cdk_shell_printf(sh, "  \033[34m%s/\033[0m\r\n", ent->d_name);
        } else {
            char sz[16];
            human(ok ? (uint64_t)st.st_size : 0, sz, sizeof(sz));
            cdk_shell_printf(sh, "  %-24s %6s\r\n", ent->d_name, sz);
        }
        n++;
    }
    closedir(dir);
    cdk_sd_unlock();

    cdk_shell_printf(sh, "%d 件\r\n", n);
    return 0;
}

static int cmd_cat(int argc, char **argv, void *user)
{
    cdk_shell_t *sh = ((app_ctx_t *)user)->sh;
    if (!need_sd(sh)) {
        return 1;
    }
    if (argc < 2) {
        cdk_shell_write(sh, "使い方: cat <ファイル>\r\n");
        return 1;
    }

    char path[256];
    resolve(argv[1], path, sizeof(path));

    cdk_sd_lock();
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        cdk_sd_unlock();
        cdk_shell_printf(sh, "\033[31m%s を開けません\033[0m\r\n", path);
        return 1;
    }

    /* 端末へそのまま流す。改行は CR LF に直さないと段差になる。 */
    char buf[256];
    size_t total = 0;
    for (;;) {
        const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        if (n == 0) {
            break;
        }
        buf[n] = '\0';
        for (size_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                cdk_shell_write(sh, "\r\n");
            } else {
                const char one[2] = {buf[i], '\0'};
                cdk_shell_write(sh, one);
            }
        }
        total += n;
        if (total > 64 * 1024) {
            cdk_shell_write(sh, "\r\n\033[33m(64KB で打ち切り)\033[0m\r\n");
            break;
        }
    }
    fclose(f);
    cdk_sd_unlock();
    cdk_shell_write(sh, "\r\n");
    return 0;
}

static int cmd_rm(int argc, char **argv, void *user)
{
    cdk_shell_t *sh = ((app_ctx_t *)user)->sh;
    if (!need_sd(sh)) {
        return 1;
    }
    if (argc < 2) {
        cdk_shell_write(sh, "使い方: rm <ファイル>\r\n");
        return 1;
    }

    char path[256];
    resolve(argv[1], path, sizeof(path));

    cdk_sd_lock();
    const int rc = unlink(path);
    cdk_sd_unlock();

    if (rc != 0) {
        cdk_shell_printf(sh, "\033[31m%s を消せません\033[0m\r\n", path);
        return 1;
    }
    cdk_shell_printf(sh, "%s を消しました\r\n", path);
    return 0;
}

static int cmd_mkdir(int argc, char **argv, void *user)
{
    cdk_shell_t *sh = ((app_ctx_t *)user)->sh;
    if (!need_sd(sh)) {
        return 1;
    }
    if (argc < 2) {
        cdk_shell_write(sh, "使い方: mkdir <ディレクトリ>\r\n");
        return 1;
    }

    char path[256];
    resolve(argv[1], path, sizeof(path));

    cdk_sd_lock();
    const int rc = mkdir(path, 0777);
    cdk_sd_unlock();

    if (rc != 0) {
        cdk_shell_printf(sh, "\033[31m%s を作れません\033[0m\r\n", path);
        return 1;
    }
    cdk_shell_printf(sh, "%s を作りました\r\n", path);
    return 0;
}

/* 動作確認用。SD が本当に読み書きできているかを 1 コマンドで見る。 */
static int cmd_sdtest(int argc, char **argv, void *user)
{
    (void)argc;
    (void)argv;
    cdk_shell_t *sh = ((app_ctx_t *)user)->sh;
    if (!need_sd(sh)) {
        return 1;
    }

    const char *path = CDK_SD_MOUNT_POINT "/cdk_test.txt";
    static const char payload[] =
        "cyberdeck M4 の書き込みテスト\n"
        "日本語も UTF-8 でそのまま入る\n"
        "0123456789 ABCDEFGHIJ\n";

    cdk_sd_lock();
    FILE *f = fopen(path, "wb");
    const bool wrote = (f != NULL) && (fwrite(payload, 1, sizeof(payload) - 1, f) ==
                                       sizeof(payload) - 1);
    if (f != NULL) {
        fclose(f);
    }

    char back[256] = {0};
    size_t got = 0;
    if (wrote) {
        FILE *r = fopen(path, "rb");
        if (r != NULL) {
            got = fread(back, 1, sizeof(back) - 1, r);
            fclose(r);
        }
    }
    cdk_sd_unlock();

    if (!wrote) {
        cdk_shell_write(sh, "\033[31m書き込みに失敗\033[0m\r\n");
        return 1;
    }
    if (got != sizeof(payload) - 1 || memcmp(back, payload, got) != 0) {
        cdk_shell_printf(sh, "\033[31m読み戻しが一致しません (%u / %u バイト)\033[0m\r\n",
                         (unsigned)got, (unsigned)(sizeof(payload) - 1));
        return 1;
    }
    cdk_shell_printf(sh, "\033[32m書いて読み戻して一致 (%u バイト)\033[0m\r\n",
                     (unsigned)got);
    return 0;
}

const cdk_shell_cmd_t APP_FS_COMMANDS[] = {
    {"sd",     "SD の状態 (sd mount / sd unmount)", cmd_sd},
    {"ls",     "一覧 (ls [パス])",                  cmd_ls},
    {"cat",    "中身を出す (cat <file>)",           cmd_cat},
    {"rm",     "消す (rm <file>)",                  cmd_rm},
    {"mkdir",  "作る (mkdir <dir>)",                cmd_mkdir},
    {"sdtest", "SD に書いて読み戻す",               cmd_sdtest},
};

const size_t APP_FS_COMMANDS_COUNT =
    sizeof(APP_FS_COMMANDS) / sizeof(APP_FS_COMMANDS[0]);
