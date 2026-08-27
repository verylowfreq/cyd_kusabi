/* cdk_font — ビットマップフォントのブロブを読む
 *
 * ESP-IDF に依存しない純 C。ブロブは tools/mkfont.py が東雲フォント (Public Domain)
 * から生成する。実機ではフラッシュに焼き込み、ホストではファイルから読む。
 *
 * ブロブの構造は tools/mkfont.py の docstring を見ること。
 */
#ifndef CDK_FONT_H
#define CDK_FONT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t *blob;
    size_t         blob_len;

    uint8_t  cell_w;   /* 半角セルの幅 (6) */
    uint8_t  cell_h;   /* セルの高さ (12) */
    uint8_t  baseline; /* セル上端からベースラインまでの行数 (10) */
    uint32_t count;

    const uint8_t *cps;  /* u16 LE 昇順 x count */
    const uint8_t *ents; /* u32 LE x count。bit31=全角、下位=bits からのオフセット */
    const uint8_t *bits;
    uint32_t       bits_len;
    uint32_t       notdef_half;
    uint32_t       notdef_wide;
} cdk_font_t;

typedef struct {
    const uint8_t *bits;   /* cell_h 行 x stride バイト。MSB が左端。 */
    uint8_t        width;  /* ピクセル幅 (6 または 12) */
    uint8_t        stride; /* 1 行あたりのバイト数 (1 または 2) */
    bool           wide;   /* 2 セル分か */
} cdk_glyph_t;

/* ブロブを検証して開く。壊れていれば false。 */
bool cdk_font_open(cdk_font_t *f, const void *blob, size_t len);

/* cp のグリフを引く。cells は 1 (半角) か 2 (全角)。
 *
 * 見つかったグリフの幅が cells と食い違う場合は notdef を返す。
 * 端末では「桁がずれないこと」が最優先なので、グリフがあっても
 * セル数が合わなければ使ってはいけない。
 *
 * 戻り値: 本物のグリフなら true、notdef で代替したら false。
 * いずれの場合も *g は描画可能な状態になる。 */
bool cdk_font_glyph(const cdk_font_t *f, uint32_t cp, int cells, cdk_glyph_t *g);

/* グリフの (x, y) が立っているか。x は 0..width-1。 */
static inline bool cdk_glyph_pixel(const cdk_glyph_t *g, int x, int y)
{
    const uint8_t byte = g->bits[(size_t)y * g->stride + (size_t)(x >> 3)];
    return (byte >> (7 - (x & 7))) & 1u;
}

#ifdef __cplusplus
}
#endif

#endif /* CDK_FONT_H */
