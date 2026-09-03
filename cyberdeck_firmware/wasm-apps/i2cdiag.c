/* I2C の叩き方ごとに番号を出す。8 枠の退避とは切り離して見るため、
 * 触るアドレスは 2 つだけ（キャッシュは埋まらない）。
 * 使い方: wasm run apps/i2cdiag.wasm [<sda> <scl>] */
#include <stdint.h>
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
    const int sda = arg_int(1, 17);
    const int scl = arg_int(2, 18);
    (void)argc;

    int r = i2c_open(sda, scl, 100000);
    if (r < 0) { cdk_printf("i2c_open 失敗 (%d)\r\n", r); return 1; }

    uint8_t b = 0;

    /* (1) 居るか。**probe で見る。**
     *
     * ここは前は素の 1 バイト読みだった。**それでこのバスを殺した。**
     * 実機で見えた順番: `read 0x70` は 0（成功）、次の `read 0x44` が -3、
     * そこから先は 0x70 への書きまで全部 -3。待機中の SHT30 に読みを
     * 1 回投げると、来ない続きを待って SCL を握り続け、バスに繋がった
     * 全員が道連れになる。ユニットの電源を抜くまで戻らない。
     * 読みが一律に危ないのではなく、**その相手がその時に読みを
     * 待っていないこと**が危ない。 */
    cdk_printf("probe 0x70 -> %d\r\n", i2c_probe(0x70));
    cdk_printf("probe 0x44 -> %d\r\n", i2c_probe(0x44));

    /* (2) 素の 1 バイト読み。0x70 (QMP6988) は応える。
     * **0x44 では絶対にやらないこと。**上の注記のとおり。 */
    r = i2c_read(0x70, &b, 1);
    cdk_printf("read  0x70 -> %d\r\n", r);

    /* (3) レジスタを指してから読む。QMP6988 の 0xD1 は chip ID = 0x5C。
     * 相手は「そのレジスタを読む」と分かっているので安全。 */
    uint8_t reg = 0xD1;
    b = 0;
    r = i2c_wrrd(0x70, &reg, 1, &b, 1);
    cdk_printf("wrrd  0x70 reg D1 -> %d (val %x)\r\n", r, b);

    /* (4) 書きだけ。相手は何も吐き出さないので安全。 */
    r = i2c_write(0x70, &reg, 1);
    cdk_printf("write 0x70 -> %d\r\n", r);
    uint8_t sht_status[2] = { 0xF3, 0x2D };
    r = i2c_write(0x44, sht_status, 2);
    cdk_printf("write 0x44 -> %d\r\n", r);

    /* (5) 8 枠のハンドルを超える。**居ないアドレスだけを叩く**
     * （相手が居ないので読みで止められる心配が無い）。
     * 修正前は 9 個目から -5 (NOMEM) になり、居る・居ないに関わらず
     * 「居ない」と見えていた。いまは全部 -3 (NACK) になるはず。 */
    cdk_printf("-- 9 個目以降が -5 でなく -3 なら退避が効いている\r\n");
    for (int a = 0x08; a <= 0x13; a++) {
        r = i2c_read(a, &b, 1);
        cdk_printf("  %x:%d", a, r);
    }
    cdk_printf("\r\n");

    i2c_close();
    return 0;
}
