/* cdk_net — WiFi と SSH クライアント（M2 実装中）
 *
 * SSH は libssh2 1.11.1 + mbedTLS。詳細は
 * components_port/libssh2/README.md と DESIGN.md §8。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* libssh2 がリンクされ、実機で初期化できることを確かめる。
 * M2 の足場が固まったら消す。 */
esp_err_t cdk_net_probe(void);

#ifdef __cplusplus
}
#endif
