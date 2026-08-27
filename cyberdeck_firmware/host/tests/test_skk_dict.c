/* SKK 辞書バイナリの読み出しテスト。
 *
 * 辞書は host/fixtures/skk/mini.jisyo から tools/skk_build.py が作る。
 * 索引の並べ直しと二分探索が噛み合っているかを見るのが主目的なので、
 * 見出し語は少数でよいが「UTF-8 バイト順で隣り合う語」は要る。
 */
#include "cdk_skk_dict.h"
#include "t.h"

#include <stdio.h>
#include <stdlib.h>

/* ---- メモリ上の辞書を読むコールバック ---- */

typedef struct {
    const uint8_t *data;
    size_t         len;
    int            reads; /* 何回読んだか。二分探索の回数を見るため */
} membuf_t;

static int mem_read(void *ctx, uint32_t off, void *buf, uint32_t len)
{
    membuf_t *m = (membuf_t *)ctx;
    m->reads++;
    if ((size_t)off + len > m->len) {
        return -1;
    }
    memcpy(buf, m->data + off, len);
    return 0;
}

static uint8_t  DICT[1 << 16];
static size_t   DICT_LEN;
static membuf_t MEM;
static cdk_skk_dict_t D;

static bool load(void)
{
    FILE *f = fopen(CDK_SKK_DICT_PATH, "rb");
    if (f == NULL) {
        printf("   辞書が開けません: %s\n", CDK_SKK_DICT_PATH);
        return false;
    }
    DICT_LEN = fread(DICT, 1, sizeof(DICT), f);
    fclose(f);
    MEM.data = DICT;
    MEM.len = DICT_LEN;
    MEM.reads = 0;
    return DICT_LEN > 32;
}

static const char *lookup(const char *key, bool ari)
{
    static char out[CDK_SKK_CAND_MAX];
    if (!cdk_skk_dict_lookup(&D, key, ari, out, sizeof(out))) {
        strcpy(out, "(なし)");
    }
    return out;
}

static void test_open(void)
{
    T_CASE("ヘッダを読む");
    T_CHECK(cdk_skk_dict_open(&D, mem_read, &MEM));
    T_EQ_INT(D.ari_count, 3);
    T_EQ_INT(D.nasi_count, 7);
    T_CHECK(D.annotations);
    T_CHECK(D.ready);

    T_CASE("壊れたヘッダは弾く");
    uint8_t bad[64];
    memcpy(bad, DICT, sizeof(bad));
    bad[0] = 'X';
    membuf_t bm = {bad, sizeof(bad), 0};
    cdk_skk_dict_t bd;
    T_CHECK(!cdk_skk_dict_open(&bd, mem_read, &bm));
    T_CHECK(!bd.ready);

    /* 索引の件数とレコードの先頭が合わない辞書。ここを通すと
     * 二分探索が見当違いの場所を読んで、静かに別の語を返す。 */
    T_CASE("索引の大きさが合わない辞書は弾く");
    memcpy(bad, DICT, sizeof(bad));
    bad[8] = (uint8_t)(bad[8] + 1); /* ari_count を 1 増やす */
    membuf_t bm2 = {bad, sizeof(bad), 0};
    T_CHECK(!cdk_skk_dict_open(&bd, mem_read, &bm2));
}

static void test_lookup(void)
{
    T_CHECK(cdk_skk_dict_open(&D, mem_read, &MEM));

    T_CASE("送りなしを引く");
    T_EQ_STR(lookup("にほん", false), "/日本/二本/");
    T_EQ_STR(lookup("にほんご", false), "/日本語/");
    T_EQ_STR(lookup("かんじ", false), "/漢字/感じ/幹事/");
    T_EQ_STR(lookup("あい", false), "/愛/相/藍;植物/");
    T_EQ_STR(lookup("とうきょう", false), "/東京/");
    T_EQ_STR(lookup("がっこう", false), "/学校/");
    T_EQ_STR(lookup("さいばーでっき", false), "/サイバーデッキ/");

    T_CASE("送りありを引く");
    T_EQ_STR(lookup("おくr", true), "/送/贈/");
    T_EQ_STR(lookup("たべr", true), "/食/");
    T_EQ_STR(lookup("かえr", true), "/帰/変/代;かわる/");

    /* 索引を取り違えると「見つからない」ではなく別の語が返る。
     * 静かに間違うほうが厄介なので、両方向で確かめる。 */
    T_CASE("送りあり / なし の索引は混ざらない");
    T_EQ_STR(lookup("おくr", false), "(なし)");
    T_EQ_STR(lookup("にほん", true), "(なし)");

    T_CASE("無い語");
    T_EQ_STR(lookup("そんなことば", false), "(なし)");
    T_EQ_STR(lookup("", false), "(なし)");
    /* 前方一致では当たらないこと */
    T_EQ_STR(lookup("にほ", false), "(なし)");
    T_EQ_STR(lookup("にほんごが", false), "(なし)");

    T_CASE("読む回数は件数の対数で済む");
    MEM.reads = 0;
    (void)lookup("にほん", false);
    /* 7 件なら 3 回の比較で足りる。1 回の比較で索引 1 回 + 見出し 1 回。 */
    T_CHECK(MEM.reads <= 12);
}

static void test_candidates(void)
{
    char cand[CDK_SKK_CAND_MAX];
    char annot[128];

    T_CASE("候補を数える");
    T_EQ_INT(cdk_skk_cand_count("/日本/二本/"), 2);
    T_EQ_INT(cdk_skk_cand_count("/漢字/感じ/幹事/"), 3);
    T_EQ_INT(cdk_skk_cand_count("/東京/"), 1);
    T_EQ_INT(cdk_skk_cand_count(""), 0);

    T_CASE("候補を取り出す");
    T_CHECK(cdk_skk_cand_at("/日本/二本/", 0, cand, sizeof(cand), NULL, 0));
    T_EQ_STR(cand, "日本");
    T_CHECK(cdk_skk_cand_at("/日本/二本/", 1, cand, sizeof(cand), NULL, 0));
    T_EQ_STR(cand, "二本");
    T_CHECK(!cdk_skk_cand_at("/日本/二本/", 2, cand, sizeof(cand), NULL, 0));

    T_CASE("注釈は候補から切り離す");
    T_CHECK(cdk_skk_cand_at("/愛/相/藍;植物/", 2, cand, sizeof(cand), annot,
                            sizeof(annot)));
    T_EQ_STR(cand, "藍");
    T_EQ_STR(annot, "植物");

    /* 注釈が無い候補では annot を空にする。前の候補の注釈が
     * 残ると、関係ない語の説明が画面に出る。 */
    T_CHECK(cdk_skk_cand_at("/愛/相/藍;植物/", 0, cand, sizeof(cand), annot,
                            sizeof(annot)));
    T_EQ_STR(cand, "愛");
    T_EQ_STR(annot, "");
}

int main(void)
{
    if (!load()) {
        printf("skk_dict: 辞書を読めませんでした\n");
        return EXIT_FAILURE;
    }
    test_open();
    test_lookup();
    test_candidates();
    return t_report("skk_dict");
}
