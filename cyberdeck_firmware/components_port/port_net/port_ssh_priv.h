/* port_net の中だけで使う口。**cdk_net.h には出さない。**
 *
 * `LIBSSH2_SESSION*` を main/ まで見せると、DESIGN.md §8.2 の
 * 「セッションは 1 本」を外から壊せるようになる。port_scp.c は同じ
 * コンポーネントの中なので、ここから借りる。
 */
#pragma once

#include "libssh2.h"

#ifdef __cplusplus
extern "C" {
#endif

/* いま開いているセッション。無ければ NULL。
 * `cdk_ssh_session_open()` が成功したあとだけ意味がある。 */
LIBSSH2_SESSION *cdk_ssh_session_raw(void);

/* メインループが刻む向きへ切り替える。**セッションとソケットの両方**。
 *
 * 片方だけだと詰まる: セッションだけノンブロッキングにしてソケットを
 * blocking のままにすると、libssh2 の `recv()` がカーネルの中で待つので、
 * EAGAIN が返らずメインループが止まる（実機で踏んだ）。
 *
 * 期限も 0 に戻す。残すと待機中の `channel_read` が EAGAIN ではなく
 * `LIBSSH2_ERROR_TIMEOUT` を返し、通信断と誤判定される。 */
void cdk_ssh_go_nonblocking(void);

#ifdef __cplusplus
}
#endif
