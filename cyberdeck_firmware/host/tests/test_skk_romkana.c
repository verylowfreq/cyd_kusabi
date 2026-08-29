/* ローマ字かな変換の単体テスト。
 *
 * 「入力した文字列 → 出た かな」だけを見る。表の中身や未確定の持ち方は
 * 実装の都合で変わってよいが、この対応表は仕様だから。
 */
#include "cdk_skk_romkana.h"
#include "t.h"

static cdk_skk_romkana_t R;

/* 文字列を 1 文字ずつ流し込んで、出た かな を全部つなげて返す。
 * 未確定は残したまま。 */
static const char *type(const char *s)
{
    static char acc[512];
    size_t used = 0;
    acc[0] = '\0';

    cdk_skk_romkana_reset(&R);
    for (const char *p = s; *p != '\0'; p++) {
        char out[32];
        bool consumed = false;
        const size_t n = cdk_skk_romkana_feed(&R, *p, out, sizeof(out), &consumed);
        if (n > 0 && used + n < sizeof(acc)) {
            memcpy(acc + used, out, n);
            used += n;
            acc[used] = '\0';
        }
    }
    return acc;
}

/* 打ち切って未確定も吐き出させる。 */
static const char *type_flush(const char *s)
{
    static char acc[512];
    size_t used = 0;
    const char *body = type(s);
    strcpy(acc, body);
    used = strlen(acc);

    char out[32];
    const size_t n = cdk_skk_romkana_flush(&R, out, sizeof(out));
    if (n > 0 && used + n < sizeof(acc)) {
        memcpy(acc + used, out, n);
        used += n;
        acc[used] = '\0';
    }
    return acc;
}

static void test_basic(void)
{
    T_CASE("五十音");
    T_EQ_STR(type("aiueo"), "あいうえお");
    T_EQ_STR(type("kakikukeko"), "かきくけこ");
    T_EQ_STR(type("sasisuseso"), "さしすせそ");
    T_EQ_STR(type("tatituteto"), "たちつてと");
    T_EQ_STR(type("naninuneno"), "なにぬねの");
    T_EQ_STR(type("hahihuheho"), "はひふへほ");
    T_EQ_STR(type("mamimumemo"), "まみむめも");
    T_EQ_STR(type("yayuyo"), "やゆよ");
    T_EQ_STR(type("rarirurero"), "らりるれろ");
    T_EQ_STR(type("wawo"), "わを");
    T_EQ_STR(type("gagigugego"), "がぎぐげご");
    T_EQ_STR(type("zazizuzezo"), "ざじずぜぞ");
    T_EQ_STR(type("dadidudedo"), "だぢづでど");
    T_EQ_STR(type("babibubebo"), "ばびぶべぼ");
    T_EQ_STR(type("papipupepo"), "ぱぴぷぺぽ");

    T_CASE("ヘボン式のつづり");
    T_EQ_STR(type("shi"), "し");
    T_EQ_STR(type("chi"), "ち");
    T_EQ_STR(type("tsu"), "つ");
    T_EQ_STR(type("fu"), "ふ");
    T_EQ_STR(type("ji"), "じ");
    T_EQ_STR(type("sha"), "しゃ");
    T_EQ_STR(type("shu"), "しゅ");
    T_EQ_STR(type("sho"), "しょ");
    T_EQ_STR(type("cha"), "ちゃ");
    T_EQ_STR(type("ja"), "じゃ");

    T_CASE("拗音");
    T_EQ_STR(type("kya"), "きゃ");
    T_EQ_STR(type("kyu"), "きゅ");
    T_EQ_STR(type("kyo"), "きょ");
    T_EQ_STR(type("nyu"), "にゅ");
    T_EQ_STR(type("ryo"), "りょ");
    T_EQ_STR(type("hyaku"), "ひゃく");

    T_CASE("小書き");
    T_EQ_STR(type("xa"), "ぁ");
    T_EQ_STR(type("xtu"), "っ");
    T_EQ_STR(type("xya"), "ゃ");

    T_CASE("l と q は綴りに使わない");
    /* **意図的に外した。** `l` は英数へ戻すキー、`q` はカタカナ切り替えで、
     * 文字の役目を兼ねると打ちかけが残ったときにモードを変えられなくなる
     * （`l` がまた打ちかけになり、次の `l` が `ll` = っ を出す堂々巡り）。
     * 小書きは x 系、くぁ は kw 系で打てるので失われる字は無い。 */
    /* ここ（ローマ字層）では未知の字はそのまま素通りする。
     * **利用者がこれを見ることは無い。** `l` と `q` は上の SKK 層が
     * モード切り替えとして先に横取りするので、ここまで降りてこない。 */
    T_EQ_STR(type("la"), "lあ");
    T_EQ_STR(type("qa"), "qあ");
    T_EQ_STR(type("kwa"), "くぁ");
}

static void test_sokuon(void)
{
    T_CASE("促音");
    T_EQ_STR(type("kitte"), "きって");
    T_EQ_STR(type("gakkou"), "がっこう");
    T_EQ_STR(type("issho"), "いっしょ");
    T_EQ_STR(type_flush("hassin"), "はっしん");
    T_EQ_STR(type("zutto"), "ずっと");
    T_EQ_STR(type("happa"), "はっぱ");
    T_EQ_STR(type("bikkuri"), "びっくり");

    T_CASE("tch は っ + ち");
    T_EQ_STR(type("matchi"), "まっち");
    T_EQ_STR(type("itchi"), "いっち");

    T_CASE("n は重ねても ん");
    T_EQ_STR(type("konna"), "こんな");
    T_EQ_STR(type("annai"), "あんない");
}

static void test_hatsuon(void)
{
    T_CASE("撥音 — 後ろが子音なら確定する");
    T_EQ_STR(type("kanji"), "かんじ");
    T_EQ_STR(type("ganbaru"), "がんばる");
    T_EQ_STR(type_flush("senmon"), "せんもん");

    T_CASE("撥音 — 後ろが母音なら な行になる");
    T_EQ_STR(type("kani"), "かに");
    T_EQ_STR(type("tanuki"), "たぬき");

    T_CASE("撥音 — nn と n' で明示できる");
    T_EQ_STR(type("hon"), "ほ");     /* 末尾の n は未確定のまま */
    T_EQ_STR(type_flush("hon"), "ほん");
    T_EQ_STR(type_flush("honn"), "ほん");
    T_EQ_STR(type("hon'"), "ほん");
    T_EQ_STR(type("hon'ya"), "ほんや");
    T_EQ_STR(type("honya"), "ほにゃ"); /* n' が無いと にゃ になる */

    T_CASE("撥音 — 記号の前でも確定する");
    T_EQ_STR(type("hon."), "ほん。");
}

static void test_symbols(void)
{
    T_CASE("記号");
    T_EQ_STR(type("-"), "ー");
    T_EQ_STR(type(","), "、");
    T_EQ_STR(type("."), "。");
    T_EQ_STR(type("["), "「");
    T_EQ_STR(type("]"), "」");
    T_EQ_STR(type("z-"), "〜");
    T_EQ_STR(type("z."), "…");
    T_EQ_STR(type("zl"), "→");
    T_EQ_STR(type_flush("ra-men"), "らーめん");

    T_CASE("表に無い文字はそのまま出る");
    T_EQ_STR(type("123"), "123");
    T_EQ_STR(type("a1"), "あ1");
    T_EQ_STR(type("@"), "@");

    T_CASE("大文字は小文字として扱う（見出し開始は上位で判断する）");
    T_EQ_STR(type("KA"), "か");
}

static void test_pending(void)
{
    T_CASE("未確定のローマ字が見える");
    cdk_skk_romkana_reset(&R);
    char out[32];
    bool consumed = false;

    cdk_skk_romkana_feed(&R, 'k', out, sizeof(out), &consumed);
    T_EQ_STR(cdk_skk_romkana_pending(&R), "k");
    T_CHECK(consumed);

    cdk_skk_romkana_feed(&R, 'y', out, sizeof(out), &consumed);
    T_EQ_STR(cdk_skk_romkana_pending(&R), "ky");

    const size_t n = cdk_skk_romkana_feed(&R, 'a', out, sizeof(out), &consumed);
    T_EQ_STR(out, "きゃ");
    T_EQ_INT(n, strlen("きゃ"));
    T_EQ_STR(cdk_skk_romkana_pending(&R), "");

    T_CASE("制御文字は食べない");
    cdk_skk_romkana_feed(&R, '\r', out, sizeof(out), &consumed);
    T_CHECK(!consumed);

    T_CASE("flush は未確定をローマ字のまま出す");
    cdk_skk_romkana_reset(&R);
    cdk_skk_romkana_feed(&R, 'k', out, sizeof(out), &consumed);
    const size_t m = cdk_skk_romkana_flush(&R, out, sizeof(out));
    T_EQ_STR(out, "k");
    T_EQ_INT(m, 1);
    T_EQ_STR(cdk_skk_romkana_pending(&R), "");
}

static void test_conversions(void)
{
    char out[256];

    T_CASE("ひらがな → カタカナ");
    cdk_skk_hira_to_kata("あいうえお", strlen("あいうえお"), out, sizeof(out));
    T_EQ_STR(out, "アイウエオ");
    cdk_skk_hira_to_kata("がっこう", strlen("がっこう"), out, sizeof(out));
    T_EQ_STR(out, "ガッコウ");
    /* 漢字や ASCII はそのまま通す */
    cdk_skk_hira_to_kata("あA漢", strlen("あA漢"), out, sizeof(out));
    T_EQ_STR(out, "アA漢");

    T_CASE("ひらがな → 半角カナ");
    cdk_skk_hira_to_hankaku("あいうえお", strlen("あいうえお"), out, sizeof(out));
    T_EQ_STR(out, "ｱｲｳｴｵ");
    cdk_skk_hira_to_hankaku("がっこう", strlen("がっこう"), out, sizeof(out));
    T_EQ_STR(out, "ｶﾞｯｺｳ");
    cdk_skk_hira_to_hankaku("ぱーてぃ", strlen("ぱーてぃ"), out, sizeof(out));
    T_EQ_STR(out, "ﾊﾟｰﾃｨ");
    /* カタカナで来ても同じ結果になること */
    cdk_skk_hira_to_hankaku("ガッコウ", strlen("ガッコウ"), out, sizeof(out));
    T_EQ_STR(out, "ｶﾞｯｺｳ");

    T_CASE("ASCII → 全角");
    cdk_skk_ascii_to_zenkaku("Abc 12!", strlen("Abc 12!"), out, sizeof(out));
    T_EQ_STR(out, "Ａｂｃ　１２！");
}

static void test_words(void)
{
    T_CASE("まとまった語");
    T_EQ_STR(type_flush("nihongo"), "にほんご");
    T_EQ_STR(type_flush("kyou"), "きょう");
    T_EQ_STR(type_flush("gakkou"), "がっこう");
    T_EQ_STR(type_flush("konnichiha"), "こんにちは");
    T_EQ_STR(type_flush("saibaadekki"), "さいばあでっき");
    T_EQ_STR(type_flush("shinbun"), "しんぶん");
    T_EQ_STR(type_flush("jouhou"), "じょうほう");
    T_EQ_STR(type_flush("tsukau"), "つかう");
    T_EQ_STR(type_flush("ryokou"), "りょこう");
    T_EQ_STR(type_flush("happyou"), "はっぴょう");
}

int main(void)
{
    test_basic();
    test_sokuon();
    test_hatsuon();
    test_symbols();
    test_pending();
    test_conversions();
    test_words();
    return t_report("skk_romkana");
}
