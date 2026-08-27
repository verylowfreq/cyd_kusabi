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
