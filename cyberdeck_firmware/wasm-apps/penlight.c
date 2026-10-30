/* 制御盤のスライダーとボタンで、Wi-Fi 越しにペンライトの色と振動を操る。
 * 使い方: wasm run apps/penlight.wasm [host]   （既定は esp32-ota.local）
 *
 * 制御盤とは UART (TX=G48 RX=G47 115200)。1 フレーム = cmd, len, data[len], 0x00。
 *   01 00 00     → 01 len "名前" 00
 *   02 00 00     → 02 04 ボタン スライダー1 スライダー2 スライダー3 00   （各 0〜255）
 *   03 01 on 00  → 03 00 00   ボタンの LED。制御盤は 500ms 来なければ自分で消す
 *
 * ペンライトへは平文の OSC を UDP 6668 へ送る。グループは 0。
 *   /led/set 0 r g b            スライダー 1/2/3 = R/G/B
 *   /vibration/set 0 128 300    ボタンを押している間 200ms ごとに送り直す
 *   /vibration/set 0 128 0      離したら止める
 * 2 引数の /vibration/set は強さを NVS に書くだけで回らないので使わない。
 *
 * Ctrl-C で抜けると後片付けはできない。振動と LED は送り直しが止まれば
 * 0.3〜0.5 秒で勝手に止まる。色は最後の色のまま残る。 */

#include <stdint.h>

#include "cdk_app.h"

#define PIN_TX   48
#define PIN_RX   47
#define BAUDRATE 115200

#define OSC_PORT 6668
#define GROUP    0

#define POLL_MS        50
#define REPLY_MS       30   /* 制御盤は 1ms 前後で返す。来なければ諦める */
#define IDENT_WAIT_MS  5000
#define COLOR_MIN_STEP 2    /* ADC の揺れで送り続けないための幅 */
#define COLOR_KEEP_MS  1000 /* UDP は落ちるので、変わらなくてもこの間隔で送り直す */
#define VIB_DUTY       128
#define VIB_PULSE_MS   300
#define VIB_REPEAT_MS  200

#define CMD_IDENT 0x01
#define CMD_READ  0x02
#define CMD_LED   0x03

/* ---- 制御盤 ---------------------------------------------------------- */

static void uart_drain(int h)
{
    uint8_t b[32];
    while (uart_read(h, b, (int)sizeof b) > 0) {}
}

/* 1 バイト待つ。期限までに来なければ -1。
 * sys_delay を挟むのは Ctrl-C を効かせるため（効くのはそこだけ）。 */
static int uart_getc(int h, cdk_i64 deadline)
{
    for (;;) {
        uint8_t b;
        if (uart_read(h, &b, 1) == 1) return b;
        if (sys_now_ms() >= deadline) return -1;
        sys_delay(1);
    }
}

/* 1 フレーム送って同じ cmd の返事を受ける。受けた data の長さを返す。
 * cap を超えた分は読み捨てる。途中で切れた返事は失敗として -1。 */
static int transact(int h, int cmd, const uint8_t *data, int n, uint8_t *out, int cap)
{
    uint8_t req[8];
    if (n > (int)sizeof req - 3) return -1;
    req[0] = (uint8_t)cmd;
    req[1] = (uint8_t)n;
    if (n > 0) memcpy(req + 2, data, (cdk_size_t)n);
    req[2 + n] = 0x00;

    uart_drain(h); /* 前に諦めた返事の残りを捨てる */
    uart_write(h, req, n + 3);

    const cdk_i64 deadline = sys_now_ms() + REPLY_MS;
    if (uart_getc(h, deadline) != cmd) return -1;
    const int len = uart_getc(h, deadline);
    if (len < 0) return -1;
    for (int i = 0; i < len; i++) {
        const int b = uart_getc(h, deadline);
        if (b < 0) return -1;
        if (i < cap) out[i] = (uint8_t)b;
    }
    if (uart_getc(h, deadline) != 0x00) return -1;
    return len < cap ? len : cap;
}

/* ---- OSC ------------------------------------------------------------- */

/* NUL を含めて 4 の倍数に詰める。 */
static int osc_put_str(uint8_t *p, const char *s)
{
    const int n = (int)strlen(s);
    const int total = (n + 4) & ~3;
    memcpy(p, s, (cdk_size_t)n);
    memset(p + n, 0, (cdk_size_t)(total - n));
    return total;
}

static int osc_put_i32(uint8_t *p, int32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
    return 4;
}

static int send_failed; /* 送れない状態が続いているあいだ 1。同じ文句を繰り返さない */

/* 引数は全部 int32。 */
static void osc_send(int sd, const char *addr, const int32_t *args, int nargs)
{
    uint8_t pkt[64];
    char    tag[8] = ",";
    for (int i = 0; i < nargs; i++) tag[1 + i] = 'i';
    tag[1 + nargs] = 0;

    int n = osc_put_str(pkt, addr);
    n += osc_put_str(pkt + n, tag);
    for (int i = 0; i < nargs; i++) n += osc_put_i32(pkt + n, args[i]);

    const int r = sock_send(sd, pkt, n);
    if (r < 0 && !send_failed) cdk_printf("\r\nペンライトへ送れません (%d)\r\n", r);
    send_failed = r < 0;
}

static void send_color(int sd, int r, int g, int b)
{
    const int32_t a[] = { GROUP, r, g, b };
    osc_send(sd, "/led/set", a, 4);
}

static void send_vibration(int sd, int ms)
{
    const int32_t a[] = { GROUP, VIB_DUTY, ms };
    osc_send(sd, "/vibration/set", a, 3);
}

/* ---- 本体 ------------------------------------------------------------ */

static int absdiff(int a, int b) { return a > b ? a - b : b - a; }

CDK_MAIN int cdk_main(int argc)
{
    char host[80] = "esp32-ota.local";
    int  hn = (int)strlen(host);
    if (argc > 1) {
        hn = sys_arg(1, host, (int)sizeof host);
        if (hn <= 0) { cdk_print("ホスト名が長すぎます\r\n"); return 1; }
    }

    const int h = uart_open(PIN_TX, PIN_RX, BAUDRATE);
    if (h < 0) { cdk_printf("uart_open に失敗 (%d)\r\n", h); return 1; }

    /* 制御盤は USB が繋がっていないと起動に 3 秒かかる。電源を入れてすぐ
     * 走らせても諦めないよう、5 秒は問い続ける。 */
    uint8_t       name[64];
    int           r = -1;
    const cdk_i64 ident_until = sys_now_ms() + IDENT_WAIT_MS;
    for (;;) {
        r = transact(h, CMD_IDENT, 0, 0, name, (int)sizeof name - 1);
        if (r >= 0 || sys_now_ms() >= ident_until) break;
        sys_delay(100);
    }
    if (r < 0) {
        cdk_printf("制御盤が応答しません（TX=G%d RX=G%d %d bps）\r\n", PIN_TX, PIN_RX, BAUDRATE);
        uart_close(h);
        return 1;
    }
    name[r] = 0;
    cdk_printf("制御盤: %s", (const char *)name); /* 名前は CR+LF で終わっている */

    if (net_up() != 1) { cdk_print("WiFi が繋がっていません\r\n"); uart_close(h); return 1; }

    const int sd = sock_open(CDK_SOCK_UDP);
    if (sd < 0) { cdk_printf("sock_open に失敗 (%d)\r\n", sd); uart_close(h); return 1; }

    cdk_printf("%s を探しています...\r\n", host);
    const cdk_i64 t0 = sys_now_ms();
    r = sock_connect(sd, host, hn, OSC_PORT);
    if (r < 0) {
        cdk_printf("見つかりません (%d)。IP が分かれば引数で渡せます:\r\n"
                   "  wasm run apps/penlight.wasm 192.168.x.x\r\n", r);
        sock_close(sd);
        uart_close(h);
        return 1;
    }
    cdk_printf("見つかりました (%d ms)。UDP %d へ送ります。Ctrl-C で終わり\r\n",
               (int)(sys_now_ms() - t0), OSC_PORT);

    int     cr = -1, cg = -1, cb = -1; /* 最後に送った色 */
    cdk_i64 color_at = 0;
    int     vib = 0;
    cdk_i64 vib_at = 0;
    int     led_ok = 1; /* 古い制御盤は 03 を知らない。一度断られたら送らない */
    int     lost = 0;

    for (;;) {
        const cdk_i64 tick = sys_now_ms();
        uint8_t       st[4];

        if (transact(h, CMD_READ, 0, 0, st, 4) != 4) {
            if (!lost) cdk_print("\r\n制御盤から読めません\r\n");
            lost = 1;
            /* ボタンが見えないので、押しっぱなし扱いにせず止める */
            if (vib) { send_vibration(sd, 0); vib = 0; }
            sys_delay(POLL_MS);
            continue;
        }
        if (lost) cdk_print("制御盤が戻りました\r\n");
        lost = 0;

        const int btn = st[0] != 0;
        const int sr = st[1], sg = st[2], sb = st[3];
        int       changed = 0;

        if (absdiff(sr, cr) >= COLOR_MIN_STEP || absdiff(sg, cg) >= COLOR_MIN_STEP ||
            absdiff(sb, cb) >= COLOR_MIN_STEP || tick - color_at >= COLOR_KEEP_MS) {
            changed = sr != cr || sg != cg || sb != cb;
            send_color(sd, sr, sg, sb);
            cr = sr; cg = sg; cb = sb;
            color_at = tick;
        }

        if (btn) {
            if (!vib || tick - vib_at >= VIB_REPEAT_MS) {
                send_vibration(sd, VIB_PULSE_MS);
                if (led_ok) {
                    const uint8_t on = 1;
                    if (transact(h, CMD_LED, &on, 1, 0, 0) < 0) {
                        cdk_print("\r\n制御盤が LED のコマンドを知りません（スケッチの書き込みが要る）\r\n");
                        led_ok = 0;
                    }
                }
                if (!vib) changed = 1;
                vib = 1;
                vib_at = tick;
            }
        } else if (vib) {
            send_vibration(sd, 0);
            if (led_ok) {
                const uint8_t off = 0;
                (void)transact(h, CMD_LED, &off, 1, 0, 0);
            }
            vib = 0;
            changed = 1;
        }

        if (changed) {
            cdk_printf("\r色 R=%d G=%d B=%d  振動 %s\033[K", cr, cg, cb, vib ? "ON" : "--");
        }

        const cdk_i64 wait = tick + POLL_MS - sys_now_ms();
        sys_delay(wait > 0 ? (int)wait : 1);
    }
}
