/* cdk_cfg — NVS に置く設定値
 *
 * WiFi や SSH の認証情報をファームウェアに焼かないための入れ物。
 * ビルド成果物に値が入っていると、バイナリを渡した相手に資格情報も渡ることになる。
 *
 * キー名は NVS の制限で 15 文字まで。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 設定で使うキー。増やすときは 15 文字を超えないこと。 */
#define CDK_CFG_WIFI_SSID "wifi.ssid"
#define CDK_CFG_WIFI_PSK  "wifi.psk"
#define CDK_CFG_SSH_HOST  "ssh.host"
#define CDK_CFG_SSH_USER  "ssh.user"
#define CDK_CFG_SSH_PASS  "ssh.pass"
/* パネルのガンマ補正。10 倍の整数を 10 進文字列で持つ（"18" なら 1.8）。
 * 未設定なら基板の既定値 CDK_BOARD_LCD_GAMMA_X10 を使う。 */
#define CDK_CFG_GAMMA     "gamma"
/* キー配列。"us" か "jis"（DESIGN.md 7.4）。未設定なら us。 */
#define CDK_CFG_KEYMAP    "keymap"
/* 文字の拡大率。"1" か "2"。展示など離れて見るときに 2。
 * **変えたら再起動が要る**（桁数と行数が変わるため）。 */
#define CDK_CFG_FONTSCALE "fontscale"
/* 最下段のステータス帯。"on" か "off"。本文が 1 行減る。
 * **変えたら再起動が要る**（fontscale と同じ理由）。 */
#define CDK_CFG_STATUSBAR "statusbar"
/* SSH の秘密鍵。未設定なら /sdcard/id_ecdsa を使う。
 * **ed25519 は使えない**（mbedTLS バックエンドの libssh2 が非対応）。 */
#define CDK_CFG_SSH_KEY   "ssh.key"

/* NVS を初期化する。何度呼んでもよい。 */
esp_err_t cdk_cfg_init(void);

/* 見つからなければ ESP_ERR_NVS_NOT_FOUND。out は必ず終端される。 */
esp_err_t cdk_cfg_get(const char *key, char *out, size_t cap);

/* value が NULL または空文字列ならキーを消す。 */
esp_err_t cdk_cfg_set(const char *key, const char *value);

/* 値の有無だけを見る。中身はコピーしない。 */
bool cdk_cfg_has(const char *key);

/* 設定名の一覧（help や `set` コマンド用）。NULL 終端。 */
const char *const *cdk_cfg_keys(void);

/* そのキーが秘密情報か。表示するとき伏せるために使う。 */
bool cdk_cfg_is_secret(const char *key);

#ifdef __cplusplus
}
#endif
