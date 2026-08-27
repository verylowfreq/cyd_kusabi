/* cdk_proto の単体テスト。
 *
 * 一番大事なのは「同期外れから必ず復帰すること」。
 * ログの混入、パニック出力、ROM ブートローダのチャタリングが
 * 制御チャネルに割り込んでも、次の正しいフレームを拾えなければならない。
 */
#include "cdk_proto.h"
#include "t.h"

#define MAX_CAPTURED 32

typedef struct {
    int      count;
    uint8_t  type[MAX_CAPTURED];
    uint32_t len[MAX_CAPTURED];
    uint8_t  payload[MAX_CAPTURED][64];
} capture_t;

static void on_frame(void *user, uint8_t type, const uint8_t *payload, uint32_t len)
{
    capture_t *c = (capture_t *)user;
    if (c->count >= MAX_CAPTURED) {
        return;
    }
    c->type[c->count] = type;
    c->len[c->count] = len;
    if (len <= sizeof(c->payload[0])) {
        memcpy(c->payload[c->count], payload, len);
    }
    c->count++;
}

static uint8_t rxbuf[CDK_PROTO_RX_BUF_MIN];

static void reset(cdk_proto_rx_t *rx, capture_t *cap)
{
    memset(cap, 0, sizeof(*cap));
    cdk_proto_rx_init(rx, rxbuf, sizeof(rxbuf), on_frame, cap);
}

int main(void)
{
    cdk_proto_rx_t rx;
    capture_t cap;
    uint8_t frame[256];
    uint8_t frame2[256];

    T_CASE("crc32 known vector");
    {
        /* CRC-32/ISO-HDLC の標準チェック値 */
        T_EQ_U32(cdk_crc32(0, "123456789", 9), 0xCBF43926u);
        /* 分割して呼んでも同じ */
        uint32_t c = cdk_crc32(0, "12345", 5);
        c = cdk_crc32(c, "6789", 4);
        T_EQ_U32(c, 0xCBF43926u);
        /* 空入力は 0 */
        T_EQ_U32(cdk_crc32(0, "", 0), 0u);
    }

    T_CASE("encode の形");
    {
        const char *p = "hi";
        size_t n = cdk_proto_encode(CDK_MSG_PING, p, 2, frame, sizeof(frame));
        T_EQ_INT(n, 2 + CDK_PROTO_OVERHEAD);
        T_EQ_INT(frame[0], CDK_PROTO_MAGIC_0);
        T_EQ_INT(frame[1], 'C');
        T_EQ_INT(frame[2], 'D');
        T_EQ_INT(frame[3], 'K');
        T_EQ_INT(frame[4], CDK_MSG_PING);
        T_EQ_INT(frame[5], 2);
        T_EQ_INT(frame[6], 0);
        T_EQ_INT(frame[7], 0);
        T_EQ_INT(frame[8], 0);
        T_EQ_INT(frame[9], 'h');
        T_EQ_INT(frame[10], 'i');
    }

    T_CASE("out_cap 不足 / payload 超過 は 0 を返す");
    {
        T_EQ_INT(cdk_proto_encode(CDK_MSG_PING, "hi", 2, frame, 5), 0);
        T_EQ_INT(cdk_proto_encode(CDK_MSG_PING, "hi", CDK_PROTO_MAX_PAYLOAD + 1,
                                  frame, sizeof(frame)),
                 0);
    }

    T_CASE("往復");
    {
        reset(&rx, &cap);
        size_t n = cdk_proto_encode(CDK_MSG_TEXT, "hello", 5, frame, sizeof(frame));
        cdk_proto_rx_feed(&rx, frame, n);
        T_EQ_INT(cap.count, 1);
        T_EQ_INT(cap.type[0], CDK_MSG_TEXT);
        T_EQ_INT(cap.len[0], 5);
        T_EQ_MEM(cap.payload[0], "hello", 5);
        T_EQ_U32(rx.stat_dropped, 0);
    }

    T_CASE("payload 0 バイト");
    {
        reset(&rx, &cap);
        size_t n = cdk_proto_encode(CDK_MSG_SHOT_REQ, NULL, 0, frame, sizeof(frame));
        cdk_proto_rx_feed(&rx, frame, n);
        T_EQ_INT(cap.count, 1);
        T_EQ_INT(cap.type[0], CDK_MSG_SHOT_REQ);
        T_EQ_INT(cap.len[0], 0);
    }

    T_CASE("1 バイトずつ投入しても組み立てられる");
    {
        reset(&rx, &cap);
        size_t n = cdk_proto_encode(CDK_MSG_TEXT, "abcdef", 6, frame, sizeof(frame));
        for (size_t i = 0; i < n; i++) {
            cdk_proto_rx_feed(&rx, frame + i, 1);
        }
        T_EQ_INT(cap.count, 1);
        T_EQ_MEM(cap.payload[0], "abcdef", 6);
    }

    T_CASE("連続した複数フレーム");
    {
        reset(&rx, &cap);
        uint8_t buf[512];
        size_t n = 0;
        n += cdk_proto_encode(CDK_MSG_PING, "a", 1, buf + n, sizeof(buf) - n);
        n += cdk_proto_encode(CDK_MSG_PONG, "bb", 2, buf + n, sizeof(buf) - n);
        n += cdk_proto_encode(CDK_MSG_READY, "ccc", 3, buf + n, sizeof(buf) - n);
        cdk_proto_rx_feed(&rx, buf, n);
        T_EQ_INT(cap.count, 3);
        T_EQ_INT(cap.type[0], CDK_MSG_PING);
        T_EQ_INT(cap.type[1], CDK_MSG_PONG);
        T_EQ_INT(cap.type[2], CDK_MSG_READY);
        T_EQ_U32(rx.stat_dropped, 0);
    }

    T_CASE("前後にログテキストが混ざっても拾える");
    {
        reset(&rx, &cap);
        const char *log = "I (123) cdk: booting\r\n";
        size_t n = cdk_proto_encode(CDK_MSG_READY, "v1", 2, frame, sizeof(frame));
        cdk_proto_rx_feed(&rx, log, strlen(log));
        cdk_proto_rx_feed(&rx, frame, n);
        cdk_proto_rx_feed(&rx, log, strlen(log));
        T_EQ_INT(cap.count, 1);
        T_EQ_INT(cap.type[0], CDK_MSG_READY);
        /* ログ 2 本ぶんが捨てられる。末尾 3 バイトは magic 判定に必要な長さに
         * 満たないためバッファに残る。 */
        T_EQ_U32(rx.stat_dropped, (uint32_t)(strlen(log) * 2 - 3));
        T_EQ_U32(rx.stat_bad_crc, 0u);
    }

    T_CASE("payload の中に magic が入っていても壊れない");
    {
        reset(&rx, &cap);
        const uint8_t evil[] = {CDK_PROTO_MAGIC_0, 'C', 'D', 'K', 0x20,
                                0xFF,              0xFF, 0xFF, 0xFF};
        size_t n = cdk_proto_encode(CDK_MSG_TEXT, evil, sizeof(evil), frame,
                                    sizeof(frame));
        cdk_proto_rx_feed(&rx, frame, n);
        T_EQ_INT(cap.count, 1);
        T_EQ_INT(cap.len[0], sizeof(evil));
        T_EQ_MEM(cap.payload[0], evil, sizeof(evil));
    }

    T_CASE("CRC が壊れたフレームは捨てて次のフレームに復帰する");
    {
        reset(&rx, &cap);
        size_t n1 = cdk_proto_encode(CDK_MSG_TEXT, "broken", 6, frame, sizeof(frame));
        frame[n1 - 1] ^= 0xFF; /* CRC を壊す */
        size_t n2 = cdk_proto_encode(CDK_MSG_TEXT, "good", 4, frame2, sizeof(frame2));

        cdk_proto_rx_feed(&rx, frame, n1);
        cdk_proto_rx_feed(&rx, frame2, n2);

        T_EQ_INT(cap.count, 1);
        T_EQ_INT(cap.len[0], 4);
        T_EQ_MEM(cap.payload[0], "good", 4);
        T_CHECK(rx.stat_bad_crc >= 1);
    }

    T_CASE("payload が壊れたフレームも捨てられる");
    {
        reset(&rx, &cap);
        size_t n1 = cdk_proto_encode(CDK_MSG_TEXT, "broken", 6, frame, sizeof(frame));
        frame[CDK_PROTO_HEADER_SIZE + 2] ^= 0x01;
        size_t n2 = cdk_proto_encode(CDK_MSG_TEXT, "good", 4, frame2, sizeof(frame2));
        cdk_proto_rx_feed(&rx, frame, n1);
        cdk_proto_rx_feed(&rx, frame2, n2);
        T_EQ_INT(cap.count, 1);
        T_EQ_MEM(cap.payload[0], "good", 4);
    }

    T_CASE("len が巨大な偽ヘッダで固まらない");
    {
        reset(&rx, &cap);
        uint8_t junk[] = {CDK_PROTO_MAGIC_0, 'C', 'D', 'K', 0x99,
                          0xFF,              0xFF, 0xFF, 0xFF};
        size_t n = cdk_proto_encode(CDK_MSG_PING, "z", 1, frame, sizeof(frame));
        cdk_proto_rx_feed(&rx, junk, sizeof(junk));
        cdk_proto_rx_feed(&rx, frame, n);
        T_EQ_INT(cap.count, 1);
        T_EQ_INT(cap.type[0], CDK_MSG_PING);
    }

    T_CASE("バッファを埋め尽くすゴミからも復帰する");
    {
        reset(&rx, &cap);
        /* 全部 magic の先頭バイトにして、最悪の再同期コストを踏ませる */
        static uint8_t flood[CDK_PROTO_RX_BUF_MIN * 3];
        for (size_t i = 0; i < sizeof(flood); i++) {
            flood[i] = CDK_PROTO_MAGIC_0;
        }
        cdk_proto_rx_feed(&rx, flood, sizeof(flood));
        T_EQ_INT(cap.count, 0);

        size_t n = cdk_proto_encode(CDK_MSG_READY, "alive", 5, frame, sizeof(frame));
        cdk_proto_rx_feed(&rx, frame, n);
        T_EQ_INT(cap.count, 1);
        T_EQ_MEM(cap.payload[0], "alive", 5);
    }

    T_CASE("最大長 payload");
    {
        reset(&rx, &cap);
        static uint8_t big[CDK_PROTO_MAX_PAYLOAD];
        static uint8_t out[CDK_PROTO_MAX_PAYLOAD + CDK_PROTO_OVERHEAD];
        for (size_t i = 0; i < sizeof(big); i++) {
            big[i] = (uint8_t)(i * 31u);
        }
        size_t n = cdk_proto_encode(CDK_MSG_SHOT_DATA, big, sizeof(big), out,
                                    sizeof(out));
        T_EQ_INT(n, sizeof(out));
        cdk_proto_rx_feed(&rx, out, n);
        T_EQ_INT(cap.count, 1);
        T_EQ_INT(cap.len[0], CDK_PROTO_MAX_PAYLOAD);
    }

    return t_report("test_proto");
}
