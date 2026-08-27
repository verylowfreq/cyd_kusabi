#include "cdk_proto.h"

#include <assert.h>
#include <string.h>

/* テーブル無しの reflected CRC32。フレームは高々数KBなので速度は問題にならない。 */
uint32_t cdk_crc32(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = ~crc;

    for (size_t i = 0; i < len; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++) {
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1u)));
        }
    }
    return ~c;
}

static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t get_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

size_t cdk_proto_encode(uint8_t type, const void *payload, uint32_t len,
                        void *out, size_t out_cap)
{
    if (len > CDK_PROTO_MAX_PAYLOAD) {
        return 0;
    }
    const size_t total = (size_t)len + CDK_PROTO_OVERHEAD;
    if (out_cap < total) {
        return 0;
    }
    if (len > 0 && payload == NULL) {
        return 0;
    }

    uint8_t *o = (uint8_t *)out;
    o[0] = CDK_PROTO_MAGIC_0;
    o[1] = CDK_PROTO_MAGIC_1;
    o[2] = CDK_PROTO_MAGIC_2;
    o[3] = CDK_PROTO_MAGIC_3;
    o[4] = type;
    put_u32le(o + 5, len);
    if (len > 0) {
        memcpy(o + CDK_PROTO_HEADER_SIZE, payload, len);
    }
    const uint32_t crc = cdk_crc32(0, o, CDK_PROTO_HEADER_SIZE + len);
    put_u32le(o + CDK_PROTO_HEADER_SIZE + len, crc);
    return total;
}

void cdk_proto_rx_init(cdk_proto_rx_t *rx, uint8_t *buf, size_t cap,
                       cdk_proto_frame_fn on_frame, void *user)
{
    assert(rx != NULL);
    assert(buf != NULL);
    assert(cap >= CDK_PROTO_RX_BUF_MIN);

    memset(rx, 0, sizeof(*rx));
    rx->buf = buf;
    rx->cap = cap;
    rx->on_frame = on_frame;
    rx->user = user;
}

static bool magic_at(const uint8_t *p)
{
    return p[0] == CDK_PROTO_MAGIC_0 && p[1] == CDK_PROTO_MAGIC_1 &&
           p[2] == CDK_PROTO_MAGIC_2 && p[3] == CDK_PROTO_MAGIC_3;
}

/* バッファ先頭から走査し、取り出せるフレームをすべて取り出す。
 * 戻り値: 消費してよい先頭バイト数 */
static size_t scan(cdk_proto_rx_t *rx)
{
    size_t i = 0;

    while (rx->used - i >= CDK_PROTO_MAGIC_SIZE) {
        if (!magic_at(rx->buf + i)) {
            i++;
            rx->stat_dropped++;
            continue;
        }
        if (rx->used - i < CDK_PROTO_HEADER_SIZE) {
            break; /* ヘッダがまだ足りない */
        }

        const uint32_t len = get_u32le(rx->buf + i + 5);
        if (len > CDK_PROTO_MAX_PAYLOAD) {
            /* magic に見えただけ。1 バイト捨てて探し直す。 */
            i++;
            rx->stat_dropped++;
            continue;
        }

        const size_t total = (size_t)len + CDK_PROTO_OVERHEAD;
        if (rx->used - i < total) {
            break; /* フレームがまだ揃っていない */
        }

        const uint32_t want = get_u32le(rx->buf + i + CDK_PROTO_HEADER_SIZE + len);
        const uint32_t have = cdk_crc32(0, rx->buf + i, CDK_PROTO_HEADER_SIZE + len);
        if (want != have) {
            i++;
            rx->stat_dropped++;
            rx->stat_bad_crc++;
            continue;
        }

        rx->stat_frames++;
        if (rx->on_frame != NULL) {
            rx->on_frame(rx->user, rx->buf[i + 4],
                         rx->buf + i + CDK_PROTO_HEADER_SIZE, len);
        }
        i += total;
    }

    return i;
}

static void compact(cdk_proto_rx_t *rx, size_t consumed)
{
    if (consumed == 0) {
        return;
    }
    if (consumed >= rx->used) {
        rx->used = 0;
        return;
    }
    memmove(rx->buf, rx->buf + consumed, rx->used - consumed);
    rx->used -= consumed;
}

void cdk_proto_rx_feed(cdk_proto_rx_t *rx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    while (len > 0) {
        size_t space = rx->cap - rx->used;
        if (space == 0) {
            /* バッファが埋まったのに完全なフレームが取れない = 同期外れ。
             * 先頭 1 バイトを捨てて前進する。 */
            compact(rx, 1);
            rx->stat_dropped++;
            space = rx->cap - rx->used;
        }

        const size_t n = (len < space) ? len : space;
        memcpy(rx->buf + rx->used, p, n);
        rx->used += n;
        p += n;
        len -= n;

        compact(rx, scan(rx));
    }
}
