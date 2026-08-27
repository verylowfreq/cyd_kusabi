/* SKK 辞書バイナリの読み出し。形式は cdk_skk_dict.h と tools/skk_build.py。 */
#include "cdk_skk_dict.h"

#include <string.h>

#define HEADER_SIZE 32
#define FLAG_ANNOTATIONS 0x0001

/* レコードの見出し語を読むときの 1 回分。見出しは実測 75 バイトまでなので
 * ふつうは 1 回で終わる。長い見出しのために継ぎ足しも用意しておく。 */
#define KEY_CHUNK 96

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

bool cdk_skk_dict_open(cdk_skk_dict_t *d, cdk_skk_read_fn read, void *ctx)
{
    memset(d, 0, sizeof(*d));
    if (read == NULL) {
        return false;
    }

    uint8_t h[HEADER_SIZE];
    if (read(ctx, 0, h, sizeof(h)) != 0) {
        return false;
    }
    if (memcmp(h, "SKKD", 4) != 0 || rd16(h + 4) != 1) {
        return false;
    }

    d->read        = read;
    d->ctx         = ctx;
    d->annotations = (rd16(h + 6) & FLAG_ANNOTATIONS) != 0;
    d->ari_count   = rd32(h + 8);
    d->nasi_count  = rd32(h + 12);
    d->index_off   = rd32(h + 16);
    d->record_off  = rd32(h + 20);
    d->record_len  = rd32(h + 24);

    /* 索引の大きさとレコードの先頭が食い違っていたら、壊れているか
     * 別の版。ここで弾かないと二分探索が見当違いの場所を読む。 */
    const uint32_t index_len = 4u * (d->ari_count + d->nasi_count);
    if (d->index_off != HEADER_SIZE || d->record_off != d->index_off + index_len) {
        return false;
    }

    d->ready = true;
    return true;
}

/* 索引の i 番目が指すレコードのオフセット（ファイル先頭から）。 */
static bool index_at(const cdk_skk_dict_t *d, uint32_t base, uint32_t i,
                     uint32_t *rec_off)
{
    uint8_t b[4];
    if (d->read(d->ctx, base + i * 4u, b, 4) != 0) {
        return false;
    }
    const uint32_t rel = rd32(b);
    if (rel >= d->record_len) {
        return false;
    }
    *rec_off = d->record_off + rel;
    return true;
}

/* レコードの見出し語を読む。NUL の次（候補列の先頭）を *cand_off に返す。 */
static bool read_key(const cdk_skk_dict_t *d, uint32_t rec_off, char *key,
                     size_t key_cap, uint32_t *cand_off)
{
    size_t used = 0;
    for (;;) {
        uint32_t want = KEY_CHUNK;
        if (rec_off + used + want > d->record_off + d->record_len) {
            want = (d->record_off + d->record_len) - (rec_off + (uint32_t)used);
        }
        if (want == 0) {
            return false;
        }
        if (used + want >= key_cap) {
            want = (uint32_t)(key_cap - used - 1);
        }
        if (want == 0) {
            return false; /* 見出しが受け皿より長い。壊れた辞書 */
        }
        if (d->read(d->ctx, rec_off + (uint32_t)used, key + used, want) != 0) {
            return false;
        }
        const void *nul = memchr(key + used, '\0', want);
        if (nul != NULL) {
            const size_t klen = (size_t)((const char *)nul - key);
            *cand_off = rec_off + (uint32_t)klen + 1u;
            return true;
        }
        used += want;
    }
}

/* 候補列を NUL まで読む。 */
static bool read_cands(const cdk_skk_dict_t *d, uint32_t off, char *out,
                       size_t out_cap)
{
    size_t used = 0;
    for (;;) {
        uint32_t want = 256;
        if (off + used + want > d->record_off + d->record_len) {
            want = (d->record_off + d->record_len) - (off + (uint32_t)used);
        }
        if (used + want >= out_cap) {
            want = (uint32_t)(out_cap - used - 1);
        }
        if (want == 0) {
            return false;
        }
        if (d->read(d->ctx, off + (uint32_t)used, out + used, want) != 0) {
            return false;
        }
        const void *nul = memchr(out + used, '\0', want);
        if (nul != NULL) {
            return true;
        }
        used += want;
    }
}

bool cdk_skk_dict_lookup(const cdk_skk_dict_t *d, const char *key, bool okuri_ari,
                         char *out, size_t out_cap)
{
    if (!d->ready || key == NULL || out == NULL || out_cap == 0) {
        return false;
    }
    out[0] = '\0';

    const uint32_t count = okuri_ari ? d->ari_count : d->nasi_count;
    if (count == 0) {
        return false;
    }
    /* 送りありの索引が先。送りなしはその後ろに続く。 */
    const uint32_t base =
        d->index_off + (okuri_ari ? 0u : d->ari_count * 4u);

    uint32_t lo = 0;
    uint32_t hi = count; /* [lo, hi) */
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2u;

        uint32_t rec_off = 0;
        if (!index_at(d, base, mid, &rec_off)) {
            return false;
        }
        char     mid_key[CDK_SKK_KEY_MAX];
        uint32_t cand_off = 0;
        if (!read_key(d, rec_off, mid_key, sizeof(mid_key), &cand_off)) {
            return false;
        }

        /* 索引は UTF-8 バイト順に並んでいる（skk_build.py が並べ直す）。
         * strcmp は符号付き char だと 0x80 以降で逆転する処理系があるので、
         * 明示的に unsigned で比べる。 */
        const unsigned char *a = (const unsigned char *)key;
        const unsigned char *b = (const unsigned char *)mid_key;
        int cmp = 0;
        while (*a != '\0' && *a == *b) {
            a++;
            b++;
        }
        cmp = (int)*a - (int)*b;

        if (cmp == 0) {
            return read_cands(d, cand_off, out, out_cap);
        }
        if (cmp < 0) {
            hi = mid;
        } else {
            lo = mid + 1u;
        }
    }
    return false;
}

int cdk_skk_cand_count(const char *cands)
{
    if (cands == NULL) {
        return 0;
    }
    int n = 0;
    const char *p = cands;
    while (*p != '\0') {
        if (*p == '/') {
            p++;
            continue;
        }
        n++;
        while (*p != '\0' && *p != '/') {
            p++;
        }
    }
    return n;
}

bool cdk_skk_cand_at(const char *cands, int index, char *out, size_t out_cap,
                     char *annot, size_t annot_cap)
{
    if (cands == NULL || out == NULL || out_cap == 0 || index < 0) {
        return false;
    }
    out[0] = '\0';
    if (annot != NULL && annot_cap > 0) {
        annot[0] = '\0';
    }

    int         n = 0;
    const char *p = cands;
    while (*p != '\0') {
        if (*p == '/') {
            p++;
            continue;
        }
        const char *start = p;
        while (*p != '\0' && *p != '/') {
            p++;
        }
        if (n == index) {
            const size_t len = (size_t)(p - start);
            /* 注釈は最初の ';' 以降。候補自体に ';' は入らない。 */
            const char  *semi = memchr(start, ';', len);
            const size_t clen = (semi != NULL) ? (size_t)(semi - start) : len;
            const size_t copy = (clen < out_cap - 1) ? clen : out_cap - 1;
            memcpy(out, start, copy);
            out[copy] = '\0';
            if (semi != NULL && annot != NULL && annot_cap > 1) {
                const size_t alen = len - clen - 1;
                const size_t acopy = (alen < annot_cap - 1) ? alen : annot_cap - 1;
                memcpy(annot, semi + 1, acopy);
                annot[acopy] = '\0';
            }
            return true;
        }
        n++;
    }
    return false;
}
