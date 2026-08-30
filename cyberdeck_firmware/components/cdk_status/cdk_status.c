#include "cdk_status.h"

#include "cdk_text.h"

#include <stdio.h>
#include <string.h>

int cdk_status_width(const char *s)
{
    if (s == NULL) {
        return 0;
    }
    int    w = 0;
    size_t n = strlen(s);
    size_t i = 0;
    while (i < n) {
        uint32_t cp = 0;
        bool     ok = false;
        const size_t used = cdk_utf8_decode((const uint8_t *)s + i, n - i, &cp, &ok);
        if (used == 0) {
            break;
        }
        i += used;
        if (ok) {
            w += cdk_char_width(cp);
        }
    }
    return w;
}

/* 1 項目ぶんを追記する。入りきらないなら**何も足さずに** false を返す。
 * 途中でちょん切ると意味が変わる（"192.168.1.9" が別のアドレスに見える）。 */
static bool push(char *out, size_t cap, size_t *len, int *w, int cols, const char *seg)
{
    const int sw = cdk_status_width(seg);
    if (*w + sw > cols) {
        return false;
    }
    const size_t sl = strlen(seg);
    if (*len + sl + 1u > cap) {
        return false;
    }
    memcpy(out + *len, seg, sl);
    *len += sl;
    out[*len] = '\0';
    *w += sw;
    return true;
}

size_t cdk_status_format(const cdk_status_t *st, int cols, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (st == NULL || cols <= 0) {
        return 0;
    }

    size_t len = 0;
    int    w = 0;
    char   seg[64];

    /* 前にあるものほど大事。入らない項目は後ろから落ちる。 */

    /* 遡っているときは何より先に出す。画面が「いまの状態」でないことが
     * 分からないと、固まったように見える。 */
    if (st->scroll_back > 0) {
        snprintf(seg, sizeof(seg), "^%d ", st->scroll_back);
        (void)push(out, cap, &len, &w, cols, seg);
    }

    if (st->wifi_up && st->ip != NULL && st->ip[0] != '\0') {
        snprintf(seg, sizeof(seg), "%s", st->ip);
    } else {
        snprintf(seg, sizeof(seg), "WiFi--");
    }
    (void)push(out, cap, &len, &w, cols, seg);

    if (st->ssh_up) {
        (void)push(out, cap, &len, &w, cols, " SSH");
    }

    if (st->ime != NULL && st->ime[0] != '\0') {
        snprintf(seg, sizeof(seg), " %s", st->ime);
        (void)push(out, cap, &len, &w, cols, seg);
    }

    if (st->sd_ok) {
        if (st->sd_mb >= 1024u) {
            snprintf(seg, sizeof(seg), " SD%luG", (unsigned long)(st->sd_mb / 1024u));
        } else {
            snprintf(seg, sizeof(seg), " SD%luM", (unsigned long)st->sd_mb);
        }
    } else {
        snprintf(seg, sizeof(seg), " SD--");
    }
    (void)push(out, cap, &len, &w, cols, seg);

    if (st->kbd) {
        (void)push(out, cap, &len, &w, cols, " KBD");
    }

    if (st->batt_mv > 0) {
        snprintf(seg, sizeof(seg), " %d.%02dV", st->batt_mv / 1000,
                 (st->batt_mv % 1000) / 10);
        (void)push(out, cap, &len, &w, cols, seg);
    }

    return len;
}
