/* 個人辞書（学習）のテスト。
 *
 * 「選んだ候補が次から先頭に出ること」と「追記だけで育てられること」を見る。
 */
#include "cdk_skk_user.h"
#include "t.h"

#include <stdio.h>
#include <stdlib.h>

static cdk_skk_user_entry_t  STORE[8];
static cdk_skk_user_t        U;

/* persist が書き出した行を溜める */
static char   SAVED[4096];
static size_t SAVED_LEN;

static void on_persist(void *ctx, const char *line, size_t len)
{
    (void)ctx;
    if (SAVED_LEN + len < sizeof(SAVED)) {
        memcpy(SAVED + SAVED_LEN, line, len);
        SAVED_LEN += len;
        SAVED[SAVED_LEN] = '\0';
    }
}

static void setup(void)
{
    cdk_skk_user_init(&U, STORE, (int)(sizeof(STORE) / sizeof(STORE[0])));
    cdk_skk_user_set_persist(&U, on_persist, NULL);
    SAVED[0] = '\0';
    SAVED_LEN = 0;
}

static const char *lookup(const char *key, bool okuri)
{
    const char *c = cdk_skk_user_lookup(&U, key, okuri);
    return (c != NULL) ? c : "(なし)";
}

static void test_learn(void)
{
    T_CASE("選んだ候補が先頭に来る");
    setup();
    cdk_skk_user_learn(&U, "にほん", false, "二本");
    T_EQ_STR(lookup("にほん", false), "/二本/");

    /* 別の候補を選んだら、そちらが先頭になり、前のものは後ろに残る */
    cdk_skk_user_learn(&U, "にほん", false, "日本");
    T_EQ_STR(lookup("にほん", false), "/日本/二本/");

    /* もう一度前のものを選べば入れ替わる */
    cdk_skk_user_learn(&U, "にほん", false, "二本");
    T_EQ_STR(lookup("にほん", false), "/二本/日本/");

    T_CASE("同じ候補を続けて選んでも重複しない");
    cdk_skk_user_learn(&U, "にほん", false, "二本");
    T_EQ_STR(lookup("にほん", false), "/二本/日本/");

    T_CASE("送りあり / なし は別のエントリ");
    setup();
    cdk_skk_user_learn(&U, "かえr", true, "代");
    cdk_skk_user_learn(&U, "かえr", false, "換え");
    T_EQ_STR(lookup("かえr", true), "/代/");
    T_EQ_STR(lookup("かえr", false), "/換え/");

    T_CASE("知らない見出しは NULL");
    T_CHECK(cdk_skk_user_lookup(&U, "しらない", false) == NULL);
    T_CHECK(cdk_skk_user_lookup(&U, "", false) == NULL);
}

static void test_persist(void)
{
    T_CASE("学習のたびに 1 行書き出す");
    setup();
    cdk_skk_user_learn(&U, "にほん", false, "日本");
    T_EQ_STR(SAVED, "n\tにほん\t/日本/\n");

    cdk_skk_user_learn(&U, "かえr", true, "代");
    T_EQ_STR(SAVED, "n\tにほん\t/日本/\na\tかえr\t/代/\n");

    T_CASE("書き出した行を読み直すと同じ状態になる");
    /* 追記だけで育てるので、同じ見出しの行が何度も出る。後の行が勝つ。 */
    char saved[4096];
    strcpy(saved, SAVED);
    strcat(saved, "n\tにほん\t/二本/日本/\n");

    setup();
    cdk_skk_user_set_persist(&U, NULL, NULL);
    char *p = saved;
    while (*p != '\0') {
        char *nl = strchr(p, '\n');
        if (nl == NULL) {
            break;
        }
        *nl = '\0';
        (void)cdk_skk_user_feed_line(&U, p);
        p = nl + 1;
    }
    T_EQ_STR(lookup("にほん", false), "/二本/日本/"); /* 後の行が勝っている */
    T_EQ_STR(lookup("かえr", true), "/代/");

    T_CASE("壊れた行は捨てる");
    T_CHECK(!cdk_skk_user_feed_line(&U, "# コメント"));
    T_CHECK(!cdk_skk_user_feed_line(&U, ""));
    T_CHECK(!cdk_skk_user_feed_line(&U, "x\tあ\t/亜/"));   /* 種別が違う */
    T_CHECK(!cdk_skk_user_feed_line(&U, "n\tあ/亜/"));      /* タブが無い */
    T_CHECK(!cdk_skk_user_feed_line(&U, "n\tあ\t亜"));      /* / で始まらない */
    T_CHECK(!cdk_skk_user_feed_line(&U, "n\t\t/亜/"));      /* 見出しが空 */
    T_CHECK(cdk_skk_user_feed_line(&U, "n\tあ\t/亜/"));     /* これは通る */
    T_EQ_STR(lookup("あ", false), "/亜/");

    T_CASE("いっぱいになったら古いものから捨てる");
    setup();
    cdk_skk_user_set_persist(&U, NULL, NULL);
    char key[32];
    for (int i = 0; i < 12; i++) { /* 容量は 8 */
        snprintf(key, sizeof(key), "か%d", i);
        cdk_skk_user_learn(&U, key, false, "候");
    }
    T_EQ_INT(U.count, 8);
    T_CHECK(cdk_skk_user_lookup(&U, "か0", false) == NULL);  /* 捨てられた */
    T_CHECK(cdk_skk_user_lookup(&U, "か11", false) != NULL); /* 残っている */
}

static void test_truncation(void)
{
    /* 実物の SKK-JISYO.L の「かえr」は注釈つきで 9 候補あり、
     * つないだ長さは個人辞書の受け皿 (192 バイト) を超える。
     *
     * ここで**選んだ候補が落ちる**と、学習が効かないのは
     * まさに候補の多い語＝いちばん学習してほしい語になる。
     * 落ちるのは後ろ側でなければならない。 */
    T_CASE("受け皿を超えても選んだ候補は先頭に残る");
    setup();
    cdk_skk_user_set_persist(&U, NULL, NULL);

    static const char *const many[] = {
        "帰", "返", "還", "孵", "反", "変", "代", "歸", "皈", "易", "換", "替",
    };
    const int n = (int)(sizeof(many) / sizeof(many[0]));

    for (int i = 0; i < n; i++) {
        cdk_skk_user_learn(&U, "かえr", true, many[i]);
        const char *c = cdk_skk_user_lookup(&U, "かえr", true);
        T_CHECK(c != NULL);
        /* 直前に選んだものが必ず先頭 */
        char head[64];
        snprintf(head, sizeof(head), "/%s/", many[i]);
        T_CHECK(strncmp(c, head, strlen(head)) == 0);
        T_CHECK(strlen(c) < CDK_SKK_USER_CANDS_MAX);
    }

    T_CASE("受け皿より長い候補そのものは捨てる");
    setup();
    cdk_skk_user_set_persist(&U, NULL, NULL);
    char huge[CDK_SKK_USER_CANDS_MAX + 32];
    memset(huge, 'X', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    cdk_skk_user_learn(&U, "ながい", false, huge);
    /* エントリはできるが候補は空。壊れた文字列を残さない。 */
    const char *c = cdk_skk_user_lookup(&U, "ながい", false);
    T_CHECK(c == NULL);
}

static void test_merge(void)
{
    char out[512];

    T_CASE("個人辞書が先、本辞書が後");
    cdk_skk_merge_cands("/二本/", "/日本/二本/", out, sizeof(out));
    T_EQ_STR(out, "/二本/日本/");

    T_CASE("個人辞書が無ければ本辞書そのまま");
    cdk_skk_merge_cands(NULL, "/日本/二本/", out, sizeof(out));
    T_EQ_STR(out, "/日本/二本/");

    T_CASE("本辞書が無ければ個人辞書そのまま");
    cdk_skk_merge_cands("/俺の造語/", NULL, out, sizeof(out));
    T_EQ_STR(out, "/俺の造語/");

    T_CASE("注釈は本辞書側のものを残す");
    /* 個人辞書は注釈を持たない。同じ語なら本辞書側を落とすので、
     * 注釈も一緒に消える。説明が欲しければ 2 番目以降で見られる。 */
    cdk_skk_merge_cands("/藍/", "/愛/相/藍;植物/", out, sizeof(out));
    T_EQ_STR(out, "/藍/愛/相/");

    T_CASE("どちらも空");
    T_EQ_INT(cdk_skk_merge_cands(NULL, NULL, out, sizeof(out)), 0);
    T_EQ_STR(out, "");
}

int main(void)
{
    test_learn();
    test_persist();
    test_truncation();
    test_merge();
    return t_report("skk_user");
}
