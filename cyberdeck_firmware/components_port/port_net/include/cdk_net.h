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

typedef enum {
    CDK_WIFI_IDLE = 0,   /* まだ繋ぎに行っていない */
    CDK_WIFI_CONNECTING, /* 繋ぎに行っている最中 */
    CDK_WIFI_CONNECTED,  /* IP まで取れた */
    CDK_WIFI_FAILED      /* 諦めた。理由は cdk_wifi_last_disconnect_reason() */
} cdk_wifi_state_t;

/* 接続を始めて**すぐ返る**。待たない。
 *
 * **待つ版は置かない。** 起動時に IP が取れるまで待つと、AP が居ない場所で
 * 20 秒画面が止まる。その間キーも効かないので、電源を切る以外に手が無い。
 * 結果は cdk_wifi_poll() で受け取り、途中の状態はステータス帯に出す。
 *
 * timeout_ms を過ぎても IP が取れなければ CDK_WIFI_FAILED になる。
 * ただし**一度でも IP が取れたあとに切れた場合は諦めない**。期限を持たず、
 * 電波が戻るまで繋ぎ直し続ける（帯は「WiFi接続中」のまま）。
 *
 * 何度呼んでもよいが、CONNECTING の間に呼び直すと今の試行を捨てる。 */
esp_err_t cdk_wifi_connect_start(const char *ssid, const char *psk,
                                 uint32_t timeout_ms);

/* いまの状態。**呼ぶたびに時間切れを見る**ので、繋ぎに行っている間は
 * メインループから定期的に呼ぶこと。呼ばないと CONNECTING のまま止まる。 */
cdk_wifi_state_t cdk_wifi_poll(void);

/* 繋ぎ直すのをやめて、**電波も止める** (esp_wifi_stop)。状態は IDLE に戻る。
 *
 * 一度でも繋がったあとに切れた場合、この機械は期限を持たずに繋ぎ直し続ける
 * （電波が戻れば勝手に戻ってほしいので）。だが**家で点けたまま外へ出た**ときは
 * 相手が二度と現れないのに探し続けることになり、電池だけが減る。
 * それを人が止めるための口。
 *
 * **この場かぎり。** NVS には何も書かないので、次に電源を入れれば普通に
 * 繋ぎに行く。戻すのは `cdk_wifi_connect_start()`（シェルでは `wifi connect`）。 */
void cdk_wifi_off(void);

bool cdk_wifi_is_connected(void);

/* "192.168.0.12" 形式。未接続なら "-" を入れる。 */
void cdk_wifi_ip(char *out, size_t cap);

/* 直近の切断理由コード（esp_wifi の wifi_err_reason_t）。0 なら未発生。 */
int cdk_wifi_last_disconnect_reason(void);

/* いまその SSID に繋がっているか。
 * `wifi connect` を繰り返し叩いても無駄に繋ぎ直さないために使う。 */
bool cdk_wifi_ssid_is(const char *ssid);

/* ---------------- SSH ---------------- */

/* TCP を張るまでの期限 (ms)。
 *
 * **`libssh2_session_set_timeout()` では防げない。** あれは libssh2 自身の
 * 呼び出しにしか効かず、その手前の TCP には届かない。
 * `CONFIG_LWIP_TCP_SYNMAXRTX=12` なので、切らないと居ない IP で分単位。 */
#define CDK_SSH_CONNECT_TIMEOUT_MS 8000

/* ブロッキングで回している間の 1 呼び出しの期限 (ms)。
 * 実測のハンドシェイクが約 1.9 秒なので、健全な相手では踏まない。 */
#define CDK_SSH_CALL_TIMEOUT_MS 10000

/* 繋いで認証するのに要るもの。**PTY のサイズは含めない。**
 * scp は端末を開かないので、意味のない cols/rows を渡させない。 */
typedef struct {
    const char *host;
    uint16_t    port;     /* 0 なら 22 */
    const char *user;
    const char *password; /* **NULL 禁止。** 無いときは ""。
                           * libssh2_userauth_password が strlen() に
                           * 素通しするので NULL だと落ちる。 */

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
} cdk_ssh_login_t;

typedef struct {
    cdk_ssh_login_t login;
    const char     *term; /* NULL なら "xterm-256color" */
    int             cols; /* cdk_term の実値を渡すこと。決め打ち禁止。 */
    int             rows;
} cdk_ssh_config_t;

/* 接続してシェルを開く。PTY まで張ってから返る。 */
esp_err_t cdk_ssh_connect(const cdk_ssh_config_t *cfg);

/* ---- チャネルを開かない接続（scp 用） ----
 *
 * TCP を張り、ハンドシェイクし、known_hosts と突き合わせ、認証するまで。
 * **チャネルは開かない。** シェルが要るなら続けて `cdk_ssh_shell_open()`、
 * ファイル転送なら `cdk_scp_*`。
 *
 * **認証の塊を 2 つ持たないためにここで切ってある。** 空パスワードで
 * password 認証を叩かない判断、`.pub` を必ず渡す理由、ホスト鍵が
 * 食い違ったら断る判断は、どれも実機で踏んだ末のもの（DESIGN.md §8.2）。
 * 写すと片方が腐る。
 *
 * すでに開いているセッションがあれば `ESP_ERR_INVALID_STATE`。
 * **黙って閉じない** — シェルの下からセッションを抜くことになる。
 *
 * 戻り値: `ESP_ERR_NOT_ALLOWED` = 認証失敗、
 * `ESP_ERR_INVALID_STATE` = ホスト鍵の不一致か二重呼び出し。 */
esp_err_t cdk_ssh_session_open(const cdk_ssh_login_t *login);

/* PTY 付きのシェルを開き、ノンブロッキングへ切り替える。
 * `cdk_ssh_session_open()` が成功したあとだけ呼べる。 */
esp_err_t cdk_ssh_shell_open(const char *term, int cols, int rows);

/* セッションとソケットだけ閉じる。**`s_open` と切断理由を触らない。**
 * シェルを持たない経路（scp）の後片付け専用。 */
void cdk_ssh_session_close(void);

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
