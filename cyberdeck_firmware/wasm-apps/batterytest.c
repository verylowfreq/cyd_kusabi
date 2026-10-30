#include "cdk_app.h"


static int append_str(char *buf, int pos, int buflen, const char *str)
{
    int len = strlen(str);

    if (pos >= buflen) {
        return pos;
    }

    /* 最後の '\0' 用に1byte残す */
    if (len > buflen - pos - 1) {
        len = buflen - pos - 1;
    }

    memcpy(&buf[pos], str, len);
    return pos + len;
}


static int append_int(char *buf, int pos, int buflen, int value)
{
    char tmp[16];
    int n = 0;
    unsigned int v;

    /*
     * INT_MINでもオーバーフローしないように、
     * unsignedで絶対値を作る。
     */
    if (value < 0) {
        if (pos + 1 < buflen) {
            buf[pos++] = '-';
        }

        v = 0u - (unsigned int)value;
    } else {
        v = (unsigned int)value;
    }

    /* 逆順に数字を作る */
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u);

    /* 正しい順番でbufへ書き込む */
    while (n != 0) {
        if (pos + 1 >= buflen) {
            break;
        }

        buf[pos++] = tmp[--n];
    }

    return pos;
}


void build_http_get_request(char *buf, int buflen, int data)
{
    int pos = 0;

    if (buf == 0 || buflen == 0) {
        return;
    }

    pos = append_str(buf, pos, buflen, "GET /?vbat=");
    pos = append_int(buf, pos, buflen, data);
    pos = append_str(buf, pos, buflen,
                     " HTTP/1.0\r\n"
                     "Connection: close\r\n"
                     "\r\n");

    buf[pos] = '\0';
}

CDK_MAIN int cdk_main(int argc)
{
    (void)argc;

    cdk_print("1分おきにHTTPリクエストを飛ばします");
    
    const char host[] = "192.168.1.205";
    const int port = 8080;

    while (1) {
        if (net_up() != 1) {
            cdk_print("ネットワークに接続されていません\r\n");
            goto interval;
        }

        int sock = sock_open(CDK_SOCK_TCP);
        if (sock < 0) {
            cdk_printf("sock_open()失敗: %d\r\n", sock);
            goto interval;
        }

        cdk_printf("%sへ接続中...\r\n", host);
        int result = sock_connect(sock, host, strlen(host), port);
        if (result < 0) {
            cdk_printf("接続に失敗： %d\r\n", result);
            goto interval;
        }
        int adc_value_mv = adc_read(4);
        int vbat_mv = adc_value_mv * 2;
        char buf[256] = {};
        build_http_get_request(buf, 255, vbat_mv);
        // snprintf(&buf[0],
        //     "GET /?vbat=%d HTTP/1.0\r\n"
        //     "Connection: close\r\n"
        //     "\r\n",
        //     vbat_mv);

        cdk_printf("送信中（VBAT=%dmV)  ", vbat_mv);

        if (sock_send(sock, buf, strlen(buf)) < 0) {
            cdk_printf("失敗：%d\r\n", result);
        } else {
            cdk_print("成功\r\n");
        }
        sock_close(sock);
        sock = 0;
interval:
        if (sock > 0) {
            sock_close(sock);
            sock = 0;
        }
        sys_delay(60 * 1000);
    }

    return 0;

    // if (argc < 2) { cdk_print("使い方: httpget.wasm <host> [path]\r\n"); return 1; }

    // char host[80];
    // const int hn = sys_arg(1, host, (int)sizeof host);
    // if (hn <= 0) { cdk_print("ホスト名が長すぎます\r\n"); return 1; }

    // char path[80] = "/";
    // int  pn = 1;
    // if (argc > 2) {
    //     pn = sys_arg(2, path, (int)sizeof path);
    //     if (pn <= 0) { path[0] = '/'; path[1] = 0; pn = 1; }
    // }

    // if (net_up() != 1) { cdk_print("WiFi が繋がっていません\r\n"); return 1; }

    // const int sd = sock_open(CDK_SOCK_TCP);
    // if (sd < 0) { cdk_printf("sock_open に失敗 (%d)\r\n", sd); return 1; }

    // cdk_printf("%s へ接続中...\r\n", host);
    // int r = sock_connect(sd, host, hn, 80);
    // if (r < 0) { cdk_printf("繋がりません (%d)\r\n", r); sock_close(sd); return 1; }

    // /* リクエストを組む。SDK に snprintf は無いので継ぎ足しで書く。 */
    // char req[256];
    // int  n = 0;
    // const char *g = "GET ";
    // for (int i = 0; g[i]; i++) req[n++] = g[i];
    // for (int i = 0; i < pn; i++) req[n++] = path[i];
    // const char *h1 = " HTTP/1.0\r\nHost: ";
    // for (int i = 0; h1[i]; i++) req[n++] = h1[i];
    // for (int i = 0; i < hn; i++) req[n++] = host[i];
    // const char *h2 = "\r\nConnection: close\r\n\r\n";
    // for (int i = 0; h2[i]; i++) req[n++] = h2[i];

    // if (sock_send(sd, req, n) < 0) { cdk_print("送れません\r\n"); sock_close(sd); return 1; }

    // int total = 0;
    // for (;;) {
    //     char buf[256];
    //     const int got = sock_recv(sd, buf, (int)sizeof buf);
    //     if (got > 0) { con_write(buf, got); total += got; continue; }
    //     if (got == 0) { sys_delay(20); continue; } /* まだ来ていない */
    //     break;                                     /* 相手が閉じた */
    // }
    // cdk_printf("\r\n--- %d バイト ---\r\n", total);
    // sock_close(sd);
    // return 0;
}
