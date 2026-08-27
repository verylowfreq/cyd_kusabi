/* cdk_skk_dict — SKK 辞書バイナリの読み出し
 *
 * 形式は tools/skk_build.py が作る。索引を二分探索してレコードを読むだけで、
 * RAM 使用量は辞書の大きさに依存しない（6.5MB の SKK-JISYO.L でも同じ）。
 *
 * 読み出しは**コールバック経由**にしてある。実機ではフラッシュの
 * skkdict パーティション（`esp_partition_read`）、SD 上のファイル
 * （`fread`）、ホストのテスト（メモリ）と読み方が違うのに、
 * 探索の理屈は全く同じだから。FILE* を直接受けると実機で使えない。
 */
#ifndef CDK_SKK_DICT_H
#define CDK_SKK_DICT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 見出し語と候補列の上限。SKK-JISYO.L の実測は見出し 75 / 候補 3,219 バイト。
 * これを超えるエントリは skk_build.py が落とすので、ここで切れることはない。 */
#define CDK_SKK_KEY_MAX  128
#define CDK_SKK_CAND_MAX 4096

/* off から len バイトを buf に読む。成功なら 0、失敗なら負。 */
typedef int (*cdk_skk_read_fn)(void *ctx, uint32_t off, void *buf, uint32_t len);

typedef struct {
    cdk_skk_read_fn read;
    void           *ctx;

    uint32_t ari_count;
    uint32_t nasi_count;
    uint32_t index_off;
    uint32_t record_off;
    uint32_t record_len;
    bool     annotations;
    bool     ready;
} cdk_skk_dict_t;

/* ヘッダを読んで検証する。形式が違えば false。 */
bool cdk_skk_dict_open(cdk_skk_dict_t *d, cdk_skk_read_fn read, void *ctx);

/* 見出し語を引く。見つかれば候補列（"/A/B;注釈/" の形）を out に入れて true。
 *
 * okuri_ari を立てると送り仮名ありの索引を引く。送りあり見出しは
 * 「おくr」のように末尾に子音がつく。索引が別なので、ここを間違えると
 * 見つからないのではなく**別の語が引ける**。 */
bool cdk_skk_dict_lookup(const cdk_skk_dict_t *d, const char *key, bool okuri_ari,
                         char *out, size_t out_cap);

/* 候補列の中の候補の数。 */
int cdk_skk_cand_count(const char *cands);

/* 候補列の index 番目を取り出す。注釈は annot に分けて入れる
 * （annot が NULL なら捨てる）。範囲外なら false。 */
bool cdk_skk_cand_at(const char *cands, int index, char *out, size_t out_cap,
                     char *annot, size_t annot_cap);

#ifdef __cplusplus
}
#endif

#endif /* CDK_SKK_DICT_H */
