/* GPIO を点滅させる。使い方: wasm run apps/blink.wasm <pin> [回数]
 * 画面が固まらないことの確認にもなる（別タスクで走っている）。 */
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
    const int pin = arg_int(1, 1);
    const int times = (argc > 2) ? arg_int(2, 10) : 10;

    const int r = gpio_mode(pin, CDK_GPIO_OUT);
    if (r < 0) { cdk_printf("G%d は使えません (%d)\r\n", pin, r); return 1; }

    for (int i = 0; i < times; i++) {
        gpio_set(pin, 1); sys_delay(250);
        gpio_set(pin, 0); sys_delay(250);
        cdk_printf("\r%d / %d", i + 1, times);
    }
    cdk_print("\r\n終わり\r\n");
    return 0;
}
