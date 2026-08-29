#include "cdk_sshkey.h"

#include <string.h>

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t cdk_b64_encode(const uint8_t *in, size_t n, char *out, size_t cap)
{
    if (in == NULL || out == NULL) {
        return 0;
    }
    const size_t need = ((n + 2u) / 3u) * 4u;
    if (cap < need + 1u) {
        return 0;
    }

    size_t o = 0;
    size_t i = 0;
    while (i + 3u <= n) {
        const uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) |
                           (uint32_t)in[i + 2];
        out[o++] = B64[(v >> 18) & 0x3Fu];
        out[o++] = B64[(v >> 12) & 0x3Fu];
        out[o++] = B64[(v >> 6) & 0x3Fu];
        out[o++] = B64[v & 0x3Fu];
        i += 3u;
    }
    const size_t rem = n - i;
    if (rem == 1u) {
        const uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = B64[(v >> 18) & 0x3Fu];
        out[o++] = B64[(v >> 12) & 0x3Fu];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2u) {
        const uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++] = B64[(v >> 18) & 0x3Fu];
        out[o++] = B64[(v >> 12) & 0x3Fu];
        out[o++] = B64[(v >> 6) & 0x3Fu];
        out[o++] = '=';
    }
    out[o] = '\0';
    return o;
}

/* SSH の「文字列」を 1 つ書く。4 バイトのビッグエンディアン長 + 中身。 */
static size_t put_str(uint8_t *out, size_t cap, size_t at, const void *data, size_t len)
{
    if (at + 4u + len > cap) {
        return 0;
    }
    out[at + 0] = (uint8_t)((len >> 24) & 0xFFu);
    out[at + 1] = (uint8_t)((len >> 16) & 0xFFu);
    out[at + 2] = (uint8_t)((len >> 8) & 0xFFu);
    out[at + 3] = (uint8_t)(len & 0xFFu);
    memcpy(out + at + 4u, data, len);
    return at + 4u + len;
}

size_t cdk_sshkey_blob_p256(const uint8_t *q, size_t q_len, uint8_t *out, size_t cap)
{
    static const char TYPE[] = "ecdsa-sha2-nistp256";
    static const char CURVE[] = "nistp256";

    if (q == NULL || out == NULL || q_len != CDK_SSHKEY_P256_Q_LEN) {
        return 0;
    }
    /* 非圧縮点でなければ組まない。圧縮点を渡されて黙って通すと、
     * サーバー側で「鍵は合っているのに弾かれる」ことになる。 */
    if (q[0] != 0x04u) {
        return 0;
    }

    size_t at = put_str(out, cap, 0, TYPE, sizeof(TYPE) - 1u);
    if (at == 0) {
        return 0;
    }
    at = put_str(out, cap, at, CURVE, sizeof(CURVE) - 1u);
    if (at == 0) {
        return 0;
    }
    at = put_str(out, cap, at, q, q_len);
    return at;
}

size_t cdk_sshkey_line_p256(const uint8_t *q, size_t q_len, const char *comment,
                            char *out, size_t cap)
{
    static const char TYPE[] = "ecdsa-sha2-nistp256";

    uint8_t      blob[160];
    const size_t blob_len = cdk_sshkey_blob_p256(q, q_len, blob, sizeof(blob));
    if (blob_len == 0 || out == NULL) {
        return 0;
    }

    const size_t tlen = sizeof(TYPE) - 1u;
    if (cap < tlen + 2u) {
        return 0;
    }
    memcpy(out, TYPE, tlen);
    out[tlen] = ' ';

    const size_t b64 = cdk_b64_encode(blob, blob_len, out + tlen + 1u,
                                      cap - tlen - 1u);
    if (b64 == 0) {
        return 0;
    }
    size_t o = tlen + 1u + b64;

    if (comment != NULL && comment[0] != '\0') {
        const size_t clen = strlen(comment);
        if (o + 1u + clen + 1u > cap) {
            return o; /* コメントが入らないだけ。鍵としては使える */
        }
        out[o++] = ' ';
        memcpy(out + o, comment, clen);
        o += clen;
        out[o] = '\0';
    }
    return o;
}

size_t cdk_sshkey_fingerprint(const uint8_t *digest, size_t digest_len, char *out,
                              size_t cap)
{
    static const char PREFIX[] = "SHA256:";
    if (digest == NULL || out == NULL || digest_len != 32u) {
        return 0;
    }
    const size_t plen = sizeof(PREFIX) - 1u;
    if (cap < plen + 1u) {
        return 0;
    }
    memcpy(out, PREFIX, plen);

    const size_t n = cdk_b64_encode(digest, digest_len, out + plen, cap - plen);
    if (n == 0) {
        return 0;
    }
    size_t o = plen + n;
    /* OpenSSH は詰め文字を出さない。付けたままだと ssh-keygen -l の
     * 出力と字面が合わず、突き合わせのときに混乱する。 */
    while (o > plen && out[o - 1u] == '=') {
        o--;
    }
    out[o] = '\0';
    return o;
}
