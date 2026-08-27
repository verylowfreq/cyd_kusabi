/* 個人辞書。仕様は cdk_skk_user.h。 */
#include "cdk_skk_user.h"

#include <stdio.h>
#include <string.h>

void cdk_skk_user_init(cdk_skk_user_t *u, cdk_skk_user_entry_t *storage, int cap)
{
    memset(u, 0, sizeof(*u));
    u->entries = storage;
    u->cap = cap;
    if (storage != NULL && cap > 0) {
        memset(storage, 0, sizeof(*storage) * (size_t)cap);
    }
}

void cdk_skk_user_set_persist(cdk_skk_user_t *u, cdk_skk_user_persist_fn fn, void *ctx)
{
    u->persist = fn;
    u->ctx = ctx;
}

static cdk_skk_user_entry_t *find(const cdk_skk_user_t *u, const char *key, bool okuri)
{
    for (int i = 0; i < u->count; i++) {
        if (u->entries[i].okuri == okuri && strcmp(u->entries[i].key, key) == 0) {
            return &u->entries[i];
        }
    }
    return NULL;
}

const char *cdk_skk_user_lookup(const cdk_skk_user_t *u, const char *key, bool okuri)
{
    if (u->entries == NULL || key == NULL || key[0] == '\0') {
        return NULL;
    }
    const cdk_skk_user_entry_t *e = find(u, key, okuri);
    return (e != NULL && e->cands[0] != '\0') ? e->cands : NULL;
}

/* 候補列の中に cand がそのままの形で入っているか。 */
static bool contains(const char *cands, const char *cand)
{
    const size_t n = strlen(cand);
    const char  *p = cands;
    while (*p != '\0') {
        if (*p == '/') {
            p++;
            continue;
        }
        const char *start = p;
        while (*p != '\0' && *p != '/') {
            p++;
        }
        const size_t len = (size_t)(p - start);
        /* 注釈は比べない。同じ語に別の注釈が付いていても同じ候補。 */
        const char  *semi = memchr(start, ';', len);
        const size_t clen = (semi != NULL) ? (size_t)(semi - start) : len;
        if (clen == n && memcmp(start, cand, n) == 0) {
            return true;
        }
    }
    return false;
}

/* cands の先頭に cand を置き、元からあった同じ候補は取り除く。 */
static void promote(char *cands, size_t cap, const char *cand)
{
    char   buf[CDK_SKK_USER_CANDS_MAX];
    size_t used = 0;
    buf[0] = '\0';

    const size_t n = strlen(cand);
    if (n + 3 >= cap) {
        return; /* 1 つも入らない */
    }
    buf[used++] = '/';
    memcpy(buf + used, cand, n);
    used += n;
    buf[used++] = '/';
    buf[used] = '\0';

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
        const size_t len = (size_t)(p - start);
        const char  *semi = memchr(start, ';', len);
        const size_t clen = (semi != NULL) ? (size_t)(semi - start) : len;
        if (clen == n && memcmp(start, cand, n) == 0) {
            continue; /* 先頭に置いたぶん */
        }
        if (used + len + 1 >= cap) {
            break; /* 入らないものは捨てる。先頭が優先されるので実害は小さい */
        }
        memcpy(buf + used, start, len);
        used += len;
        buf[used++] = '/';
        buf[used] = '\0';
    }
    memcpy(cands, buf, used + 1);
}

void cdk_skk_user_learn(cdk_skk_user_t *u, const char *key, bool okuri,
                        const char *cand)
{
    if (u->entries == NULL || key == NULL || cand == NULL || key[0] == '\0' ||
        cand[0] == '\0') {
        return;
    }
    if (strlen(key) >= CDK_SKK_USER_KEY_MAX) {
        return;
    }

    cdk_skk_user_entry_t *e = find(u, key, okuri);
    if (e == NULL) {
        if (u->count >= u->cap) {
            /* いっぱい。いちばん古いものを捨てる。
             * 使用頻度を数えるほどの規模ではないので順序で切る。 */
            memmove(&u->entries[0], &u->entries[1],
                    sizeof(u->entries[0]) * (size_t)(u->cap - 1));
            u->count = u->cap - 1;
        }
        e = &u->entries[u->count++];
        memset(e, 0, sizeof(*e));
        snprintf(e->key, sizeof(e->key), "%s", key);
        e->okuri = okuri;
    }
    promote(e->cands, sizeof(e->cands), cand);

    if (u->persist != NULL) {
        char line[CDK_SKK_USER_LINE_MAX];
        const int n = snprintf(line, sizeof(line), "%c\t%s\t%s\n", okuri ? 'a' : 'n',
                               e->key, e->cands);
        if (n > 0 && (size_t)n < sizeof(line)) {
            u->persist(u->ctx, line, (size_t)n);
        }
    }
}

bool cdk_skk_user_feed_line(cdk_skk_user_t *u, const char *line)
{
    if (u->entries == NULL || line == NULL) {
        return false;
    }
    if (line[0] == '#' || line[0] == ';' || line[0] == '\0') {
        return false;
    }
    if ((line[0] != 'a' && line[0] != 'n') || line[1] != '\t') {
        return false;
    }
    const bool  okuri = (line[0] == 'a');
    const char *key = line + 2;
    const char *tab = strchr(key, '\t');
    if (tab == NULL || tab == key) {
        return false;
    }
    const size_t klen = (size_t)(tab - key);
    if (klen >= CDK_SKK_USER_KEY_MAX) {
        return false;
    }

    char keybuf[CDK_SKK_USER_KEY_MAX];
    memcpy(keybuf, key, klen);
    keybuf[klen] = '\0';

    const char *cands = tab + 1;
    if (*cands != '/') {
        return false;
    }
    char cbuf[CDK_SKK_USER_CANDS_MAX];
    size_t clen = strlen(cands);
    while (clen > 0 && (cands[clen - 1] == '\n' || cands[clen - 1] == '\r')) {
        clen--;
    }
    if (clen == 0 || clen >= sizeof(cbuf)) {
        return false;
    }
    memcpy(cbuf, cands, clen);
    cbuf[clen] = '\0';

    /* 後の行が勝つ。追記だけで育てるので、同じ見出しが何度も出てよい。 */
    cdk_skk_user_entry_t *e = find(u, keybuf, okuri);
    if (e == NULL) {
        if (u->count >= u->cap) {
            memmove(&u->entries[0], &u->entries[1],
                    sizeof(u->entries[0]) * (size_t)(u->cap - 1));
            u->count = u->cap - 1;
        }
        e = &u->entries[u->count++];
        memset(e, 0, sizeof(*e));
        snprintf(e->key, sizeof(e->key), "%s", keybuf);
        e->okuri = okuri;
    }
    memcpy(e->cands, cbuf, clen + 1);
    return true;
}

size_t cdk_skk_merge_cands(const char *user_cands, const char *sys_cands, char *out,
                           size_t out_cap)
{
    size_t used = 0;
    if (out == NULL || out_cap < 2) {
        return 0;
    }
    out[0] = '\0';

    for (int pass = 0; pass < 2; pass++) {
        const char *src = (pass == 0) ? user_cands : sys_cands;
        if (src == NULL) {
            continue;
        }
        const char *p = src;
        while (*p != '\0') {
            if (*p == '/') {
                p++;
                continue;
            }
            const char *start = p;
            while (*p != '\0' && *p != '/') {
                p++;
            }
            const size_t len = (size_t)(p - start);

            /* 個人辞書に入っているものを本辞書からもう一度出さない。 */
            char one[128];
            const char  *semi = memchr(start, ';', len);
            const size_t clen = (semi != NULL) ? (size_t)(semi - start) : len;
            if (clen < sizeof(one)) {
                memcpy(one, start, clen);
                one[clen] = '\0';
                if (used > 0 && contains(out, one)) {
                    continue;
                }
            }

            if (used == 0) {
                out[used++] = '/';
                out[used] = '\0';
            }
            if (used + len + 2 >= out_cap) {
                break;
            }
            memcpy(out + used, start, len);
            used += len;
            out[used++] = '/';
            out[used] = '\0';
        }
    }
    return used;
}
