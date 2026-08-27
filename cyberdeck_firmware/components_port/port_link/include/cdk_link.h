/* cdk_link — ホスト PC との制御チャネル (DESIGN.md §3.4)
 *
 * 既定では USB-Serial-JTAG を cdk_proto のバイナリフレーム専用に使う。
 * ログは UART1 (PORT.C) に出るので、このチャネルにはテキストが混ざらない。
 *
 * 1 チャネル縮退モード (CDK_LINK_SINGLE) では同じ線にログも流れるが、
 * cdk_proto の受信側が再同期するので機能はする。
 */
#pragma once

#include "cdk_proto.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t cdk_link_init(void);

/* 受信フレームの処理先。init の前後どちらでも設定できる。 */
void cdk_link_set_handler(cdk_proto_frame_fn fn, void *user);

/* 1 フレーム送信する。payload は CDK_PROTO_MAX_PAYLOAD 以下。 */
esp_err_t cdk_link_send(uint8_t type, const void *payload, uint32_t len);

/* 大きなデータを CDK_PROTO_MAX_PAYLOAD ごとに分割して送る。
 * 各フレームの先頭 4 バイトに u32le のバイトオフセットが入る。 */
esp_err_t cdk_link_send_chunked(uint8_t type, const void *data, size_t len);

/* 受信を進める。最大 wait_ms だけ待つ。フレームが揃えばハンドラが呼ばれる。 */
void cdk_link_poll(uint32_t wait_ms);

/* 診断用。再同期が起きていないかを見る。 */
void cdk_link_stats(uint32_t *frames, uint32_t *bad_crc, uint32_t *dropped);

#ifdef __cplusplus
}
#endif
