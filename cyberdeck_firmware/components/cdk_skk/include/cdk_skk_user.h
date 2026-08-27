/* cdk_skk_user — 個人辞書（確定時の学習）
 *
 * 選んだ候補を次から先頭に出す。これが無いと、よく使う語でも毎回
 * 同じ回数 Space を押すことになり、長い文章を書くのが苦痛になる。
 *
 * 中身は RAM に持ち、変更のたびに**1 行追記する**。全体を書き直さないのは、
 * 書いている途中で電源が落ちたときに辞書ごと失うのを避けるため。
 * 同じ見出しの行が複数あってよく、**後の行が勝つ**。
 *
 * 形式（UTF-8、1 行 1 エントリ、タブ区切り）:
 *
 *     # cdk skk user dictionary v1
 *     n<TAB>にほん<TAB>/日本/二本/
 *     a<TAB>かえr<TAB>/代/帰/
 *
 * 先頭の n / a は送りなし / 送りあり。SKK 本来の辞書形式は
 * 「送りあり」「送りなし」をファイル内の区画で分けるので、追記だけで
 * 育てられない。ここは行ごとに持つ独自形式にして、必要になったら
 * 変換して書き出す。
 *
 * ファイルの読み書きはここではやらない（純 C コアなので）。
 * 行を渡す / 行を受け取るコールバックだけを持つ。
 */
#ifndef CDK_SKK_USER_H
#define CDK_SKK_USER_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CDK_SKK_USER_KEY_MAX   64
#define CDK_SKK_USER_CANDS_MAX 192
#define CDK_SKK_USER_LINE_MAX  (CDK_SKK_USER_KEY_MAX + CDK_SKK_USER_CANDS_MAX + 8)

typedef struct {
    char key[CDK_SKK_USER_KEY_MAX];
    char cands[CDK_SKK_USER_CANDS_MAX];
    bool okuri;
} cdk_skk_user_entry_t;

/* 学習した 1 行を保存する。NULL なら保存しない（テストや SD 無しのとき）。 */
typedef void (*cdk_skk_user_persist_fn)(void *ctx, const char *line, size_t len);

typedef struct {
    cdk_skk_user_entry_t   *entries;
    int                     cap;
    int                     count;
    cdk_skk_user_persist_fn persist;
    void                   *ctx;
} cdk_skk_user_t;

void cdk_skk_user_init(cdk_skk_user_t *u, cdk_skk_user_entry_t *storage, int cap);
void cdk_skk_user_set_persist(cdk_skk_user_t *u, cdk_skk_user_persist_fn fn, void *ctx);

/* 保存してある 1 行を取り込む。読み込み時に使う。
 * 形式が違う行やコメント行は黙って捨てて false を返す。 */
bool cdk_skk_user_feed_line(cdk_skk_user_t *u, const char *line);

/* 見出し語の候補列を返す。無ければ NULL。 */
const char *cdk_skk_user_lookup(const cdk_skk_user_t *u, const char *key, bool okuri);

/* 確定した候補を先頭に持ってくる。persist が設定されていれば 1 行書き出す。 */
void cdk_skk_user_learn(cdk_skk_user_t *u, const char *key, bool okuri,
                        const char *cand);

/* 個人辞書と本辞書の候補列を繋ぐ。個人辞書が先、重複は落とす。
 * out に入りきらないぶんは捨てる（先頭が優先されるので実害が小さい）。 */
size_t cdk_skk_merge_cands(const char *user_cands, const char *sys_cands, char *out,
                           size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* CDK_SKK_USER_H */
