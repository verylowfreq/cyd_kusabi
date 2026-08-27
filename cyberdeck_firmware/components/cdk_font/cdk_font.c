#include "cdk_font.h"

#include <string.h>

#define HDR_SIZE 48
#define ENT_WIDE 0x80000000u
#define ENT_MASK 0x7FFFFFFFu

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

bool cdk_font_open(cdk_font_t *f, const void *blob, size_t len)
{
    if (f == NULL || blob == NULL || len < HDR_SIZE) {
        return false;
    }
    const uint8_t *b = (const uint8_t *)blob;
    if (memcmp(b, "CDKF", 4) != 0) {
        return false;
    }
    if (rd16(b + 4) != 1u) {
        return false;
    }

    memset(f, 0, sizeof(*f));
    f->blob = b;
    f->blob_len = len;
    f->cell_w = b[8];
    f->cell_h = b[9];
    f->baseline = b[10];
    f->count = rd32(b + 12);

    const uint32_t cp_off = rd32(b + 16);
    const uint32_t ent_off = rd32(b + 20);
    const uint32_t bits_off = rd32(b + 24);
    const uint32_t bits_len = rd32(b + 28);
    f->notdef_half = rd32(b + 32);
    f->notdef_wide = rd32(b + 36);

    if (f->cell_w == 0 || f->cell_h == 0 || f->count == 0) {
        return false;
    }
    /* セクションがブロブに収まっているか。壊れたブロブで範囲外を読まない。 */
    if ((size_t)cp_off + (size_t)f->count * 2u > len) {
        return false;
    }
    if ((size_t)ent_off + (size_t)f->count * 4u > len) {
        return false;
    }
    if ((size_t)bits_off + (size_t)bits_len > len) {
        return false;
    }
    const uint32_t wide_glyph = (uint32_t)f->cell_h * 2u;
    if (f->notdef_half + (uint32_t)f->cell_h > bits_len ||
        f->notdef_wide + wide_glyph > bits_len) {
        return false;
    }

    f->cps = b + cp_off;
    f->ents = b + ent_off;
    f->bits = b + bits_off;
    f->bits_len = bits_len;
    return true;
}

static void set_notdef(const cdk_font_t *f, int cells, cdk_glyph_t *g)
{
    const bool wide = (cells == 2);
    g->wide = wide;
    g->stride = wide ? 2u : 1u;
    g->width = (uint8_t)(f->cell_w * (wide ? 2 : 1));
    g->bits = f->bits + (wide ? f->notdef_wide : f->notdef_half);
}

bool cdk_font_glyph(const cdk_font_t *f, uint32_t cp, int cells, cdk_glyph_t *g)
{
    if (cells != 1 && cells != 2) {
        cells = 1;
    }
    set_notdef(f, cells, g);

    if (cp > 0xFFFFu) {
        return false; /* BMP 外はグリフを持たない */
    }

    /* 昇順の u16 配列を二分探索 */
    uint32_t lo = 0;
    uint32_t hi = f->count;
    uint32_t found = f->count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2u;
        const uint16_t v = rd16(f->cps + (size_t)mid * 2u);
        if (v < cp) {
            lo = mid + 1u;
        } else if (v > cp) {
            hi = mid;
        } else {
            found = mid;
            break;
        }
    }
    if (found >= f->count) {
        return false;
    }

    const uint32_t ent = rd32(f->ents + (size_t)found * 4u);
    const bool wide = (ent & ENT_WIDE) != 0u;
    const uint32_t off = ent & ENT_MASK;

    /* セル数が食い違うグリフは使わない。桁がずれるくらいなら げた を出す。 */
    if (wide != (cells == 2)) {
        return false;
    }

    const uint32_t need = (uint32_t)f->cell_h * (wide ? 2u : 1u);
    if (off + need > f->bits_len) {
        return false; /* 壊れたブロブ */
    }

    g->wide = wide;
    g->stride = wide ? 2u : 1u;
    g->width = (uint8_t)(f->cell_w * (wide ? 2 : 1));
    g->bits = f->bits + off;
    return true;
}
