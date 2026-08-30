/* 外部 UART と端末をつなぐ。使い方: wasm run apps/uartecho.wasm <tx> <rx> [baud]
 * 端末に打った文字を UART へ、UART から来た文字を端末へ。Ctrl-C で終わり。 */
#include "cdk_app.h"

static int arg_int(int idx, int fallback)
{
    char a[16];
    const int n = sys_arg(idx, a, (int)sizeof a);
    if (n <= 0) return fallback;
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (a[i] < '0' || a[i] > '9') return fallback;
        v = v * 10 + (a[i] - '0');
    }
    return v;
}

CDK_MAIN int cdk_main(int argc)
{
    (void)argc;
    const int tx = arg_int(1, 43);
    const int rx = arg_int(2, 44);
    const int baud = arg_int(3, 115200);

    const int h = uart_open(tx, rx, baud);
    if (h < 0) { cdk_printf("uart_open に失敗 (%d)\r\n", h); return 1; }
    cdk_printf("TX=G%d RX=G%d %d bps。Ctrl-C で終わり\r\n", tx, rx, baud);

    for (;;) {
        char buf[64];
        int  idle = 1;

        int n = con_read(buf, (int)sizeof buf);
        if (n > 0) { uart_write(h, buf, n); idle = 0; }

        n = uart_read(h, buf, (int)sizeof buf);
        if (n > 0) { con_write(buf, n); idle = 0; }

        if (idle) sys_delay(10);
    }
}
