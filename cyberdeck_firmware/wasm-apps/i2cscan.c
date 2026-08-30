/* I2C を叩く。使い方: wasm run apps/i2cscan.wasm <sda> <scl>
 * 見つけたアドレスに 1 バイト書いて応答を見る（scan と違いバスは開けたまま）。 */
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
    const int sda = arg_int(1, 2);
    const int scl = arg_int(2, 1);
    (void)argc;

    const int r = i2c_open(sda, scl, 100000);
    if (r < 0) { cdk_printf("i2c_open に失敗 (%d)\r\n", r); return 1; }
    cdk_printf("SDA=G%d SCL=G%d で探します\r\n", sda, scl);

    int found = 0;
    for (int addr = 0x08; addr <= 0x77; addr++) {
        unsigned char b = 0;
        /* 0 バイト書きは通らないので 1 バイト読んでみる。
         * 居なければエラーが返る。 */
        if (i2c_read(addr, &b, 1) >= 0) {
            cdk_printf("  0x%x\r\n", addr);
            found++;
        }
    }
    cdk_printf("%d 個\r\n", found);
    i2c_close();
    return 0;
}
