/* port_ssh — libssh2 による対話 SSH クライアント
 *
 * 目標は「PTY 付きのシェルを開いて CLI をそのまま動かす」こと。
 * DESIGN.md §8.2 の通り、PTY が取れることが libssh2 を選んだ理由なので、
 * 端末名と桁数・行数は必ず呼び出し側（cdk_term の実値）から渡す。
 *
 * ハンドシェイクと認証はブロッキングで済ませ、そのあと
 * ノンブロッキングに切り替えてメインループから読み書きする。
 */
#include "cdk_net.h"

#include "cdk_knownhosts.h"

#include "esp_log.h"

#include <stdio.h>
#include "libssh2.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static const char *TAG = "cdk_ssh";

static int                s_sock = -1;
static LIBSSH2_SESSION   *s_session;
static LIBSSH2_CHANNEL   *s_channel;
static bool               s_open;
static bool               s_lib_inited;
static char               s_fingerprint[64];
static cdk_hostkey_result_t s_hostkey_result = CDK_HOSTKEY_UNVERIFIED;
static cdk_ssh_disconnect_t s_disconnect = CDK_SSH_DISCONNECT_NONE;

static void log_fingerprint(void)
{
    const char *fp = libssh2_hostkey_hash(s_session, LIBSSH2_HOSTKEY_HASH_SHA1);
    if (fp == NULL) {
        s_fingerprint[0] = '\0';
        return;
    }
    /* SHA1 は 20 バイト */
    int n = 0;
    for (int i = 0; i < 20 && n < (int)sizeof(s_fingerprint) - 4; i++) {
        n += snprintf(s_fingerprint + n, sizeof(s_fingerprint) - (size_t)n, "%02X%s",
                      (unsigned char)fp[i], (i < 19) ? ":" : "");
    }
    ESP_LOGI(TAG, "ホスト鍵 SHA1: %s", s_fingerprint);
}

/* keyboard-interactive のコールバック。
 * sshd の設定によっては password 認証を出さず keyboard-interactive しか
 * 受け付けない。中身は同じパスワードなので、全プロンプトにそれを返す。 */
static const char *s_kbd_password;

static void kbd_callback(const char *name, int name_len, const char *instruction,
                         int instruction_len, int num_prompts,
                         const LIBSSH2_USERAUTH_KBDINT_PROMPT *prompts,
                         LIBSSH2_USERAUTH_KBDINT_RESPONSE *responses, void **abstract)
{
    (void)name;
    (void)name_len;
    (void)instruction;
    (void)instruction_len;
    (void)prompts;
    (void)abstract;

    if (s_kbd_password == NULL) {
        return;
    }
    const size_t len = strlen(s_kbd_password);
    for (int i = 0; i < num_prompts; i++) {
        responses[i].text = strdup(s_kbd_password);
        responses[i].length = (unsigned int)len;
    }
}

static int connect_socket(const char *host, uint16_t port)
{
    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);

    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    const int gai = getaddrinfo(host, portstr, &hints, &res);
    if (gai != 0 || res == NULL) {
        ESP_LOGE(TAG, "名前解決に失敗しました: %s (gai=%d)", host, gai);
        return -1;
    }

    const int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() に失敗 errno=%d", errno);
        freeaddrinfo(res);
        return -1;
    }
    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(TAG, "connect() に失敗 errno=%d", errno);
        close(sock);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);

    /* Nagle を切る。対話端末では 1 文字ずつ送るので遅延が体感に直結する。 */
    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return sock;
}

void cdk_ssh_close(void)
{
    if (s_channel != NULL) {
        libssh2_channel_free(s_channel);
        s_channel = NULL;
    }
    if (s_session != NULL) {
        libssh2_session_disconnect(s_session, "bye");
        libssh2_session_free(s_session);
        s_session = NULL;
    }
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    /* **こちらから閉じたときは繋ぎ直さない。** 通信断で閉じた場合は
     * すでに read/write 側が理由を入れているので、そちらを消さない。 */
    if (s_open) {
        s_disconnect = CDK_SSH_DISCONNECT_LOCAL;
    }
    s_open = false;
}

esp_err_t cdk_ssh_connect(const cdk_ssh_config_t *cfg)
{
    if (cfg == NULL || cfg->host == NULL || cfg->user == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_open) {
        cdk_ssh_close();
    }

    if (!s_lib_inited) {
        if (libssh2_init(0) != 0) {
            ESP_LOGE(TAG, "libssh2_init に失敗");
            return ESP_FAIL;
        }
        s_lib_inited = true;
    }

    s_sock = connect_socket(cfg->host, cfg->port ? cfg->port : 22);
    if (s_sock < 0) {
        return ESP_FAIL;
    }

    s_session = libssh2_session_init();
    if (s_session == NULL) {
        ESP_LOGE(TAG, "session_init に失敗");
        cdk_ssh_close();
        return ESP_FAIL;
    }

    /* ハンドシェイクと認証はブロッキングで通す。状態機械を書くほどの回数ではない。 */
    libssh2_session_set_blocking(s_session, 1);

    int rc = libssh2_session_handshake(s_session, s_sock);
    if (rc != 0) {
        ESP_LOGE(TAG, "ハンドシェイクに失敗 rc=%d", rc);
        cdk_ssh_close();
        return ESP_FAIL;
    }
    log_fingerprint();

    /* known_hosts と突き合わせる。食い違ったら必ず断る。 */
    const cdk_hostkey_result_t hk =
        cdk_knownhosts_check(s_session, cfg->host, cfg->port ? cfg->port : 22);
    s_hostkey_result = hk;
    if (hk == CDK_HOSTKEY_MISMATCH) {
        ESP_LOGE(TAG, "ホスト鍵が %s の記録と一致しません。接続を中止します",
                 cdk_knownhosts_path());
        cdk_ssh_close();
        return ESP_ERR_INVALID_STATE;
    }

    /* サーバーが受け付ける方式を見てから選ぶ。
     * ここを出しておかないと「パスワードが違う」のか
     * 「その方式を出していない」のか切り分けられない。 */
    const char *methods = libssh2_userauth_list(s_session, cfg->user,
                                                (unsigned int)strlen(cfg->user));
    ESP_LOGI(TAG, "サーバーが提示する認証方式: %s", (methods != NULL) ? methods : "(取得失敗)");

    rc = LIBSSH2_ERROR_AUTHENTICATION_FAILED;

    /* **パスワードが無いなら叩かない。**
     *
     * sshd の試行回数 (MaxAuthTries) を 1 つ無駄にするし、何より失敗の
     * 理由が嘘になる。鍵だけで入ろうとして駄目だったときに「password
     * 認証に失敗」と出ると、パスワードの問題だと思って見当違いの
     * ところを直しに行く（`ssh user@host` は鍵だけで行くので、
     * この経路は普通に起きる）。 */
    const bool have_pass = (cfg->password != NULL && cfg->password[0] != '\0');

    /* **公開鍵を先に試す。** パスワードより先にするのは、鍵を置いたのに
     * 黙ってパスワードで入っていた、という状態を作らないため。
     *
     * **`.pub` を必ず渡す。** mbedTLS バックエンドで秘密鍵から公開鍵を
     * 導出する経路 (`_libssh2_mbedtls_pub_priv_key`) は **RSA しか
     * 受け付けない**（`MBEDTLS_PK_RSA` 以外は "Key type not supported"）。
     * 一方 ECDSA の署名と秘密鍵の読み込みは実装されているので、
     * 公開鍵を外から渡せば ECDSA でも通る。
     *
     * NULL を渡して ECDSA の鍵を使うと `rc=-16` で落ちる。これは
     * 「鍵が未登録」と区別がつかないので、原因の分からない失敗になる
     * （実機で踏んだ）。 */
    if (cfg->key_path != NULL && cfg->key_path[0] != '\0' && methods != NULL &&
        strstr(methods, "publickey") != NULL) {
        char pubpath[160];
        snprintf(pubpath, sizeof(pubpath), "%s.pub", cfg->key_path);

        /* `.pub` が無ければ NULL で導出に賭ける（RSA ならそれで通る）。 */
        const char *pub = NULL;
        FILE       *pf = fopen(pubpath, "rb");
        if (pf != NULL) {
            fclose(pf);
            pub = pubpath;
        } else {
            ESP_LOGW(TAG, "%s がありません。RSA 以外はこれが無いと通りません",
                     pubpath);
        }

        rc = libssh2_userauth_publickey_fromfile_ex(
            s_session, cfg->user, (unsigned int)strlen(cfg->user), pub,
            cfg->key_path, cfg->key_pass);
        if (rc == 0) {
            ESP_LOGI(TAG, "公開鍵で認証しました (%s)", cfg->key_path);
        } else {
            /* **ここで諦めない。** 鍵が authorized_keys に入っていないのは
             * 普通に起きる。パスワードへ落ちる。 */
            char *msg = NULL;
            libssh2_session_last_error(s_session, &msg, NULL, 0);
            ESP_LOGW(TAG, "公開鍵認証に失敗 rc=%d (%s)。%s", rc,
                     (msg != NULL) ? msg : "?",
                     have_pass ? "パスワードを試します"
                               : "パスワードは無いのでここで終わりです");
        }
    }

    if (rc != 0 && have_pass && methods != NULL &&
        strstr(methods, "password") != NULL) {
        rc = libssh2_userauth_password(s_session, cfg->user, cfg->password);
        if (rc != 0) {
            ESP_LOGW(TAG, "password 認証に失敗 rc=%d", rc);
        }
    }
    if (rc != 0 && have_pass && methods != NULL &&
        strstr(methods, "keyboard-interactive") != NULL) {
        s_kbd_password = cfg->password;
        rc = libssh2_userauth_keyboard_interactive(s_session, cfg->user, kbd_callback);
        s_kbd_password = NULL;
        if (rc != 0) {
            ESP_LOGW(TAG, "keyboard-interactive 認証に失敗 rc=%d", rc);
        }
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "認証に失敗しました rc=%d（方式: %s）", rc,
                 (methods != NULL) ? methods : "?");
        cdk_ssh_close();
        return ESP_ERR_NOT_ALLOWED;
    }

    s_channel = libssh2_channel_open_session(s_session);
    if (s_channel == NULL) {
        ESP_LOGE(TAG, "チャネルを開けません rc=%d",
                 libssh2_session_last_errno(s_session));
        cdk_ssh_close();
        return ESP_FAIL;
    }

    /* ここが libssh2 を選んだ理由。端末名もサイズも引数で渡せる。
     * サイズは呼び出し側が cdk_term の実値を入れること。
     * 最終ハードの 80x26 を決め打ちすると試作機(53x20)で表示が崩れる。 */
    const char *term = (cfg->term != NULL) ? cfg->term : "xterm-256color";
    rc = libssh2_channel_request_pty_ex(s_channel, term, (unsigned int)strlen(term),
                                        NULL, 0, cfg->cols, cfg->rows, 0, 0);
    if (rc != 0) {
        ESP_LOGE(TAG, "PTY 要求に失敗 rc=%d", rc);
        cdk_ssh_close();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "PTY %s %dx%d", term, cfg->cols, cfg->rows);

    rc = libssh2_channel_shell(s_channel);
    if (rc != 0) {
        ESP_LOGE(TAG, "シェルの起動に失敗 rc=%d", rc);
        cdk_ssh_close();
        return ESP_FAIL;
    }

    /* ここから先はメインループが回すのでノンブロッキングにする */
    libssh2_session_set_blocking(s_session, 0);
    const int flags = fcntl(s_sock, F_GETFL, 0);
    fcntl(s_sock, F_SETFL, flags | O_NONBLOCK);

    s_open = true;
    s_disconnect = CDK_SSH_DISCONNECT_NONE;
    ESP_LOGI(TAG, "%s@%s に接続しました", cfg->user, cfg->host);
    return ESP_OK;
}

bool cdk_ssh_is_open(void)
{
    return s_open;
}

const char *cdk_ssh_fingerprint(void)
{
    return s_fingerprint;
}

int cdk_ssh_hostkey_status(void)
{
    return (int)s_hostkey_result;
}

int cdk_ssh_read(uint8_t *buf, size_t cap)
{
    if (!s_open || buf == NULL || cap == 0) {
        return -1;
    }
    const ssize_t n = libssh2_channel_read(s_channel, (char *)buf, cap);
    if (n == LIBSSH2_ERROR_EAGAIN) {
        return 0; /* いま来ていないだけ */
    }
    if (n < 0) {
        ESP_LOGW(TAG, "読み取りエラー rc=%d", (int)n);
        s_open = false;
        s_disconnect = CDK_SSH_DISCONNECT_ERROR;
        return -1;
    }
    if (n == 0 && libssh2_channel_eof(s_channel)) {
        /* リモートが正常に閉じた（`exit` を打った）。**繋ぎ直さない。**
         * 抜けたつもりが勝手に入り直されると、抜ける手段が無くなる。 */
        ESP_LOGI(TAG, "リモートがセッションを閉じました");
        s_open = false;
        s_disconnect = CDK_SSH_DISCONNECT_EOF;
        return -1;
    }
    return (int)n;
}

cdk_ssh_disconnect_t cdk_ssh_disconnect_reason(void)
{
    return s_disconnect;
}

int cdk_ssh_write(const uint8_t *buf, size_t len)
{
    if (!s_open || buf == NULL) {
        return -1;
    }
    size_t sent = 0;
    while (sent < len) {
        const ssize_t n = libssh2_channel_write(s_channel, (const char *)(buf + sent),
                                                len - sent);
        if (n == LIBSSH2_ERROR_EAGAIN) {
            /* 送りきれない。呼び出し側で再送させるより、ここで少し粘る方が
             * キー入力の取りこぼしが起きにくい。 */
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        if (n < 0) {
            ESP_LOGW(TAG, "書き込みエラー rc=%d", (int)n);
            s_open = false;
            return -1;
        }
        sent += (size_t)n;
    }
    return (int)sent;
}

esp_err_t cdk_ssh_resize(int cols, int rows)
{
    if (!s_open) {
        return ESP_ERR_INVALID_STATE;
    }
    const int rc = libssh2_channel_request_pty_size_ex(s_channel, cols, rows, 0, 0);
    if (rc != 0 && rc != LIBSSH2_ERROR_EAGAIN) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
