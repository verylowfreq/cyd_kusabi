/* cdk_skk_romkana — ローマ字 → かな 変換
 *
 * ESP-IDF に依存しない純 C。ホストのテストと実機で同じコードが動く。
 *
 * 状態は「まだ かな にならない未確定のローマ字」だけ。
 * 1 文字入れるたびに 0 個以上の かな が出て、未確定が残ることもある。
 *
 *   k          → (出力なし)      未確定 "k"
 *   a          → "か"            未確定 ""
 *   t t a      → "った"          ("tt" で促音、"t" が残って "ta")
 *   n          → (出力なし)      未確定 "n"
 *   k          → "ん"            未確定 "k"   ← n の後ろが子音なので確定
 *
 * かな は常にひらがなで出す。カタカナ・半角カナへの変換は
 * cdk_skk_kana_to_katakana() などで別に行う。ここで持つと
 * 変換表がモードの数だけ増えるうえ、辞書引きは常にひらがなで行うため。
 */
#ifndef CDK_SKK_ROMKANA_H
#define CDK_SKK_ROMKANA_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 未確定ローマ字の最大長。"tch" のような 3 文字が最長なので 4 で足りるが、
 * 想定外の入力でも落ちないよう少し余裕を持たせる。 */
#define CDK_SKK_ROM_MAX 8

typedef struct {
    char pend[CDK_SKK_ROM_MAX + 1]; /* 未確定のローマ字。NUL 終端 */
} cdk_skk_romkana_t;

void cdk_skk_romkana_reset(cdk_skk_romkana_t *r);

/* ASCII 1 文字を入れる。
 *
 * out に確定した かな（UTF-8）を書き、その長さを返す。
 * 変換されずに素通りする文字（数字・記号の一部）もここから出る。
 *
 * consumed には「この文字を SKK が食べたか」を書く。
 * false のときは呼び出し側が別の意味（モード切替など）で処理してよい。
 *
 * out_cap は 16 バイト以上を渡すこと。
 */
size_t cdk_skk_romkana_feed(cdk_skk_romkana_t *r, char c, char *out, size_t out_cap,
                            bool *consumed);

/* 未確定のローマ字を強制的に吐き出す（確定キーを押したときなど）。
 * "n" が残っていれば "ん" になる。それ以外はローマ字のまま出る。 */
size_t cdk_skk_romkana_flush(cdk_skk_romkana_t *r, char *out, size_t out_cap);

/* 未確定のローマ字（表示用）。空なら "" を返す。 */
const char *cdk_skk_romkana_pending(const cdk_skk_romkana_t *r);

/* ひらがな → カタカナ。範囲外の文字はそのまま複写する。
 * 変換後の長さを返す（NUL 終端する）。 */
size_t cdk_skk_hira_to_kata(const char *in, size_t len, char *out, size_t out_cap);

/* ひらがな → 半角カナ。濁点・半濁点は 2 文字になる。 */
size_t cdk_skk_hira_to_hankaku(const char *in, size_t len, char *out, size_t out_cap);

/* ASCII → 全角英数。 */
size_t cdk_skk_ascii_to_zenkaku(const char *in, size_t len, char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* CDK_SKK_ROMKANA_H */
