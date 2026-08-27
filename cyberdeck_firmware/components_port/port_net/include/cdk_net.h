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
} cdk_ssh_config_t;

/* 接続してシェルを開く。PTY まで張ってから返る。 */
esp_err_t cdk_ssh_connect(const cdk_ssh_config_t *cfg);

bool cdk_ssh_is_open(void);

/* 直近に接続したホストの鍵指紋 (SHA1)。未接続なら空文字列。 */
const char *cdk_ssh_fingerprint(void);

/* 受信。0 = いま来ていない。負 = エラーまたは切断。 */
int cdk_ssh_read(uint8_t *buf, size_t cap);

/* 送信。全部送るまで粘る。負 = エラー。 */
int cdk_ssh_write(const uint8_t *buf, size_t len);

/* 画面サイズ変更をリモートへ通知する (window-change)。 */
esp_err_t cdk_ssh_resize(int cols, int rows);

void cdk_ssh_close(void);

/* ---------------- 診断 ---------------- */

/* libssh2 がリンクされ、実機で初期化できることを確かめる。 */
esp_err_t cdk_net_probe(void);

#ifdef __cplusplus
}
#endif
