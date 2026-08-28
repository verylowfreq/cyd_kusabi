/* cdk_canvas — RGB565 のオフスクリーンキャンバス
 *
 * ESP-IDF に依存しない純 C。
 *
 * 方針: 画面の内容はここが唯一の真実。液晶からは絶対に読み戻さない。
 * すべての描画はキャンバスに対して行い、変更のあった行帯だけを
 * パネルへ転送する。スクリーンショットはこのバッファのダンプ。
 * 同じコードをホスト側テストでも動かして、実機なしで描画を検証する。
 */
#ifndef CDK_CANVAS_H
#define CDK_CANVAS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* キャンバス内部は常にネイティブエンディアンの u16 で RGB565。
 * パネルへの転送時のバイト順は port_display が面倒を見る。 */
static inline uint16_t cdk_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r & 0xF8u) << 8) |
                      ((uint16_t)(g & 0xFCu) << 3) |
                      ((uint16_t)(b) >> 3));
}

#define CDK_BLACK   cdk_rgb565(0x00, 0x00, 0x00)
#define CDK_WHITE   cdk_rgb565(0xFF, 0xFF, 0xFF)
#define CDK_RED     cdk_rgb565(0xFF, 0x00, 0x00)
#define CDK_GREEN   cdk_rgb565(0x00, 0xFF, 0x00)
#define CDK_BLUE    cdk_rgb565(0x00, 0x00, 0xFF)
#define CDK_YELLOW  cdk_rgb565(0xFF, 0xFF, 0x00)
#define CDK_MAGENTA cdk_rgb565(0xFF, 0x00, 0xFF)
#define CDK_CYAN    cdk_rgb565(0x00, 0xFF, 0xFF)

typedef struct {
    uint16_t *pix; /* w * h 要素。所有権は呼び出し側 */
    int       w;
    int       h;
    /* 未転送の行範囲 [dirty_y0, dirty_y1)。空のとき y0 >= y1。 */
    int dirty_y0;
    int dirty_y1;
} cdk_canvas_t;

/* pix は w*h*2 バイト以上。成功で true。 */
bool cdk_canvas_init(cdk_canvas_t *c, uint16_t *pix, int w, int h);

static inline size_t cdk_canvas_bytes(int w, int h)
{
    return (size_t)w * (size_t)h * sizeof(uint16_t);
}

/* --- ダーティ行管理 --- */
void cdk_canvas_mark_dirty(cdk_canvas_t *c, int y0, int y1);
void cdk_canvas_mark_all_dirty(cdk_canvas_t *c);
/* 転送すべき行範囲を返す。無ければ false。 */
bool cdk_canvas_dirty(const cdk_canvas_t *c, int *y0, int *y1);
void cdk_canvas_dirty_clear(cdk_canvas_t *c);

/* --- 描画。すべてクリップ済み。範囲外は黙って無視する。 --- */
void cdk_canvas_clear(cdk_canvas_t *c, uint16_t color);
void cdk_canvas_fill_rect(cdk_canvas_t *c, int x, int y, int w, int h, uint16_t color);
void cdk_canvas_rect(cdk_canvas_t *c, int x, int y, int w, int h, uint16_t color);
void cdk_canvas_hline(cdk_canvas_t *c, int x, int y, int w, uint16_t color);
void cdk_canvas_vline(cdk_canvas_t *c, int x, int y, int h, uint16_t color);
void cdk_canvas_put(cdk_canvas_t *c, int x, int y, uint16_t color);
uint16_t cdk_canvas_get(const cdk_canvas_t *c, int x, int y);

/* 1bpp のビットマップを描く。行あたり (w+7)/8 バイト、MSB が左。
 * bg に CDK_CANVAS_NO_BG を渡すと 0 のビットは描かない（透過）。
 * M1 のグリフ描画がこれを使う。 */
#define CDK_CANVAS_NO_BG 0xFFFFFFFFu
void cdk_canvas_blit_1bpp(cdk_canvas_t *c, int x, int y, int w, int h,
                          const uint8_t *bits, uint32_t fg, uint32_t bg);

/* 同じものを scale 倍で描く。1 画素を scale x scale に広げるだけで、
 * 間は補間しない（ビットマップフォントは字形そのものが最終形なので、
 * なめらかにすると輪郭が濁って読みにくくなる）。
 * w, h は**元のビットマップの大きさ**。描かれるのは w*scale x h*scale。 */
void cdk_canvas_blit_1bpp_scaled(cdk_canvas_t *c, int x, int y, int w, int h,
                                 const uint8_t *bits, uint32_t fg, uint32_t bg,
                                 int scale);

/* --- M0 の検証用テストパターン ---
 * 決定的に描く。同じ w,h ならホストでも実機でも 1 バイト違わず同じ結果になること。
 * これが `cdk.py shot --verify` の基準になる。 */
void cdk_canvas_testpattern(cdk_canvas_t *c);

#ifdef __cplusplus
}
#endif

#endif /* CDK_CANVAS_H */
