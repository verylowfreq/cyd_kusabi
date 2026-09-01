/* TLS の recv が「200ms 待って何も無ければ 0」を守っているか実機で測る。
 * 使い方: wasm run apps/tlsidle.wasm <host>
 *
 * 繋いだあと**何も送らずに**受信だけ回す。相手は黙っているので sock_recv は
 * 毎回 0 を返す。3 秒間の呼び出し回数を数えると:
 *
 *   約 15 回   … 200ms の上限が効いている（正しい）
 *   数千回     … ハンドシェイク後にソケットを非ブロッキングのまま残した
 *                （esp_tls が即 WANT_READ を返し、ゲストが空回りする）
 *
 * Ctrl-C がこの最中に効くことも併せて見る。 */
#include "cdk_app.h"

CDK_MAIN int cdk_main(int argc)
{
    if (argc < 2) { cdk_print("使い方: tlsidle.wasm <host>\r\n"); return 1; }

    char host[80];
    const int hn = sys_arg(1, host, (int)sizeof host);
    if (hn <= 0) { cdk_print("ホスト名が長すぎます\r\n"); return 1; }

    if (net_up() != 1) { cdk_print("WiFi が繋がっていません\r\n"); return 1; }

    const int sd = sock_open(CDK_SOCK_TLS);
    if (sd < 0) { cdk_printf("sock_open に失敗 (%d)\r\n", sd); return 1; }

    const int r = sock_connect(sd, host, hn, 443);
    if (r < 0) { cdk_printf("繋がりません (%d)\r\n", r); sock_close(sd); return 1; }
    cdk_print("繋がりました。何も送らずに 3 秒間受信を回します\r\n");

    const cdk_i64 t0 = sys_now_ms();
    int           calls = 0;
    for (;;) {
        char      buf[64];
        const int got = sock_recv(sd, buf, (int)sizeof buf);
        calls++;
        if (got < 0) { cdk_print("相手が閉じました\r\n"); break; }
        if (sys_now_ms() - t0 >= 3000) break;
    }
    const int ms = (int)(sys_now_ms() - t0);

    cdk_printf("%d ms で %d 回\r\n", ms, calls);
    if (calls <= 40) cdk_print("OK: 200ms の上限が効いています\r\n");
    else             cdk_print("NG: 空回りしています\r\n");

    sock_close(sd);
    return 0;
}
