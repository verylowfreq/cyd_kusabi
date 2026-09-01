/* TLS で HTTP GET する。使い方: wasm run apps/httpsget.wasm <host> [path] [-k]
 *
 * httpget.c との違いは sock_open の型と既定のポートだけ。**TLS はファーム側
 * (ESP-IDF の esp-tls) が終端するので、繋がったあとの send / recv は
 * 素の TCP とまったく同じに書ける。**
 *
 * -k を付けると証明書を検証しない（self-signed.badssl.com のような相手を
 * 試すため）。普段は付けないこと。 */
#include "cdk_app.h"

CDK_MAIN int cdk_main(int argc)
{
    if (argc < 2) { cdk_print("使い方: httpsget.wasm <host> [path] [-k]\r\n"); return 1; }

    char host[80];
    const int hn = sys_arg(1, host, (int)sizeof host);
    if (hn <= 0) { cdk_print("ホスト名が長すぎます\r\n"); return 1; }

    char path[80] = "/";
    int  pn = 1;
    if (argc > 2) {
        pn = sys_arg(2, path, (int)sizeof path);
        if (pn <= 0) { path[0] = '/'; path[1] = 0; pn = 1; }
    }

    /* -k はどの位置に来てもよい。引数が少ないので素直に全部見る。 */
    int insecure = 0;
    for (int i = 1; i < argc; i++) {
        char a[8];
        const int an = sys_arg(i, a, (int)sizeof a);
        if (an == 2 && a[0] == '-' && a[1] == 'k') insecure = 1;
    }
    /* path の位置に -k が来ていたら、path は既定に戻す。 */
    if (pn == 2 && path[0] == '-' && path[1] == 'k') { path[0] = '/'; path[1] = 0; pn = 1; }

    if (net_up() != 1) { cdk_print("WiFi が繋がっていません\r\n"); return 1; }

    if (insecure) cdk_print("** 証明書を検証しません **\r\n");
    const int sd = sock_open(insecure ? CDK_SOCK_TLS_INSECURE : CDK_SOCK_TLS);
    if (sd < 0) { cdk_printf("sock_open に失敗 (%d)\r\n", sd); return 1; }

    cdk_printf("%s へ TLS で接続中...\r\n", host);
    int r = sock_connect(sd, host, hn, 443);
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
