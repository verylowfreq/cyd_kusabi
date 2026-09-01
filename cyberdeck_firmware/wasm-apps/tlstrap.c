/* TLS を開いたまま落ちるアプリ。使い方: wasm run apps/tlstrap.wasm <host>
 *
 * Ctrl-C ではなく **wasm のトラップ**で終わる経路を試す。TLS のセッションは
 * port_sock.c の静的な表に載っていて、閉じるのは cdk_sock_close だけなので、
 * ここで後始末が漏れると**再起動するまで TLS が 1 本も開けなくなる。**
 *
 * これを走らせた後に tls2.wasm を走らせて、1 本目がまだ開けることを見る。 */
#include "cdk_app.h"

CDK_MAIN int cdk_main(int argc)
{
    if (argc < 2) { cdk_print("使い方: tlstrap.wasm <host>\r\n"); return 1; }

    char host[80];
    const int hn = sys_arg(1, host, (int)sizeof host);
    if (hn <= 0) { cdk_print("ホスト名が長すぎます\r\n"); return 1; }

    if (net_up() != 1) { cdk_print("WiFi が繋がっていません\r\n"); return 1; }

    const int sd = sock_open(CDK_SOCK_TLS);
    cdk_printf("sock_open -> %d\r\n", sd);
    if (sd < 0) return 1;

    cdk_printf("接続... %d\r\n", sock_connect(sd, host, hn, 443));
    cdk_print("これから落ちます（sock_close は呼びません）\r\n");

    /* リニアメモリの外を触ってトラップさせる。sock_close は通らない。 */
    volatile char *p = (volatile char *)0x7ffffff0;
    *p = 1;

    cdk_print("ここには来ないはず\r\n");
    sock_close(sd);
    return 0;
}
