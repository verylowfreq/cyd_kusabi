/* cdk_screen — 端末のセルグリッドを RGB565 キャンバスへ描く
 *
 * ESP-IDF に依存しない純 C。実機でもホストのテストでも同じコードが動く。
 */
#ifndef CDK_SCREEN_H
#define CDK_SCREEN_H

#include "cdk_canvas.h"
#include "cdk_font.h"
#include "cdk_term.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const cdk_font_t *font;
    int origin_x;
    int origin_y;

    uint16_t palette[256]; /* xterm 256 色を RGB565 に落としたもの */
    uint16_t default_fg;
    uint16_t default_bg;

    /* SGR の太字を「明るい色」で表現する。
     * 6x12 に太字のグリフを別に持つとフラッシュが倍になるうえ、
     * 6px 幅では太らせても潰れて読めない。xterm の既定と同じ振る舞い。 */
    bool bold_is_bright;

    /* パネルの暗さを補正するガンマ。10 倍の整数で持つ（18 なら 1.8）。
     * 10（= 1.0）で無補正。`cdk_screen_set_gamma()` で変える。
     *
     * **パレットではなくセルの色に最後に掛ける。** dim（半分にする）や
     * 反転を先に済ませてから補正しないと、「論理的に半分の明るさ」ではなく
     * 「補正後の値の半分」になってしまい、dim が沈みすぎる。 */
    uint8_t gamma_x10;
    uint8_t lut5[32]; /* 5bit 成分 (R,B) の対応表 */
    uint8_t lut6[64]; /* 6bit 成分 (G) の対応表 */

    /* 文字の拡大率。1 が等倍、2 で 2 倍（展示用の大きい文字）。
     *
     * **フォントを差し替えるのではなく、12px のグリフを画素ごと広げる。**
     * 24px の日本語ビットマップフォントを別に持つと、字形もカバー範囲も
     * 変わって「その字だけ出ない」問題を新しく作る。等倍のグリフを
     * 2x2 に広げるだけなら、字形もカバー範囲もそのままで容量も増えない。
     *
     * 画面の桁数・行数はこれで変わる（480x320 なら 80x26 → 40x13）ので、
     * 変えたら cdk_term を作り直すこと。 */
    uint8_t scale;

    /* カーソルの前回位置。移動時にその行を描き直すため。 */
    int last_cx;
    int last_cy;
    bool cursor_drawn;

    /* 未確定文字列（日本語入力のプリエディット）。
     *
     * **セルグリッドの上に重ねて描く処理をここに置くのは意図的。**
     * 実機側だけで別に描くと、ホストの基準レンダラ (cdk_render) が
     * 同じ絵を作れなくなり、`shot --verify` の突き合わせから外れる。
     * ▽ ▼ * は mkfont.py が半角として自前生成している字なので、
     * 桁ずれが起きるならまさにここ。検証の外に出したくない。 */
    char preedit[128];
    bool preedit_changed;
    int  preedit_row;   /* 前回描いた行。消すときにダーティにする */
    int  preedit_cells; /* 前回描いた桁数 */
    bool preedit_drawn;
} cdk_screen_t;

void cdk_screen_init(cdk_screen_t *s, const cdk_font_t *font);

/* パネルの暗さを補正するガンマを設定する。gamma_x10 は 10 倍の整数
 * （18 なら 1.8）。10 以下は無補正として扱う。
 *
 * 0 と最大値は動かさないので、黒は黒のまま、白は白のまま。中間だけ持ち上がる。
 *
 * **中身は整数演算だけで書いてある。** `powf` を使うとホストと実機で
 * 最終ビットが揃う保証が無く、`shot --verify` の完全一致が
 * たまに崩れることになる。整数なら同じ C コードが必ず同じ値を出す。 */
void cdk_screen_set_gamma(cdk_screen_t *s, uint8_t gamma_x10);

/* 文字の拡大率。1 か 2。**桁数と行数が変わる**ので、これを変えたら
 * cdk_screen_cols/rows を取り直して cdk_term を作り直すこと。 */
void cdk_screen_set_scale(cdk_screen_t *s, uint8_t scale);

/* 拡大率込みのセルの大きさ。桁数の計算と重ね描きの位置合わせに使う。 */
int cdk_screen_cell_w(const cdk_screen_t *s);
int cdk_screen_cell_h(const cdk_screen_t *s);

/* キャンバスに収まる桁数・行数。ステータス帯のぶんは呼び側で引くこと。 */
int cdk_screen_cols(const cdk_screen_t *s, int canvas_w);
int cdk_screen_rows(const cdk_screen_t *s, int canvas_h);

/* ダーティ行だけを描き、端末側のダーティを消す。
 * all を立てると全行を描く。 */
void cdk_screen_draw(cdk_screen_t *s, cdk_canvas_t *cv, cdk_term_t *t, bool all);

/* 未確定文字列を差し替える。NULL か "" で消える。
 * カーソル位置から反転表示で重なる。右端に収まらないときは左へ寄せる。 */
void cdk_screen_set_preedit(cdk_screen_t *s, const char *utf8);

/* 1 セルを描く。テストと部分更新用。 */
void cdk_screen_draw_cell(const cdk_screen_t *s, cdk_canvas_t *cv,
                          const cdk_cell_t *cell, int col, int row, bool cursor);

#ifdef __cplusplus
}
#endif

#endif /* CDK_SCREEN_H */
