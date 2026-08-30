/* cdk_sock — ユーザーアプリ向けの素の TCP / UDP ソケット。
 *
 * SSH (cdk_ssh_*) とは別物。あちらは 1 本のセッションを握る専用の口で、
 * こちらはアプリが好きに開ける薄い包み。
 *
 * **どの呼び出しも有限時間で戻る。** ユーザーアプリを Ctrl-C で止められる
 * のは native の入口だけなので、ここでブロックすると止められなくなる。
 */
#ifndef CDK_SOCK_H
#define CDK_SOCK_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CDK_SOCK_TYPE_TCP 0
#define CDK_SOCK_TYPE_UDP 1

/* 戻り値。0 以上はバイト数や成功。 */
#define CDK_SOCK_CLOSED  (-2) /* 相手が閉じた */
#define CDK_SOCK_TIMEOUT (-3)
#define CDK_SOCK_ABORTED (-4) /* abort_fn が true を返した */

/* 接続を待っている間、刻むたびに呼ばれる。true を返すとやめる。
 * これで port_net は port_wasm を知らずに Ctrl-C に従える。 */
typedef bool (*cdk_sock_abort_fn)(void);

/* 失敗したら -1。受信は最初から 200ms の上限つき。 */
int cdk_sock_open(int type);

/* 名前解決して繋ぐ。**非ブロッキングで刻みながら abort_fn を見る。**
 * 成功 0 / CDK_SOCK_TIMEOUT / CDK_SOCK_ABORTED / -1。 */
int cdk_sock_connect(int sd, const char *host, int port, int timeout_ms,
                     cdk_sock_abort_fn abort_fn);

int cdk_sock_send(int sd, const void *p, int n);

/* 200ms 待って何も来なければ 0。相手が閉じたら CDK_SOCK_CLOSED。 */
int cdk_sock_recv(int sd, void *p, int n);

int cdk_sock_close(int sd);

#ifdef __cplusplus
}
#endif

#endif /* CDK_SOCK_H */
