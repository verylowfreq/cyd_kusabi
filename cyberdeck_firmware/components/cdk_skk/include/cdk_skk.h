/* cdk_skk — SKK 方式の日本語入力エンジン
 *
 * ESP-IDF に依存しない純 C。「キーイベント → 確定した文字列 + 表示状態」の
 * 関数として、ホストで全部テストできる形にしてある。
 *
 * 入力の流れ（DESIGN.md §9）:
 *
 *   キー → cdk_skk_key() ┬→ 消費した (true)  … 未確定。preedit を画面に出す
 *                        └→ 消費しない(false) … 呼び出し側がそのまま流す
 *
 *   確定した文字列は commit コールバックで出る。
 *
 * SKK は「どこから漢字にするか」を人が指示する方式なので、
 * 変換範囲の推定が要らない。状態機械が小さく、全部テストできる。
 *
 *   ▽ (MIDASHI) 見出し語を入力中。大文字で始めて入る
 *   ▼ (SELECT)  候補を選択中。Space で次、x で前
 *
 * 送り仮名は ▽ の途中でもう一度大文字を打って始める。
 *   K a n g a E r u → ▽かんが*え → 辞書は "かんがe" で引く → ▼考える
 */
#ifndef CDK_SKK_H
#define CDK_SKK_H

#include "cdk_keys.h"
#include "cdk_skk_dict.h"
#include "cdk_skk_romkana.h"
#include "cdk_skk_user.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CDK_SKK_MIDASHI_MAX 96
#define CDK_SKK_OKURI_MAX   24
#define CDK_SKK_PREEDIT_MAX 256

typedef enum {
    CDK_SKK_ASCII = 0, /* 直接入力。SKK は何もしない */
    CDK_SKK_HIRAGANA,
    CDK_SKK_KATAKANA,
    CDK_SKK_HANKAKU, /* 半角カナ */
    CDK_SKK_ZENEI,   /* 全英 */
} cdk_skk_mode_t;

typedef enum {
    CDK_SKK_ST_DIRECT = 0,
    CDK_SKK_ST_MIDASHI, /* ▽ */
    CDK_SKK_ST_OKURI,   /* ▽ の送り仮名を入力中 */
    CDK_SKK_ST_SELECT,  /* ▼ */
} cdk_skk_state_t;

typedef void (*cdk_skk_commit_fn)(void *ctx, const char *utf8, size_t len);

typedef struct {
    cdk_skk_mode_t    mode;
    cdk_skk_state_t   state;
    cdk_skk_romkana_t rk;

    cdk_skk_commit_fn commit;
    void             *ctx;

    const cdk_skk_dict_t *dict;
    cdk_skk_user_t       *user; /* 個人辞書。NULL なら学習しない */

    /* いま引いている見出し（学習に使う）。 */
    char lookup_key[CDK_SKK_KEY_MAX];
    bool lookup_okuri;

    /* ▽ の中身。かな（abbrev のときは ASCII）。 */
    char midashi[CDK_SKK_MIDASHI_MAX];
    /* 送り仮名のかな。"え" など。 */
    char okuri[CDK_SKK_OKURI_MAX];
    /* 送りの頭の子音。辞書の見出しは midashi + これ。 */
    char okuri_head;
    /* abbrev (/) で入った見出しは ASCII のまま辞書を引く。 */
    bool abbrev;

    char cands[CDK_SKK_CAND_MAX];
    int  cand_index;
    int  cand_count;

    char preedit[CDK_SKK_PREEDIT_MAX];

    /* 直前の変換が辞書に無かった。呼び出し側が知らせたいときに見る。 */
    bool last_lookup_failed;
} cdk_skk_t;

void cdk_skk_init(cdk_skk_t *s, cdk_skk_commit_fn commit, void *ctx);

/* 辞書を差し替える。NULL にすると変換だけができなくなる（かな入力は動く）。 */
void cdk_skk_set_dict(cdk_skk_t *s, const cdk_skk_dict_t *dict);

/* 個人辞書を繋ぐ。確定した候補が次から先頭に出るようになる。 */
void cdk_skk_set_user(cdk_skk_t *s, cdk_skk_user_t *user);

void           cdk_skk_set_mode(cdk_skk_t *s, cdk_skk_mode_t m);
cdk_skk_mode_t cdk_skk_get_mode(const cdk_skk_t *s);

/* 入力中のものを全部捨てる。SSH が切れたときなど、
 * 画面の持ち主が変わるところで呼ぶ。 */
void cdk_skk_reset(cdk_skk_t *s);

/* キーを 1 つ処理する。true を返したら SKK が消費したので、
 * 呼び出し側はそのキーを先へ流さないこと。 */
bool cdk_skk_key(cdk_skk_t *s, const cdk_key_t *k);

/* 画面に出す未確定の文字列。何も無ければ ""。 */
const char *cdk_skk_preedit(const cdk_skk_t *s);

/* モードの短い表示。"あ" "ア" "ｱ" "Ａ" / ASCII なら ""。 */
const char *cdk_skk_mode_label(const cdk_skk_t *s);

#ifdef __cplusplus
}
#endif

#endif /* CDK_SKK_H */
