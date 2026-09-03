/* M5Stack Unit ENV III（SHT30 + QMP6988）を読む。
 * 使い方: wasm run apps/envunit3.wasm [<sda> <scl>]  既定は Grove の G17/G18。
 *
 * 実機で I2C を確かめるための道具でもあるので、**失敗したら理由の番号を
 * 必ず出す**。
 *
 *   -3 (CDK_E_NOENT) そのアドレスが返事をしない (NACK)、または
 *                    バスが返事を待ったまま時間切れ。**どちらかは
 *                    ログ UART を見る。** `i2c.master` の E 行が出ていたら
 *                    配線かプルアップ、出ていなければ誰も居ない。
 *   -2 (CDK_E_PERM)  予約ピンなどで断られた（i2c_open のとき）。
 *   -5 (CDK_E_NOMEM) I2C が既に開いている（1 本だけ）。
 */
#include <stdint.h>
#include "cdk_app.h"

#define SHT30_ADDR   0x44
#define QMP6988_ADDR 0x76

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

/* SHT3x の CRC-8（多項式 0x31、初期値 0xFF）。
 * これを見ないと、配線がおかしくて 0xFF が並んでいるだけの状態を
 * 「気温 130℃」として表示してしまう。 */
static uint8_t crc8(const uint8_t *p, int n)
{
    uint8_t c = 0xFF;
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++) {
            c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x31) : (uint8_t)(c << 1);
        }
    }
    return c;
}

/* 100 倍した固定小数を「-12.34」の形で出す。cdk_printf に %f は無い。 */
static void print_centi(const char *label, int centi)
{
    const char *sign = (centi < 0) ? "-" : "";
    if (centi < 0) centi = -centi;
    const int frac = centi % 100;
    cdk_printf("%s%s%d.%d%d\r\n", label, sign, centi / 100,
               frac / 10, frac % 10);
}

CDK_MAIN int cdk_main(int argc)
{
    const int sda = arg_int(1, 17);
    const int scl = arg_int(2, 18);
    (void)argc;

    int r = i2c_open(sda, scl, 400000);
    if (r < 0) { cdk_printf("i2c_open に失敗 (%d)\r\n", r); return r; }
    cdk_printf("SDA=G%d SCL=G%d\r\n", sda, scl);

    /* まず「居るか」を見る。ここで NOENT なら配線か電源の話で、
     * 下の測定を直しても意味が無い。0x70 は同じ基板の気圧計なので、
     * 片方だけ返事をするなら基板は生きている。 */
    uint8_t status_cmd[2] = { 0xF3, 0x2D }; /* SHT3x: ステータス読み出し */
    uint8_t status[3] = { 0, 0, 0 };
    r = i2c_wrrd(SHT30_ADDR, status_cmd, 2, status, 3);
    if (r < 0) {
        cdk_printf("SHT30 (0x44) が返事をしない (%d)\r\n", r);
        uint8_t b = 0;
        const int q = i2c_read(QMP6988_ADDR, &b, 1);
        cdk_printf("参考: QMP6988 (0x70) は %s (%d)\r\n",
                   (q >= 0) ? "居る" : "居ない", q);
        i2c_close();
        return CDK_E_IO;
    }
    cdk_printf("SHT30 は居る (status %x %x)\r\n", status[0], status[1]);

    sys_delay(50);

    /* 測ってから読む。0x2C06 は高精度・クロックストレッチ有効。
     * 最長 15ms 掛かるので、余裕を見て 50ms 待ってから読む。 */
    uint8_t meas_cmd[2] = { 0x2C, 0x06 };
    r = i2c_write(SHT30_ADDR, meas_cmd, 2);
    if (r < 0) { cdk_printf("測定命令に失敗 (%d)\r\n", r); i2c_close(); return CDK_E_IO; }

    sys_delay(50);

    uint8_t data[6] = { 0, 0, 0, 0, 0, 0 };
    r = i2c_read(SHT30_ADDR, data, 6);
    if (r < 0) { cdk_printf("読み出しに失敗 (%d)\r\n", r); i2c_close(); return CDK_E_IO; }

    if (crc8(&data[0], 2) != data[2] || crc8(&data[3], 2) != data[5]) {
        cdk_printf("CRC が合わない: %x %x %x %x %x %x\r\n",
                   data[0], data[1], data[2], data[3], data[4], data[5]);
        i2c_close();
        return CDK_E_IO;
    }

    /* 浮動小数を使わない。T = -45 + 175*raw/65535、RH = 100*raw/65535 を
     * 100 倍した整数で持つ（17500*65535 は int32 に収まる）。 */
    const int32_t traw = (int32_t)((uint32_t)data[0] << 8 | data[1]);
    const int32_t hraw = (int32_t)((uint32_t)data[3] << 8 | data[4]);
    const int32_t t_centi = (17500 * traw) / 65535 - 4500;
    const int32_t h_centi = (10000 * hraw) / 65535;

    print_centi("温度 [C]:   ", (int)t_centi);
    print_centi("湿度 [%]:   ", (int)h_centi);

    i2c_close();
    return 0;
}
