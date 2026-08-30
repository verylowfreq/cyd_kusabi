/* 素の TCP で HTTP GET する。使い方: wasm run apps/httpget.wasm <host> [path]
 * TLS は無いので http のみ。ソケット API の確認用。 */
#include "cdk_app.h"

CDK_MAIN int cdk_main(int argc)
{
    if (argc < 2) { cdk_print("使い方: httpget.wasm <host> [path]\r\n"); return 1; }

    char host[80];
    const int hn = sys_arg(1, host, (int)sizeof host);
    if (hn <= 0) { cdk_print("ホスト名が長すぎます\r\n"); return 1; }

    char path[80] = "/";
    int  pn = 1;
    if (argc > 2) {
        pn = sys_arg(2, path, (int)sizeof path);
        if (pn <= 0) { path[0] = '/'; path[1] = 0; pn = 1; }
    }

    if (net_up() != 1) { cdk_print("WiFi が繋がっていません\r\n"); return 1; }

    const int sd = sock_open(CDK_SOCK_TCP);
    if (sd < 0) { cdk_printf("sock_open に失敗 (%d)\r\n", sd); return 1; }

    cdk_printf("%s へ接続中...\r\n", host);
    int r = sock_connect(sd, host, hn, 80);
    if (r < 0) { cdk_printf("繋がりません (%d)\r\n", r); sock_close(sd); return 1; }

    /* リクエストを組む。SDK に snprintf は無いので継ぎ足しで書く。 */
    char req[256];
    int  n = 0;
    const char *g = "GET ";
    for (int i = 0; g[i]; i++) req[n++] = g[i];
    for (int i = 0; i < pn; i++) req[n++] = path[i];
    const char *h1 = " HTTP/1.0\r\nHost: ";
    for (int i = 0; h1[i]; i++) req[n++] = h1[i];
    for (int i = 0; i < hn; i++) req[n++] = host[i];
    const char *h2 = "\r\nConnection: close\r\n\r\n";
    for (int i = 0; h2[i]; i++) req[n++] = h2[i];

    if (sock_send(sd, req, n) < 0) { cdk_print("送れません\r\n"); sock_close(sd); return 1; }

    int total = 0;
    for (;;) {
        char buf[256];
        const int got = sock_recv(sd, buf, (int)sizeof buf);
        if (got > 0) { con_write(buf, got); total += got; continue; }
        if (got == 0) { sys_delay(20); continue; } /* まだ来ていない */
        break;                                     /* 相手が閉じた */
    }
    cdk_printf("\r\n--- %d バイト ---\r\n", total);
    sock_close(sd);
    return 0;
}
