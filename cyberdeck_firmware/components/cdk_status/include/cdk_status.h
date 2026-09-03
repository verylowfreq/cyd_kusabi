/* cdk_status — 最下段のステータス帯の文字列を組み立てる
 *
 * ESP-IDF に依存しない純 C。組み立てだけを持ち、描くのは呼び出し側。
 *
 * **帯は端末グリッドの外にある。** 端末には 1 行少なく渡し、余った 1 行ぶんの
 * 場所へ main が直接描く。理由は 2 つ:
 *
 *  - **DECSTBM で最下行を確保しても守れない。** `vi` も `less` も `top` も
 *    自分でスクロール領域を設定するか `ESC[r` で解除して全画面を使う。
 *    SSH 先でそれらを動かすのがこの機械の主目的なので、上書きは通常動作。
 *  - グリッドの外なので折り返しが起きない。#29（ステータス行が幅を超えて
 *    画面全体がスクロールし本文が消える）の経路が構造的に無くなる。
 */
#ifndef CDK_STATUS_H
#define CDK_STATUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool        wifi_up;
    const char *ip;   /* NULL か "" なら出さない */
    /* 繋ぎに行っている最中か。**IP より弱い。** 繋がったあとに切れて
     * 繋ぎ直している間も立つので、両方立っていたら IP を出す
     * （古いフラグのせいで生きている IP を隠さない）。 */
    bool        wifi_connecting;
    bool        sd_ok;
    uint32_t    sd_mb;
    const char *ime;  /* かな入力の状態表示。NULL なら出さない */
    bool        kbd;  /* USB キーボードがつながっているか */
    bool        ssh_up;
    /* スクロールバックを何行遡っているか。0 なら出さない。
     * **いちばん前に出す。** 遡っている間は画面が「いまの状態」ではないので、
     * それが分からないと固まったように見える。40 桁でも落ちない位置に置く。 */
    int         scroll_back;
    int         batt_mv; /* 0 なら出さない */
} cdk_status_t;

/* ステータス帯の文字列を作る。**表示桁数で cols に収める。**
 *
 * 収まらないときは後ろから項目ごと落とす（途中でちょん切らない）。
 * 桁数は `cdk_char_width` で数える。バイト数で数えると、日本語が入った
 * 瞬間にずれる（#21 と同じ壊れ方）。
 *
 * 戻り値は out に書いたバイト数。 */
size_t cdk_status_format(const cdk_status_t *st, int cols, char *out, size_t cap);

/* 文字列の表示桁数。クリップの確認用に公開しておく。 */
int cdk_status_width(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* CDK_STATUS_H */
