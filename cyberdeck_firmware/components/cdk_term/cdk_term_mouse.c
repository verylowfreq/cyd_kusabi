/* Shift+PgUp / PgDn を相手のアプリへどう渡すか。
 *
 * この機械にマウスは無い。SSH 先の全画面アプリ（claude / codex など）は
 * スクロールをマウスのホイールで受けることが多いので、Shift+PgUp / PgDn を
 * ホイールの報告に置き換えて送る。判断と符号化だけを持ち、送るのは main。
 */
#include "cdk_term.h"

#include <stdio.h>

cdk_page_route_t cdk_term_page_route(const cdk_term_t *t)
{
    if (t->mouse_mode != 0) {
        return CDK_PAGE_WHEEL;
    }
    if (t->alt_active) {
        return CDK_PAGE_KEY;
    }
    return CDK_PAGE_LOCAL;
}

int cdk_term_wheel_steps(const cdk_term_t *t)
{
    const int n = t->rows / 3;
    return (n < 1) ? 1 : n;
}

size_t cdk_term_wheel_report(const cdk_term_t *t, bool up, uint8_t *out, size_t cap)
{
    if (t->mouse_mode == 0) {
        return 0;
    }

    /* ボタン番号 64 = ホイール上、65 = 下。座標は 1 始まり。
     *
     * **位置は横は中央、縦は上から 1/4。** アプリは「ポインタの下の区画」を
     * 動かすことがある。入力欄はたいてい下にあり、codex は入力欄と
     * その上 2 行ほどではホイールに反応しない（2026-10-31、PC の SSH で確認）。
     * 40x12 では中央（7 行目）がそこに入って効かなかった。
     * 最上段にしないのは、見出しや枠を置くアプリがあるから。 */
    const int btn = up ? 64 : 65;
    const int x = t->cols / 2 + 1;
    const int y = (t->rows / 4 < 1) ? 1 : t->rows / 4;

    int n;
    switch (t->mouse_enc) {
    case 1006: /* SGR */
        n = snprintf((char *)out, cap, "\033[<%d;%d;%dM", btn, x, y);
        break;
    case 1015: /* urxvt */
        n = snprintf((char *)out, cap, "\033[%d;%d;%dM", 32 + btn, x, y);
        break;
    default:
        /* 既定 (X10 形式) と 1005 (UTF-8)。値が 128 未満なら同じバイト列。
         * この機械の桁数・行数ならそこに収まる。 */
        if (32 + x >= 128 || 32 + y >= 128) {
            return 0;
        }
        if (cap < 6) {
            return 0;
        }
        out[0] = 0x1B;
        out[1] = '[';
        out[2] = 'M';
        out[3] = (uint8_t)(32 + btn);
        out[4] = (uint8_t)(32 + x);
        out[5] = (uint8_t)(32 + y);
        return 6;
    }
    if (n < 0 || (size_t)n >= cap) {
        return 0;
    }
    return (size_t)n;
}
