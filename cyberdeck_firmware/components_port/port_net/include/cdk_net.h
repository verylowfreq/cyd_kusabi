/* cdk_net — WiFi と SSH クライアント
 *
 * SSH は libssh2 1.11.1 + mbedTLS。選定の経緯は DESIGN.md §8.2、
 * ベンダリングの注意は components_port/libssh2/README.md。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- WiFi ---------------- */

/* NVS / netif / event loop / WiFi ドライバを初期化する。接続はまだしない。 */
esp_err_t cdk_wifi_init(void);

/* STA として接続し、IP が取れるまで待つ。timeout_ms で諦める。
 * 何度呼んでもよい（再接続に使える）。 */
esp_err_t cdk_wifi_connect(const char *ssid, const char *psk, uint32_t timeout_ms);

bool cdk_wifi_is_connected(void);

/* "192.168.0.12" 形式。未接続なら "-" を入れる。 */
void cdk_wifi_ip(char *out, size_t cap);

/* 直近の切断理由コード（esp_wifi の wifi_err_reason_t）。0 なら未発生。 */
int cdk_wifi_last_disconnect_reason(void);

/* ---------------- SSH ---------------- */

typedef struct {
    const char *host;
    uint16_t    port;      /* 0 なら 22 */
    const char *user;
    const char *password;
    const char *term;      /* NULL なら "xterm-256color" */
    int         cols;      /* cdk_term の実値を渡すこと。決め打ち禁止。 */
    int         rows;

    /* 秘密鍵のパス。NULL か "" なら公開鍵認証を試さない。
     *
     * **同じ場所に `.pub` も要る**（ECDSA の場合）。mbedTLS バックエンドで
     * 秘密鍵から公開鍵を導出する経路は RSA しか受け付けないため。
     *
     * **ed25519 は使えない。** mbedTLS バックエンドの libssh2 は
     * `LIBSSH2_ED25519 = 0`。使えるのは RSA と ECDSA だけ。
     * `ssh-keygen` の既定が ed25519 なので、ここは必ず引っかかる。 */
    const char *key_path;
    const char *key_pass; /* 秘密鍵のパスフレーズ。NULL 可 */
} cdk_ssh_config_t;

/* 接続してシェルを開く。PTY まで張ってから返る。 */
esp_err_t cdk_ssh_connect(const cdk_ssh_config_t *cfg);

bool cdk_ssh_is_open(void);

/* 直近に接続したホストの鍵指紋 (SHA1)。未接続なら空文字列。 */
const char *cdk_ssh_fingerprint(void);

/* known_hosts の判定結果 (cdk_hostkey_result_t)。 */
int cdk_ssh_hostkey_status(void);

/* 受信。0 = いま来ていない。負 = エラーまたは切断。 */
int cdk_ssh_read(uint8_t *buf, size_t cap);

/* 送信。全部送るまで粘る。負 = エラー。 */
int cdk_ssh_write(const uint8_t *buf, size_t len);

/* 画面サイズ変更をリモートへ通知する (window-change)。 */
esp_err_t cdk_ssh_resize(int cols, int rows);

void cdk_ssh_close(void);

/* 切れた理由。**自動再接続してよいかの判断に使う。**
 * リモートで `exit` を打ったのに繋ぎ直されると、抜けられなくなる。 */
typedef enum {
    CDK_SSH_DISCONNECT_NONE = 0,
    CDK_SSH_DISCONNECT_EOF,   /* リモートが閉じた（exit）。繋ぎ直さない */
    CDK_SSH_DISCONNECT_ERROR, /* 通信が切れた。繋ぎ直してよい */
    CDK_SSH_DISCONNECT_LOCAL  /* こちらから閉じた。繋ぎ直さない */
} cdk_ssh_disconnect_t;

cdk_ssh_disconnect_t cdk_ssh_disconnect_reason(void);

/* ---------------- SSH の鍵 ---------------- */

/* 実機で ECDSA P-256 の鍵を作り、PEM を path に、公開鍵 1 行を
 * path + ".pub" に書く。line には公開鍵の 1 行、fp には SHA256 の指紋。
 *
 * **ed25519 は作らない。** mbedTLS バックエンドの libssh2 が対応して
 * いないので、作っても認証に使えない。 */
esp_err_t cdk_sshkey_generate(const char *path, const char *comment, char *line,
                              size_t line_cap, char *fp, size_t fp_cap);

/* 既にある秘密鍵から公開鍵の 1 行と指紋を出す。
 * 鍵が無ければ ESP_ERR_NOT_FOUND、読めなければ ESP_ERR_INVALID_ARG。 */
esp_err_t cdk_sshkey_info(const char *path, char *line, size_t line_cap, char *fp,
                          size_t fp_cap);

/* ---------------- 診断 ---------------- */

/* libssh2 がリンクされ、実機で初期化できることを確かめる。 */
esp_err_t cdk_net_probe(void);

#ifdef __cplusplus
}
#endif
