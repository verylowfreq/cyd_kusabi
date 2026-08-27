/* SKK まわりのコマンド。
 *
 * `skkdict` を先に作ったのは意図的で、**入力の配線より前に
 * 「辞書がフラッシュから引けること」だけを確かめたい**ため。
 * 一度に両方を繋ぐと、変換できないときに辞書とキー配線のどちらが
 * 悪いのかを 6x12 の画面越しに切り分けることになる。
 */
#include "app.h"

#include "cdk_skk.h"
#include "cdk_skkdict_src.h"

#include <stdlib.h>
#include <string.h>

static app_ctx_t *ctx_of(void *user)
{
    return (app_ctx_t *)user;
}

static int cmd_skk(int argc, char **argv, void *user)
{
    app_ctx_t   *c = ctx_of(user);
    cdk_shell_t *sh = c->sh;

    if (argc >= 2 && strcmp(argv[1], "on") == 0) {
        c->skk_set_mode(CDK_SKK_HIRAGANA);
        cdk_shell_write(sh, "かな入力: \033[32mオン\033[0m\r\n");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "off") == 0) {
        c->skk_set_mode(CDK_SKK_ASCII);
        cdk_shell_write(sh, "かな入力: オフ\r\n");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "kata") == 0) {
        c->skk_set_mode(CDK_SKK_KATAKANA);
        cdk_shell_write(sh, "かな入力: \033[32mカタカナ\033[0m\r\n");
        return 0;
    }
    if (argc >= 2) {
        cdk_shell_write(sh, "使い方: skk [on|off|kata]\r\n");
        return 1;
    }

    static const char *const NAMES[] = {"オフ", "ひらがな", "カタカナ", "半角カナ",
                                        "全英"};
    const int m = (int)c->skk_get_mode();
    cdk_shell_printf(sh, "モード  : %s\r\n", (m >= 0 && m < 5) ? NAMES[m] : "?");
    cdk_shell_printf(sh, "辞書    : %s\r\n", cdk_skkdict_describe());
    cdk_shell_printf(sh, "個人辞書: %s に %d 件追記\r\n", CDK_SKK_USER_PATH,
                     cdk_skkuser_written());
    cdk_shell_write(sh, "切り替え: Ctrl-J（cdk.py key ctrl+j）\r\n");
    return 0;
}

static int cmd_skkdict(int argc, char **argv, void *user)
{
    app_ctx_t   *c = ctx_of(user);
    cdk_shell_t *sh = c->sh;

    if (argc < 2) {
        cdk_shell_write(sh, "使い方: skkdict <見出し語> [送りの子音]\r\n"
                            "        skkdict にほん\r\n"
                            "        skkdict かえ r     （送りあり）\r\n");
        cdk_shell_printf(sh, "辞書: %s\r\n", cdk_skkdict_describe());
        return 1;
    }

    const cdk_skk_dict_t *d = c->skk_dict();
    if (d == NULL || !d->ready) {
        cdk_shell_write(sh, "\033[31m辞書が開いていません\033[0m\r\n");
        return 1;
    }

    char key[CDK_SKK_KEY_MAX];
    snprintf(key, sizeof(key), "%s%s", argv[1], (argc >= 3) ? argv[2] : "");
    const bool ari = (argc >= 3);

    static char cands[CDK_SKK_CAND_MAX];
    if (!cdk_skk_dict_lookup(d, key, ari, cands, sizeof(cands))) {
        cdk_shell_printf(sh, "\033[33m%s は %s の索引にありません\033[0m\r\n", key,
                         ari ? "送りあり" : "送りなし");
        return 1;
    }

    const int n = cdk_skk_cand_count(cands);
    cdk_shell_printf(sh, "%s (%s) %d 件\r\n", key, ari ? "送りあり" : "送りなし", n);
    for (int i = 0; i < n && i < 12; i++) {
        char cand[256];
        char annot[128];
        if (!cdk_skk_cand_at(cands, i, cand, sizeof(cand), annot, sizeof(annot))) {
            break;
        }
        if (annot[0] != '\0') {
            cdk_shell_printf(sh, "  %d. %s \033[90m; %s\033[0m\r\n", i + 1, cand, annot);
        } else {
            cdk_shell_printf(sh, "  %d. %s\r\n", i + 1, cand);
        }
    }
    if (n > 12) {
        cdk_shell_printf(sh, "  ... 残り %d 件\r\n", n - 12);
    }
    return 0;
}

const cdk_shell_cmd_t APP_SKK_COMMANDS[] = {
    {"skk",     "かな入力 (skk on|off|kata)",     cmd_skk},
    {"skkdict", "辞書を引く (skkdict <見出し>)",  cmd_skkdict},
};

const size_t APP_SKK_COMMANDS_COUNT =
    sizeof(APP_SKK_COMMANDS) / sizeof(APP_SKK_COMMANDS[0]);
