/* TLS の同時本数の上限を実機で確かめるためのアプリ。
 * 使い方: wasm run apps/tls2.wasm <host>
 *
 * 開けるだけ開き（最大 3 本）、**開けた全部で実際に送受信できるところまで**
 * 見る。上限がいくつでも同じように使えるので、TLS_MAX を変えたときの
 * 確認にそのまま使える。
 *
 * 開けるかどうかだけ見るのでは足りない。TLS_MAX = 2 を試したとき、
 * 2 本目のハンドシェイクで
 *   E esp-aes: Failed to allocate memory
 * が出ただけでなく、**繋がっている 1 本目の送受信まで巻き添えで壊れた。**
 * だから「開けた本数」ではなく「全部でバイトが取れたか」を答えにする。 */
#include "cdk_app.h"

#define TRY 5

CDK_MAIN int cdk_main(int argc)
{
    if (argc < 2) { cdk_print("使い方: tls2.wasm <host>\r\n"); return 1; }

    char host[80];
    const int hn = sys_arg(1, host, (int)sizeof host);
    if (hn <= 0) { cdk_print("ホスト名が長すぎます\r\n"); return 1; }

    if (net_up() != 1) { cdk_print("WiFi が繋がっていません\r\n"); return 1; }

    /* 第 2 引数で試す本数を絞れる。上限より少なく開いたときの様子
     * （とくに 1 本だけのときのハンドシェイク時間）を見るため。 */
    int want = TRY;
    if (argc > 2) {
        char a[4];
        const int an = sys_arg(2, a, (int)sizeof a);
        if (an == 1 && a[0] >= '1' && a[0] <= '9') want = a[0] - '0';
        if (want > TRY) want = TRY;
    }

    int sd[TRY];
    int open_n = 0;
    for (int i = 0; i < want; i++) {
        sd[i] = sock_open(CDK_SOCK_TLS);
        cdk_printf("%d 本目: sock_open -> %d\r\n", i + 1, sd[i]);
        if (sd[i] < 0) break;
        open_n++;
    }
    if (open_n == 0) { cdk_print("1 本も開けませんでした\r\n"); return 1; }
    cdk_printf("開けたのは %d 本\r\n", open_n);

    /* ハンドシェイクにかかる時間も見る。mbedtls を PSRAM に置くと
     * 遅くなるはずなので、どれくらい払っているのかを数字で残す。 */
    for (int i = 0; i < open_n; i++) {
        const cdk_i64 t0 = sys_now_ms();
        const int     r = sock_connect(sd[i], host, hn, 443);
        cdk_printf("%d 本目を接続... %d (%d ms)\r\n", i + 1, r, (int)(sys_now_ms() - t0));
    }

    const char *req = "GET / HTTP/1.0\r\nConnection: close\r\n\r\n";
    int         n = 0;
    while (req[n]) n++;

    int got[TRY];
    int done[TRY];
    for (int i = 0; i < open_n; i++) {
        got[i] = 0;
        done[i] = 0;
        if (sock_send(sd[i], req, n) < 0) {
            cdk_printf("%d 本目: 送れません\r\n", i + 1);
            done[i] = 1;
        }
    }

    for (;;) {
        int alive = 0;
        for (int i = 0; i < open_n; i++) {
            if (done[i]) continue;
            char      buf[128];
            const int r = sock_recv(sd[i], buf, (int)sizeof buf);
            if (r > 0)       { got[i] += r; alive = 1; }
            else if (r == 0) { alive = 1; }
            else             { done[i] = 1; }
        }
        if (!alive) break;
        sys_delay(20);
    }

    int ok = 1;
    for (int i = 0; i < open_n; i++) {
        cdk_printf("%d 本目 %d バイト\r\n", i + 1, got[i]);
        if (got[i] <= 0) ok = 0;
        sock_close(sd[i]);
    }
    if (ok) cdk_printf("OK: %d 本とも取れました\r\n", open_n);
    else    cdk_print("NG: 取れなかった本があります\r\n");
    return ok ? 0 : 1;
}
