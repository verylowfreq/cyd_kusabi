/* I2C のバスを走査する。使い方: wasm run apps/i2cscan.wasm <sda> <scl>
 *
 * `i2c_probe` で叩くので、シェルの `i2c scan` と同じ結果になる
 * （あちらも同じ i2c_master_probe を通る）。バスは開けたままにする点だけが違う。
 *
 * **昔は i2c_read で走査していた。あれは相手を壊す。** 予期しない読みを
 * 投げられた SHT30 が、来ない続きを待って SCL を握り続け、ユニットの電源を
 * 抜くまでバスごと死んだ（実機で再現）。そのうえ 8 枠のハンドルを使い切って
 * 0x10 より上を一切見つけられなかった。走査は必ず i2c_probe で書くこと。
 */
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
        if (i2c_probe(addr) >= 0) {
            cdk_printf("  0x%x\r\n", addr);
            found++;
        }
    }
    cdk_printf("%d 個\r\n", found);
    i2c_close();
    return 0;
}
