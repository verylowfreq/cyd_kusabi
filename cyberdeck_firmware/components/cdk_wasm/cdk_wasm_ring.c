/* リングバッファ。単一生産者・単一消費者。
 *
 * head は書く側、tail は読む側しか進めない。両方 volatile。
 * wasm タスクとメインループは同じ CPU に固定してあるので、
 * これで足りる（port_wasm.c を見よ）。満杯の判定に 1 バイト余らせて、
 * head == tail を「空」に一意に決める。 */
#include "cdk_wasm.h"

#include <assert.h>
#include <string.h>

void cdk_wasm_ring_init(cdk_wasm_ring_t *r, uint8_t *buf, int cap)
{
    assert(r && buf && cap > 1);
    r->buf  = buf;
    r->cap  = cap;
    r->head = 0;
    r->tail = 0;
}

int cdk_wasm_ring_used(const cdk_wasm_ring_t *r)
{
    const int h = r->head, t = r->tail;
    return h >= t ? h - t : h + r->cap - t;
}

int cdk_wasm_ring_free(const cdk_wasm_ring_t *r)
{
    /* 1 バイトは満杯と空を見分けるために使えない。 */
    return r->cap - 1 - cdk_wasm_ring_used(r);
}

int cdk_wasm_ring_write(cdk_wasm_ring_t *r, const void *p, int n)
{
    if (!p || n <= 0) return 0;

    int room = cdk_wasm_ring_free(r);
    if (n > room) n = room;
    if (n <= 0) return 0;

    const uint8_t *src = (const uint8_t *)p;
    int h = r->head;
    const int first = (r->cap - h < n) ? r->cap - h : n;
    memcpy(r->buf + h, src, (size_t)first);
    if (first < n) memcpy(r->buf, src + first, (size_t)(n - first));

    h += n;
    if (h >= r->cap) h -= r->cap;
    r->head = h;
    return n;
}

int cdk_wasm_ring_read(cdk_wasm_ring_t *r, void *p, int n)
{
    if (!p || n <= 0) return 0;

    int have = cdk_wasm_ring_used(r);
    if (n > have) n = have;
    if (n <= 0) return 0;

    uint8_t *dst = (uint8_t *)p;
    int t = r->tail;
    const int first = (r->cap - t < n) ? r->cap - t : n;
    memcpy(dst, r->buf + t, (size_t)first);
    if (first < n) memcpy(dst + first, r->buf, (size_t)(n - first));

    t += n;
    if (t >= r->cap) t -= r->cap;
    r->tail = t;
    return n;
}
