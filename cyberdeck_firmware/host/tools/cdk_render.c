/* cdk_render — 実機のスクリーンショットと突き合わせるための基準画像を吐く。
 *
 *   cdk_render testpattern <width> <height> <out.raw>
 *   cdk_render vt <input.vt> <width> <height> <out.raw> [preedit.txt]
 *
 * preedit.txt は日本語入力の未確定文字列（UTF-8）。実機の SKK が出して
 * いるものと同じ文字列を渡すと、重ね描きまで含めて比較できる。
 * 引数ではなくファイルで渡すのは、Windows の main() が argv を ANSI
 * コードページで渡してきて日本語が壊れるため。
 *
 * 出力は RGB565 リトルエンディアンの生バイト列 (width*height*2 バイト)。
 * 実機が送ってくる SHOT_DATA と 1 バイト違わず一致するはず。
 * tools/cdk.py shot --verify がこれを呼んで比較する。
 *
 * vt モードは端末エミュレータとフォント描画まで含めて再現する。
 * ホストの単体テストと実機の検証が同じ基準レンダラを使うことで、
 * 「実機だけで壊れている」ことを機械的に切り分けられる。
 */
#include "cdk_canvas.h"
#include "cdk_screen.h"
#include "cdk_term.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(void)
{
    fprintf(stderr,
            "usage:\n"
            "  cdk_render testpattern <width> <height> <out.raw>\n"
            "  cdk_render vt <input.vt> <width> <height> <out.raw> [preedit.txt]\n");
    return 2;
}

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)n + 1u);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    *len = fread(buf, 1, (size_t)n, f);
    fclose(f);
    return buf;
}

static int write_raw(const char *path, const cdk_canvas_t *c)
{
    const size_t npix = (size_t)c->w * (size_t)c->h;
    unsigned char *out = (unsigned char *)malloc(npix * 2u);
    if (out == NULL) {
        return 1;
    }
    /* ホストのエンディアンに依存しないよう明示的にリトルエンディアンで組む */
    for (size_t i = 0; i < npix; i++) {
        out[i * 2 + 0] = (unsigned char)(c->pix[i] & 0xFFu);
        out[i * 2 + 1] = (unsigned char)((c->pix[i] >> 8) & 0xFFu);
    }
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        perror("cdk_render: fopen");
        free(out);
        return 1;
    }
    const size_t wrote = fwrite(out, 1, npix * 2u, f);
    fclose(f);
    free(out);
    if (wrote != npix * 2u) {
        fprintf(stderr, "cdk_render: 書き込みが途中で終わりました\n");
        return 1;
    }
    return 0;
}

static int render_testpattern(int w, int h, const char *out)
{
    uint16_t *pix = (uint16_t *)malloc(cdk_canvas_bytes(w, h));
    if (pix == NULL) {
        return 1;
    }
    cdk_canvas_t c;
    if (!cdk_canvas_init(&c, pix, w, h)) {
        free(pix);
        return 1;
    }
    cdk_canvas_testpattern(&c);
    const int rc = write_raw(out, &c);
    free(pix);
    return rc;
}

static uint8_t g_gamma_x10 = 10; /* 10 = 無補正 */
static uint8_t g_scale = 1;      /* 文字の拡大率 */
static int     g_status_h = 0;   /* ステータス帯の高さ（画素） */

static int render_vt(const char *in_path, int w, int h, const char *out,
                     const char *preedit_path)
{
    size_t font_len = 0;
    uint8_t *font_blob = slurp(CDK_FONT_BLOB_PATH, &font_len);
    if (font_blob == NULL) {
        fprintf(stderr, "cdk_render: フォントを開けません: %s\n", CDK_FONT_BLOB_PATH);
        return 1;
    }
    cdk_font_t font;
    if (!cdk_font_open(&font, font_blob, font_len)) {
        fprintf(stderr, "cdk_render: フォントブロブが壊れています\n");
        free(font_blob);
        return 1;
    }

    size_t in_len = 0;
    uint8_t *in = slurp(in_path, &in_len);
    if (in == NULL) {
        fprintf(stderr, "cdk_render: 入力を開けません: %s\n", in_path);
        free(font_blob);
        return 1;
    }

    cdk_screen_t screen;
    cdk_screen_init(&screen, &font);
    cdk_screen_set_gamma(&screen, g_gamma_x10);
    cdk_screen_set_scale(&screen, g_scale);

    if (preedit_path != NULL) {
        size_t   pre_len = 0;
        uint8_t *pre = slurp(preedit_path, &pre_len);
        if (pre == NULL) {
            fprintf(stderr, "cdk_render: 未確定文字列を開けません: %s\n", preedit_path);
            free(in);
            free(font_blob);
            return 1;
        }
        while (pre_len > 0 && (pre[pre_len - 1] == '\n' || pre[pre_len - 1] == '\r')) {
            pre_len--;
        }
        pre[pre_len] = '\0';
        cdk_screen_set_preedit(&screen, (const char *)pre);
        free(pre);
    }

    const int cols = cdk_screen_cols(&screen, w);
    /* **実機と同じ行数にする。** 実機は帯のぶん 1 行少ない端末を作って
     * いるので、ここが 1 行多いと折り返しとスクロールがずれ、
     * 本文だけを比べても食い違う。 */
    const int rows = cdk_screen_rows(&screen, h - g_status_h);
    if (cols <= 0 || rows <= 0) {
        fprintf(stderr, "cdk_render: 画面が小さすぎます\n");
        free(in);
        free(font_blob);
        return 1;
    }

    void *tmem = malloc(cdk_term_mem_size(cols, rows));
    cdk_term_t term;
    if (tmem == NULL ||
        !cdk_term_init(&term, cols, rows, tmem, cdk_term_mem_size(cols, rows))) {
        fprintf(stderr, "cdk_render: 端末を初期化できません\n");
        free(tmem);
        free(in);
        free(font_blob);
        return 1;
    }
    cdk_term_feed(&term, in, in_len);

    uint16_t *pix = (uint16_t *)malloc(cdk_canvas_bytes(w, h));
    cdk_canvas_t canvas;
    if (pix == NULL || !cdk_canvas_init(&canvas, pix, w, h)) {
        free(pix);
        free(tmem);
        free(in);
        free(font_blob);
        return 1;
    }
    /* 端末が使わない余白は背景色で塗っておく（実機と揃える） */
    cdk_canvas_clear(&canvas, screen.default_bg);
    cdk_screen_draw(&screen, &canvas, &term, true);

    const int rc = write_raw(out, &canvas);
    free(pix);
    free(tmem);
    free(in);
    free(font_blob);
    return rc;
}

int main(int argc, char **argv)
{
    /* --gamma=N / --scale=N を先に抜き取る。位置引数の並びを崩さないため。
     *
     * **実機と同じ値でここも描かないと `shot --verify` が落ちる。**
     * ガンマも文字の拡大率も実機側で変えられるようにしてあるので、
     * 基準画像がその値に追随しないと「実機が壊れた」ように見える。
     * cdk.py が SHOT_INFO で受け取った値をそのまま渡している。 */
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--scale=", 8) == 0) {
            g_scale = (uint8_t)atoi(argv[i] + 8);
        } else if (strncmp(argv[i], "--status-h=", 11) == 0) {
            g_status_h = atoi(argv[i] + 11);
        } else if (strncmp(argv[i], "--gamma=", 8) == 0) {
            g_gamma_x10 = (uint8_t)atoi(argv[i] + 8);
        } else {
            continue;
        }
        {
            for (int k = i; k < argc - 1; k++) {
                argv[k] = argv[k + 1];
            }
            argc--;
            i--;
        }
    }

    if (argc < 2) {
        return usage();
    }

    if (strcmp(argv[1], "testpattern") == 0 && argc == 5) {
        const int w = atoi(argv[2]);
        const int h = atoi(argv[3]);
        if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
            fprintf(stderr, "cdk_render: 解像度が不正です: %dx%d\n", w, h);
            return 2;
        }
        return render_testpattern(w, h, argv[4]);
    }

    if (strcmp(argv[1], "vt") == 0 && (argc == 6 || argc == 7)) {
        const int w = atoi(argv[3]);
        const int h = atoi(argv[4]);
        if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
            fprintf(stderr, "cdk_render: 解像度が不正です: %dx%d\n", w, h);
            return 2;
        }
        return render_vt(argv[2], w, h, argv[5], (argc == 7) ? argv[6] : NULL);
    }

    return usage();
}
