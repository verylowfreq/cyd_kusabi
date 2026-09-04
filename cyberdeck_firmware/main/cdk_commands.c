/* シェルの組み込みコマンド。
 *
 * ここは「引数を解いて port 層を呼び、結果を整形する」だけに留める。
 * 実際の操作は components_port/ 側が持つ。
 */
#include "app.h"

#include "cdk_cfg.h"
#include "cdk_knownhosts.h"
#include "cdk_net.h"
#include "cdk_periph.h"
#include "cdk_storage.h" /* cdk_sd_mounted — scp の前に SD を見る */

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"

#include <stdlib.h>
#include <string.h>

static app_ctx_t *ctx_of(void *user)
{
    return (app_ctx_t *)user;
}

/* port_periph へ渡す printf 風の出力先 */
static void periph_out(void *user, const char *fmt, ...)
{
    cdk_shell_t *sh = (cdk_shell_t *)user;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if (n >= (int)sizeof(buf)) {
        n = (int)sizeof(buf) - 1;
    }
    buf[n] = '\0';
    cdk_shell_write(sh, buf);
}

/* ------------------------------------------------------------------ */

static int cmd_help(int argc, char **argv, void *user)
{
    (void)argc;
    (void)argv;
    cdk_shell_t *sh = ctx_of(user)->sh;

    /* シェルに登録されている表をそのまま見る。
     * コマンド表を増やしてもここを直さなくて済む。 */
    cdk_shell_write(sh, "\033[1m使えるコマンド\033[0m\r\n");
    for (size_t i = 0; i < sh->ncmds; i++) {
        cdk_shell_printf(sh, "  \033[36m%-7s\033[0m %s\r\n", sh->cmds[i].name,
                         sh->cmds[i].usage);
    }
    return 0;
}

static int cmd_clear(int argc, char **argv, void *user)
{
    (void)argc;
    (void)argv;
    cdk_shell_write(ctx_of(user)->sh, "\033[2J\033[H");
    return 0;
}

static int cmd_echo(int argc, char **argv, void *user)
{
    cdk_shell_t *sh = ctx_of(user)->sh;
    for (int i = 1; i < argc; i++) {
        cdk_shell_printf(sh, "%s%s", (i > 1) ? " " : "", argv[i]);
    }
    cdk_shell_write(sh, "\r\n");
    return 0;
}

static int cmd_history(int argc, char **argv, void *user)
{
    (void)argc;
    (void)argv;
    cdk_shell_t *sh = ctx_of(user)->sh;
    /* 新しいものが 0 番なので、古い順に並べ直して出す */
    for (int i = sh->hist_count - 1; i >= 0; i--) {
        cdk_shell_printf(sh, "%3d  %s\r\n", sh->hist_count - i, sh->hist[i]);
    }
    return 0;
}

static int cmd_info(int argc, char **argv, void *user)
{
    (void)argc;
    (void)argv;
    app_ctx_t *ctx = ctx_of(user);
    cdk_shell_t *sh = ctx->sh;

    const esp_app_desc_t *app = esp_app_get_description();
    cdk_shell_printf(sh, "app     : %s %s\r\n", app->project_name, app->version);
    cdk_shell_printf(sh, "idf     : %s\r\n", app->idf_ver);
    cdk_shell_printf(sh, "端末    : %d桁 x %d行\r\n", ctx->term->cols, ctx->term->rows);
    cdk_shell_printf(sh, "稼働    : %llu 秒\r\n",
                     (unsigned long long)(esp_timer_get_time() / 1000000));

    char ip[20];
    cdk_wifi_ip(ip, sizeof(ip));
    cdk_shell_printf(sh, "wifi    : %s (%s)\r\n",
                     cdk_wifi_is_connected() ? "接続" : "未接続", ip);
    cdk_shell_printf(sh, "ssh     : %s\r\n", ctx->ssh_is_open() ? "接続" : "未接続");
    return 0;
}

static int cmd_free(int argc, char **argv, void *user)
{
    (void)argc;
    (void)argv;
    cdk_shell_t *sh = ctx_of(user)->sh;
    cdk_shell_printf(sh, "internal: %u バイト空き (最大ブロック %u)\r\n",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cdk_shell_printf(sh, "spiram  : %u バイト空き (最大ブロック %u)\r\n",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    return 0;
}

static int cmd_reboot(int argc, char **argv, void *user)
{
    (void)argc;
    (void)argv;
    app_ctx_t *ctx = ctx_of(user);
    cdk_shell_write(ctx->sh, "再起動します...\r\n");
    ctx->reboot();
    return 0;
}

/* ------------------------------------------------------------------ */
/* 設定                                                                */
/* ------------------------------------------------------------------ */

static int cmd_set(int argc, char **argv, void *user)
{
    cdk_shell_t *sh = ctx_of(user)->sh;

    if (argc == 1) {
        const char *const *keys = cdk_cfg_keys();
        for (size_t i = 0; keys[i] != NULL; i++) {
            char buf[128];
            const bool has = cdk_cfg_get(keys[i], buf, sizeof(buf)) == ESP_OK;
            if (!has || buf[0] == '\0') {
                /* **未設定でも既定が効くものは、その値を出す。**
                 * 「(未設定)」だけだと、設定しないと動かないのか、
                 * 既定で動いているのかが読み取れない。 */
                const char *dflt = NULL;
                if (strcmp(keys[i], CDK_CFG_SSH_KEY) == 0) {
                    dflt = cdk_ssh_key_path();
                } else if (strcmp(keys[i], CDK_CFG_SSH_RECONNECT) == 0) {
                    dflt = "on";
                } else if (strcmp(keys[i], CDK_CFG_FONTSCALE) == 0) {
                    dflt = "1";
                } else if (strcmp(keys[i], CDK_CFG_STATUSBAR) == 0) {
                    dflt = "off";
                }
                if (dflt != NULL) {
                    cdk_shell_printf(sh, "  %-10s \033[90m%s (既定)\033[0m\r\n",
                                     keys[i], dflt);
                } else {
                    cdk_shell_printf(sh, "  %-10s \033[90m(未設定)\033[0m\r\n", keys[i]);
                }
            } else if (cdk_cfg_is_secret(keys[i])) {
                /* パスワードは伏せる。ただし「設定されている」ことと桁数は出す。
                 * 全部隠すと設定できているのか分からず、かえって危ない。 */
                cdk_shell_printf(sh, "  %-10s \033[90m(設定済み %u 文字)\033[0m\r\n",
                                 keys[i], (unsigned)strlen(buf));
            } else {
                cdk_shell_printf(sh, "  %-10s %s\r\n", keys[i], buf);
            }
        }
        return 0;
    }

    if (argc == 2) {
        cdk_shell_printf(sh, "使い方: set <キー> <値>   （空文字列で削除）\r\n");
        return 1;
    }

    const esp_err_t err = cdk_cfg_set(argv[1], argv[2]);
    if (err != ESP_OK) {
        cdk_shell_printf(sh, "\033[31m書けません: %s\033[0m\r\n", esp_err_to_name(err));
        return 1;
    }
    cdk_shell_printf(sh, "%s を設定しました\r\n", argv[1]);

    /* ガンマだけはその場で反映する。パネルの明るさは目で決めるしかないので、
     * 焼き直しや再起動を挟むと追い込めない。 */
    if (strcmp(argv[1], CDK_CFG_GAMMA) == 0) {
        cdk_apply_gamma_setting();
    }
    if (strcmp(argv[1], CDK_CFG_KEYMAP) == 0) {
        cdk_apply_keymap_setting();
    }
    if (strcmp(argv[1], CDK_CFG_SSH_RECONNECT) == 0) {
        cdk_apply_reconnect_setting();
    }
    /* 拡大率だけは再起動が要る。桁数と行数が変わるので端末を作り直す
     * 必要があり、その場でやると画面の中身が消える。**黙って効かないより、
     * 効かないと言うほうがいい。** */
    if (strcmp(argv[1], CDK_CFG_FONTSCALE) == 0 ||
        strcmp(argv[1], CDK_CFG_STATUSBAR) == 0) {
        cdk_shell_printf(sh, "\033[33m再起動すると反映されます (reboot)\033[0m\r\n");
    }
    return 0;
}

static int cmd_wifi(int argc, char **argv, void *user)
{
    app_ctx_t *ctx = ctx_of(user);
    cdk_shell_t *sh = ctx->sh;

    if (argc >= 2 && strcmp(argv[1], "connect") == 0) {
        ctx->wifi_connect();
        return 0;
    }
    /* 繋ぎ直しをやめて電波も止める。**外へ持ち出すとき用。**
     * 一度繋がったあとの切断は期限なしで繋ぎ直し続けるので、
     * これが無いと電池が減るのを止められない（DESIGN.md §8.1）。 */
    if (argc >= 2 && strcmp(argv[1], "off") == 0) {
        ctx->wifi_off();
        return 0;
    }
    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        char ip[20];
        cdk_wifi_ip(ip, sizeof(ip));
        /* 帯と同じ 3 通りを出す。帯が「WiFi接続中」なのに
         * ここが「未接続」だと、どちらが本当か分からない。 */
        const char *label;
        if (cdk_wifi_is_connected()) {
            label = "\033[32m接続\033[0m";
        } else if (cdk_wifi_poll() == CDK_WIFI_CONNECTING) {
            label = "\033[33m接続中\033[0m";
        } else {
            label = "未接続";
        }
        cdk_shell_printf(sh, "%s  ip=%s  直近の切断理由=%d\r\n", label, ip,
                         cdk_wifi_last_disconnect_reason());
        return 0;
    }
    cdk_shell_write(sh, "使い方: wifi [status|connect|off]\r\n");
    return 1;
}

/* 公開鍵の 1 行は 200 バイト前後ある。**スタックに積まない。**
 * シェルはメインループからしか動かないので static でよい。
 * メインタスクのスタックは libssh2 と mbedTLS が食うので、
 * こちらは削れるだけ削る（不具合 #40）。 */
static char s_key_line[512];
static char s_key_fp[80];

/* `ssh user@host` の分解先。main.c の s_ssh_arg_* へ写されるので、
 * ここは受け渡しの間だけ持てばよい。スタックに積まない理由は上と同じ。 */
static char s_ssh_user_arg[48];
static char s_ssh_host_arg[80];

static int cmd_ssh(int argc, char **argv, void *user)
{
    app_ctx_t *ctx = ctx_of(user);
    cdk_shell_t *sh = ctx->sh;

    if (argc >= 2 && strcmp(argv[1], "close") == 0) {
        ctx->ssh_close();
        cdk_shell_write(sh, "切断しました\r\n");
        return 0;
    }

    /* その場で打ってもらったパスワードを捨てる。**人に貸す前に押せる口。**
     * NVS の `ssh.pass` は消さない（消すなら `set ssh.pass ""`）。 */
    if (argc >= 2 && strcmp(argv[1], "forget") == 0) {
        ctx->ssh_forget();
        cdk_shell_write(sh, "打ってもらったパスワードを忘れました\r\n");
        return 0;
    }

    /* 鍵の状態を見る。パスは cdk_ssh_key_path() が唯一の出どころなので、
     * ここで見る場所と実際に認証で使う場所は必ず一致する。 */
    if (argc >= 2 && strcmp(argv[1], "key") == 0) {
        const char *path = cdk_ssh_key_path();
        const esp_err_t err = cdk_sshkey_info(path, s_key_line, sizeof(s_key_line), s_key_fp, sizeof(s_key_fp));
        if (err == ESP_ERR_NOT_FOUND) {
            cdk_shell_printf(sh, "%s はありません。`ssh keygen` で作れます\r\n", path);
            return 1;
        }
        if (err != ESP_OK) {
            cdk_shell_printf(sh,
                             "\033[31m%s を鍵として読めません\033[0m\r\n"
                             "  PEM 形式の RSA か ECDSA のみ。"
                             "\033[33med25519 は使えません\033[0m\r\n",
                             path);
            return 1;
        }
        cdk_shell_printf(sh, "鍵    : %s\r\n指紋  : %s\r\n%s\r\n", path, s_key_fp, s_key_line);
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "keygen") == 0) {
        const char *path = cdk_ssh_key_path();

        /* 上書きは黙ってやらない。鍵を消すと入れなくなる。 */
        if (cdk_sshkey_info(path, s_key_line, sizeof(s_key_line), s_key_fp, sizeof(s_key_fp)) == ESP_OK &&
            !(argc >= 3 && strcmp(argv[2], "-f") == 0)) {
            cdk_shell_printf(sh,
                             "\033[33m%s は既にあります\033[0m\r\n"
                             "  上書きするなら ssh keygen -f\r\n",
                             path);
            return 1;
        }

        cdk_shell_write(sh, "ECDSA P-256 の鍵を作っています...\r\n");
        if (cdk_sshkey_generate(path, "cyberdeck", s_key_line,
                                sizeof(s_key_line), s_key_fp, sizeof(s_key_fp)) != ESP_OK) {
            cdk_shell_write(sh, "\033[31m鍵を作れませんでした（SD は挿さっていますか）\033[0m\r\n");
            return 1;
        }
        cdk_shell_printf(sh,
                         "鍵    : %s\r\n指紋  : %s\r\n\r\n"
                         "下の 1 行を接続先の ~/.ssh/authorized_keys に貼ってください:\r\n"
                         "\033[36m%s\033[0m\r\n",
                         path, s_key_fp, s_key_line);
        /* FAT には権限が無い。カードを抜けば誰でも読める。 */
        cdk_shell_write(sh,
                        "\033[33m注意: SD に権限は無いので、カードを抜けば秘密鍵は読めます\033[0m\r\n");
        return 0;
    }

    /* Linux と同じ `ssh user@host`。**NVS には書かない**（その場かぎり）。
     * 覚えさせたいなら `set ssh.host` / `set ssh.user`。
     *
     * ここまで来た argv[1] は close/key/keygen ではない。**黙って
     * 無視しない。** 引数が意味を持つようになったので、`ssh myhost` が
     * 何も言わず NVS の相手へ繋ぐのは以前より悪い。 */
    const char *at_user = NULL;
    const char *at_host = NULL;
    if (argc >= 2) {
        if (argc > 2) {
            cdk_shell_write(sh, "使い方: ssh [user@host|close|forget|key|keygen]\r\n");
            return 1;
        }
        if (!cdk_shell_split_userhost(argv[1], s_ssh_user_arg,
                                      sizeof(s_ssh_user_arg), s_ssh_host_arg,
                                      sizeof(s_ssh_host_arg))) {
            cdk_shell_printf(sh, "\033[31m%s を user@host として読めません\033[0m\r\n",
                             argv[1]);
            cdk_shell_write(sh, "使い方: ssh [user@host|close|forget|key|keygen]\r\n");
            return 1;
        }
        at_user = s_ssh_user_arg;
        at_host = s_ssh_host_arg;
    }

    if (!cdk_wifi_is_connected()) {
        /* **「待て」と「自分で繋げ」を分ける。** 起動が WiFi を待たなく
         * なったので、IP が来る前に `ssh` と打つのが普通の順序になった。
         * 繋ぎに行っている最中なら待てば帯が IP になるが、`wifi off` した
         * あとや諦めたあとは帯が変わらない。待てと言うと永久に待つ
         * （帯の `WiFi--` は「自分では戻らない」の意味。DESIGN.md §8.1）。 */
        if (cdk_wifi_poll() == CDK_WIFI_CONNECTING) {
            cdk_shell_write(sh, "\033[33mWiFi 接続中です。待ってから\033[0m\r\n");
        } else {
            cdk_shell_write(sh,
                            "\033[31mWiFi が繋がっていません (wifi connect)"
                            "\033[0m\r\n");
        }
        return 1;
    }
    if (!ctx->ssh_open(at_user, at_host)) {
        return 1;
    }
    return 0;
}

/* `scp` の分解先。`argv` はコマンドが return した時点で無効なので、
 * 渡す間だけここに持つ（`s_ssh_user_arg` と同じ作法）。 */
static cdk_scp_args_t s_scp;
static char           s_scp_remote[200];

/* エラーコードを 1 行にする。**文言をコマンド側に散らさない。** */
static const char *scp_reason(cdk_scp_parse_t e)
{
    switch (e) {
    case CDK_SCP_E_BOTH_LOCAL:
        return "どちらかを user@host:パス にしてください";
    case CDK_SCP_E_BOTH_REMOTE:
        return "相手から相手へは送れません";
    case CDK_SCP_E_NO_USER:
        return "user@host:パス の形で書いてください";
    case CDK_SCP_E_USERHOST:
        return "user@host として読めません";
    case CDK_SCP_E_NO_REMOTE:
        return "取ってくるパスを書いてください";
    case CDK_SCP_E_NO_LOCAL:
        return "手元のパスが空です";
    case CDK_SCP_E_TOOLONG:
        return "パスが長すぎます";
    default:
        return "引数が読めません";
    }
}

/* 手元のパスの末尾の名前。`user@host:` とだけ書かれたときに使う。 */
static const char *basename_of(const char *p)
{
    const char *slash = strrchr(p, '/');
    return (slash != NULL) ? slash + 1 : p;
}

static int cmd_scp(int argc, char **argv, void *user)
{
    app_ctx_t   *ctx = ctx_of(user);
    cdk_shell_t *sh = ctx->sh;

    if (argc != 3) {
        cdk_shell_write(sh, "使い方: scp <手元> user@host:<パス>   送る\r\n");
        cdk_shell_write(sh, "        scp user@host:<パス> <手元>   取る\r\n");
        /* **1 本だけ。** 黙って無視するより、無いと言う。 */
        cdk_shell_write(sh, "ファイルは 1 本だけ。-r はありません\r\n");
        return 1;
    }

    const cdk_scp_parse_t e = cdk_shell_scp_parse(argv[1], argv[2], &s_scp);
    if (e != CDK_SCP_OK) {
        cdk_shell_printf(sh, "\033[31m%s\033[0m\r\n", scp_reason(e));
        cdk_shell_write(sh, "使い方: scp <手元> user@host:<パス> / 逆も可\r\n");
        return 1;
    }

    if (!cdk_sd_mounted()) {
        cdk_shell_write(sh,
                        "\033[31mSD がマウントされていません (sd mount)\033[0m\r\n");
        return 1;
    }
    if (!cdk_wifi_is_connected()) {
        /* `ssh` と同じ 2 通り分け。「待て」と「自分で繋げ」を混ぜない。 */
        if (cdk_wifi_poll() == CDK_WIFI_CONNECTING) {
            cdk_shell_write(sh, "\033[33mWiFi 接続中です。待ってから\033[0m\r\n");
        } else {
            cdk_shell_write(
                sh, "\033[31mWiFi が繋がっていません (wifi connect)\033[0m\r\n");
        }
        return 1;
    }
    if (cdk_ssh_is_open()) {
        /* セッションは 1 本しかない。開いたまま始めると壊し合う。 */
        cdk_shell_write(sh,
                        "\033[31mSSH を先に閉じてください (ssh close)\033[0m\r\n");
        return 1;
    }

    /* `user@host:` とだけ書かれたら、手元の名前をそのまま使う。
     * **空のまま渡してはいけない。** libssh2 は最後の `/` から名前を切るので、
     * 空だと名前無しの要求になって相手が断る。 */
    const char *remote = s_scp.remote;
    if (s_scp.remote_is_empty) {
        snprintf(s_scp_remote, sizeof(s_scp_remote), "%s",
                 basename_of(s_scp.local));
        remote = s_scp_remote;
    }

    return ctx->scp_start(s_scp.upload, s_scp.local, remote, s_scp.user,
                          s_scp.host)
               ? 0
               : 1;
}

/* ホスト鍵の状態。TOFU で覚えた直後は指紋を確認できるようにしておく。 */
static int cmd_hostkey(int argc, char **argv, void *user)
{
    cdk_shell_t *sh = ctx_of(user)->sh;

    if (argc >= 2 && strcmp(argv[1], "forget") == 0) {
        switch (cdk_knownhosts_forget()) {
        case CDK_KNOWNHOSTS_FORGET_OK:
            cdk_shell_printf(sh, "%s を消しました。次の接続は TOFU になります\r\n",
                             cdk_knownhosts_path());
            return 0;
        case CDK_KNOWNHOSTS_FORGET_ABSENT:
            cdk_shell_printf(sh, "%s はもともとありません\r\n",
                             cdk_knownhosts_path());
            return 0;
        case CDK_KNOWNHOSTS_FORGET_NO_SD:
            cdk_shell_write(sh, "\033[31mSD がマウントされていません\033[0m\r\n");
            return 1;
        default:
            cdk_shell_printf(sh, "\033[31m%s を消せません\033[0m\r\n",
                             cdk_knownhosts_path());
            return 1;
        }
    }
    if (argc >= 2) {
        cdk_shell_write(sh, "使い方: hostkey [forget]\r\n");
        return 1;
    }

    static const char *const NAMES[] = {
        "\033[32m記録と一致\033[0m",
        "\033[33m初めて見たので覚えた (TOFU)\033[0m",
        "\033[31m記録と不一致\033[0m",
        "\033[33m検証できず (SD 無し等)\033[0m",
        "\033[31mエラー\033[0m",
    };
    const int st = cdk_ssh_hostkey_status();
    const char *fp = cdk_ssh_fingerprint();

    cdk_shell_printf(sh, "指紋    : %s\r\n", (fp[0] != '\0') ? fp : "(未接続)");
    cdk_shell_printf(sh, "検証    : %s\r\n", (st >= 0 && st < 5) ? NAMES[st] : "?");
    cdk_shell_printf(sh, "記録先  : %s\r\n", cdk_knownhosts_path());
    return 0;
}

/* ------------------------------------------------------------------ */
/* 周辺                                                                */
/* ------------------------------------------------------------------ */

static int cmd_gpio(int argc, char **argv, void *user)
{
    cdk_shell_t *sh = ctx_of(user)->sh;

    if (argc < 3) {
        cdk_shell_write(sh, "使い方: gpio mode <pin> <in|in_pu|in_pd|out>\r\n"
                            "        gpio set  <pin> <0|1>\r\n"
                            "        gpio get  <pin>\r\n");
        return 1;
    }
    const int pin = atoi(argv[2]);

    const char *why = NULL;
    if (cdk_gpio_is_reserved(pin, &why)) {
        cdk_shell_printf(sh, "\033[31mG%d は %s に使われています\033[0m\r\n", pin, why);
        return 1;
    }

    if (strcmp(argv[1], "get") == 0) {
        int level = 0;
        if (cdk_gpio_get(pin, &level) != ESP_OK) {
            cdk_shell_printf(sh, "\033[31mG%d を読めません\033[0m\r\n", pin);
            return 1;
        }
        cdk_shell_printf(sh, "G%d = %d\r\n", pin, level);
        return 0;
    }
    if (argc < 4) {
        cdk_shell_write(sh, "引数が足りません\r\n");
        return 1;
    }
    if (strcmp(argv[1], "mode") == 0) {
        if (cdk_gpio_mode(pin, argv[3]) != ESP_OK) {
            cdk_shell_printf(sh, "\033[31mモード '%s' が不正です\033[0m\r\n", argv[3]);
            return 1;
        }
        cdk_shell_printf(sh, "G%d を %s にしました\r\n", pin, argv[3]);
        return 0;
    }
    if (strcmp(argv[1], "set") == 0) {
        const int level = atoi(argv[3]);
        if (cdk_gpio_set(pin, level) != ESP_OK) {
            cdk_shell_printf(sh, "\033[31mG%d に書けません\033[0m\r\n", pin);
            return 1;
        }
        cdk_shell_printf(sh, "G%d = %d\r\n", pin, level ? 1 : 0);
        return 0;
    }
    cdk_shell_write(sh, "mode / set / get のどれかです\r\n");
    return 1;
}

static int cmd_adc(int argc, char **argv, void *user)
{
    cdk_shell_t *sh = ctx_of(user)->sh;

    if (argc < 3 || strcmp(argv[1], "read") != 0) {
        cdk_shell_write(sh, "使い方: adc read <ch>   （ADC1 の 0-9）\r\n");
        return 1;
    }
    const int ch = atoi(argv[2]);
    int raw = 0, mv = -1;
    const esp_err_t err = cdk_adc_read(ch, &raw, &mv);
    if (err != ESP_OK) {
        cdk_shell_printf(sh, "\033[31m読めません: %s\033[0m\r\n", esp_err_to_name(err));
        return 1;
    }
    if (mv >= 0) {
        cdk_shell_printf(sh, "ADC1 ch%d: raw=%d  %d mV\r\n", ch, raw, mv);
    } else {
        cdk_shell_printf(sh, "ADC1 ch%d: raw=%d  (校正なし)\r\n", ch, raw);
    }
    return 0;
}

static int cmd_i2c(int argc, char **argv, void *user)
{
    cdk_shell_t *sh = ctx_of(user)->sh;

    if (argc < 2 || strcmp(argv[1], "scan") != 0) {
        cdk_shell_write(sh, "使い方: i2c scan <sda> <scl>\r\n"
                            "  内蔵バス (G12/G11) は BSP が使用中なので走査できません。\r\n"
                            "  Grove ポートに挿したものを見てください。\r\n");
        return 1;
    }
    if (argc < 4) {
        cdk_shell_write(sh, "sda と scl のピン番号が要ります\r\n");
        return 1;
    }
    const int sda = atoi(argv[2]);
    const int scl = atoi(argv[3]);

    cdk_shell_printf(sh, "SDA=G%d SCL=G%d を走査中...\r\n", sda, scl);
    int found = 0;
    const esp_err_t err = cdk_i2c_scan(sda, scl, 100000, periph_out, sh, &found);
    if (err == ESP_ERR_INVALID_STATE) {
        cdk_shell_write(sh,
                        "\033[31mG11/G12 は内蔵 I2C で、BSP が使用中です\033[0m\r\n"
                        "外部ポートのピンを指定してください\r\n");
        return 1;
    }
    if (err != ESP_OK) {
        cdk_shell_printf(sh, "\033[31m走査できません: %s\033[0m\r\n",
                         esp_err_to_name(err));
        return 1;
    }
    cdk_shell_printf(sh, "%d 個見つかりました\r\n", found);
    return 0;
}

/* ------------------------------------------------------------------ */

const cdk_shell_cmd_t APP_COMMANDS[] = {
    {"help",    "このコマンド一覧",                       cmd_help},
    {"clear",   "画面を消す",                             cmd_clear},
    {"echo",    "引数をそのまま出す",                     cmd_echo},
    {"history", "コマンド履歴",                           cmd_history},
    {"info",    "機器と接続の状態",                       cmd_info},
    {"free",    "ヒープの空き",                           cmd_free},
    {"set",     "設定の表示と変更 (set <key> <value>)",   cmd_set},
    {"wifi",    "WiFi (wifi connect|off)",                cmd_wifi},
    {"ssh",     "SSH (user@host|close|forget|key|keygen)",       cmd_ssh},
    {"scp",     "ファイル転送 (scp <手元> user@host:<パス>)", cmd_scp},
    {"gpio",    "GPIO の mode / set / get",               cmd_gpio},
    {"adc",     "ADC1 を読む (adc read <ch>)",            cmd_adc},
    {"i2c",     "I2C を走査 (i2c scan <sda> <scl>)",      cmd_i2c},
    {"hostkey", "SSH ホスト鍵の検証状態 (hostkey forget)", cmd_hostkey},
    {"reboot",  "再起動",                                 cmd_reboot},
};

const size_t APP_COMMANDS_COUNT = sizeof(APP_COMMANDS) / sizeof(APP_COMMANDS[0]);
