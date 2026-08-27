/* SKK 状態機械のテスト。
 *
 * 「打った通りの文字列 → 確定した文字列 + 画面に出る未確定部分」で見る。
 * 内部の状態の持ち方は実装の都合で変わってよいが、この対応は仕様。
 *
 * 打鍵は文字列で書く。特殊キーは記号 1 文字に割り当てる:
 *   ' ' Space   '\n' Enter   '\b' BS   '\x1b' ESC   '\x0a'→ '\n' と同じ
 *   '^' の次の 1 文字は Ctrl 付き（"^j" は Ctrl-J）
 */
#include "cdk_skk.h"
#include "t.h"

#include <stdio.h>
#include <stdlib.h>

static char   COMMITTED[1024];
static size_t COMMITTED_LEN;

static void on_commit(void *ctx, const char *utf8, size_t len)
{
    (void)ctx;
    if (COMMITTED_LEN + len < sizeof(COMMITTED)) {
        memcpy(COMMITTED + COMMITTED_LEN, utf8, len);
        COMMITTED_LEN += len;
        COMMITTED[COMMITTED_LEN] = '\0';
    }
}

static cdk_skk_t      S;
static cdk_skk_dict_t D;

/* ---- 辞書（ファイルから読む） ---- */
typedef struct {
    const uint8_t *data;
    size_t         len;
} membuf_t;

static uint8_t  DICT[1 << 16];
static size_t   DICT_LEN;
static membuf_t MEM;

static int mem_read(void *ctx, uint32_t off, void *buf, uint32_t len)
{
    membuf_t *m = (membuf_t *)ctx;
    if ((size_t)off + len > m->len) {
        return -1;
    }
    memcpy(buf, m->data + off, len);
    return 0;
}

static bool load_dict(void)
{
    FILE *f = fopen(CDK_SKK_DICT_PATH, "rb");
    if (f == NULL) {
        return false;
    }
    DICT_LEN = fread(DICT, 1, sizeof(DICT), f);
    fclose(f);
    MEM.data = DICT;
    MEM.len = DICT_LEN;
    return cdk_skk_dict_open(&D, mem_read, &MEM);
}

/* ---- 打鍵 ---- */

static void press(const cdk_key_t *k)
{
    (void)cdk_skk_key(&S, k);
}

static void type(const char *seq)
{
    for (const char *p = seq; *p != '\0'; p++) {
        cdk_key_t k = {0, 0, 0};
        if (*p == '^' && p[1] != '\0') {
            p++;
            k.mods = CDK_MOD_CTRL_BIT;
            k.codepoint = (uint32_t)*p;
        } else if (*p == '\n') {
            k.keycode = CDK_K_ENTER;
        } else if (*p == '\b') {
            k.keycode = CDK_K_BS;
        } else if (*p == 0x1B) {
            k.keycode = CDK_K_ESC;
        } else if (*p == ' ') {
            k.keycode = CDK_K_SPACE;
        } else {
            k.codepoint = (uint32_t)(unsigned char)*p;
        }
        press(&k);
    }
}

/* 状態を初期化してから打つ。かなモードで始める。 */
static void start(cdk_skk_mode_t mode)
{
    cdk_skk_init(&S, on_commit, NULL);
    cdk_skk_set_dict(&S, &D);
    cdk_skk_set_mode(&S, mode);
    COMMITTED[0] = '\0';
    COMMITTED_LEN = 0;
}

static const char *run(cdk_skk_mode_t mode, const char *seq)
{
    start(mode);
    type(seq);
    return COMMITTED;
}

/* ------------------------------------------------------------------ */

static void test_mode(void)
{
    T_CASE("ASCII のときは何も食べない");
    cdk_skk_init(&S, on_commit, NULL);
    cdk_skk_set_dict(&S, &D);
    COMMITTED[0] = '\0';
    COMMITTED_LEN = 0;
    cdk_key_t a = {0, 0, 'a'};
    T_CHECK(!cdk_skk_key(&S, &a));
    T_EQ_STR(COMMITTED, "");

    T_CASE("Ctrl-J で かな入力に入る");
    cdk_key_t cj = {0, CDK_MOD_CTRL_BIT, 'j'};
    T_CHECK(cdk_skk_key(&S, &cj));
    T_EQ_INT(cdk_skk_get_mode(&S), CDK_SKK_HIRAGANA);
    T_CHECK(cdk_skk_key(&S, &a));
    T_EQ_STR(COMMITTED, "あ");

    T_CASE("l で ASCII に戻る");
    type("l");
    T_EQ_INT(cdk_skk_get_mode(&S), CDK_SKK_ASCII);
    T_CHECK(!cdk_skk_key(&S, &a));

    T_CASE("q でカタカナと往復する");
    start(CDK_SKK_HIRAGANA);
    type("q");
    T_EQ_INT(cdk_skk_get_mode(&S), CDK_SKK_KATAKANA);
    type("aiueo");
    T_EQ_STR(COMMITTED, "アイウエオ");
    type("q");
    T_EQ_INT(cdk_skk_get_mode(&S), CDK_SKK_HIRAGANA);

    T_CASE("L で全英、Ctrl-J で戻る");
    start(CDK_SKK_HIRAGANA);
    type("L");
    T_EQ_INT(cdk_skk_get_mode(&S), CDK_SKK_ZENEI);
    type("Ab1");
    T_EQ_STR(COMMITTED, "Ａｂ１");
    type("^j");
    T_EQ_INT(cdk_skk_get_mode(&S), CDK_SKK_HIRAGANA);

    T_CASE("モードの表示");
    start(CDK_SKK_HIRAGANA);
    T_EQ_STR(cdk_skk_mode_label(&S), "あ");
    start(CDK_SKK_KATAKANA);
    T_EQ_STR(cdk_skk_mode_label(&S), "ア");
    start(CDK_SKK_HANKAKU);
    T_EQ_STR(cdk_skk_mode_label(&S), "ｱ");
    start(CDK_SKK_ASCII);
    T_EQ_STR(cdk_skk_mode_label(&S), "");
}

static void test_kana(void)
{
    T_CASE("かなをそのまま打つ");
    T_EQ_STR(run(CDK_SKK_HIRAGANA, "nihongo"), "にほんご");
    T_EQ_STR(run(CDK_SKK_KATAKANA, "nihongo"), "ニホンゴ");
    T_EQ_STR(run(CDK_SKK_HANKAKU, "nihongo"), "ﾆﾎﾝｺﾞ");

    T_CASE("未確定のローマ字は画面に出る");
    start(CDK_SKK_HIRAGANA);
    type("ky");
    T_EQ_STR(cdk_skk_preedit(&S), "ky");
    T_EQ_STR(COMMITTED, "");
    type("o");
    T_EQ_STR(cdk_skk_preedit(&S), "");
    T_EQ_STR(COMMITTED, "きょ");

    T_CASE("Ctrl-J で未確定を吐き出す");
    start(CDK_SKK_HIRAGANA);
    type("hon^j");
    T_EQ_STR(COMMITTED, "ほん");

    T_CASE("BS は未確定のローマ字を消す。無ければ端末へ");
    start(CDK_SKK_HIRAGANA);
    type("ky");
    cdk_key_t bs = {CDK_K_BS, 0, 0};
    T_CHECK(cdk_skk_key(&S, &bs));
    T_EQ_STR(cdk_skk_preedit(&S), "k");
    T_CHECK(cdk_skk_key(&S, &bs));
    T_EQ_STR(cdk_skk_preedit(&S), "");
    T_CHECK(!cdk_skk_key(&S, &bs)); /* もう消すものが無い */
}

static void test_midashi(void)
{
    T_CASE("大文字で ▽ に入る");
    start(CDK_SKK_HIRAGANA);
    type("Nihon");
    /* 末尾の n はまだ「ん」か「な行」か決まらないので、ローマ字のまま見える */
    T_EQ_STR(cdk_skk_preedit(&S), "▽にほn");
    T_EQ_STR(COMMITTED, "");
    T_EQ_STR(cdk_skk_mode_label(&S), "あ");

    T_CASE("Enter で かな のまま確定する");
    type("\n");
    T_EQ_STR(COMMITTED, "にほん");
    T_EQ_STR(cdk_skk_preedit(&S), "");

    T_CASE("ESC で捨てる");
    start(CDK_SKK_HIRAGANA);
    type("Nihon\x1b");
    T_EQ_STR(COMMITTED, "");
    T_EQ_STR(cdk_skk_preedit(&S), "");

    T_CASE("▽ の中で BS は見出しを削る");
    start(CDK_SKK_HIRAGANA);
    type("Nihon");
    type("\b");
    T_EQ_STR(cdk_skk_preedit(&S), "▽にほ");
    type("\b\b");
    T_EQ_STR(cdk_skk_preedit(&S), "▽");
    type("\b");
    T_EQ_STR(cdk_skk_preedit(&S), ""); /* ▽ から抜ける */

    T_CASE("▽ の中の q はカタカナで確定する");
    T_EQ_STR(run(CDK_SKK_HIRAGANA, "Nihonq"), "ニホン");
}

static void test_convert(void)
{
    T_CASE("Space で変換して ▼ になる");
    start(CDK_SKK_HIRAGANA);
    type("Nihon ");
    T_EQ_STR(cdk_skk_preedit(&S), "▼日本");
    T_EQ_STR(COMMITTED, "");

    T_CASE("Space で次の候補");
    type(" ");
    T_EQ_STR(cdk_skk_preedit(&S), "▼二本");
    T_CASE("最後の候補で止まる");
    type(" ");
    T_EQ_STR(cdk_skk_preedit(&S), "▼二本");
    T_CASE("x で前の候補");
    type("x");
    T_EQ_STR(cdk_skk_preedit(&S), "▼日本");

    T_CASE("Enter で確定");
    type("\n");
    T_EQ_STR(COMMITTED, "日本");
    T_EQ_STR(cdk_skk_preedit(&S), "");

    T_CASE("候補が 3 つある語");
    start(CDK_SKK_HIRAGANA);
    type("Kanji ");
    T_EQ_STR(cdk_skk_preedit(&S), "▼漢字");
    type(" ");
    T_EQ_STR(cdk_skk_preedit(&S), "▼感じ");
    type(" ");
    T_EQ_STR(cdk_skk_preedit(&S), "▼幹事");
    type("\n");
    T_EQ_STR(COMMITTED, "幹事");

    T_CASE("注釈は候補に混ぜない");
    start(CDK_SKK_HIRAGANA);
    type("Ai   \n"); /* 3 回 Space で 3 番目 = 藍;植物 */
    T_EQ_STR(COMMITTED, "藍");

    T_CASE("▼ で他の文字を打つと確定して打ち直す");
    start(CDK_SKK_HIRAGANA);
    type("Nihon ");
    type("go");
    T_EQ_STR(COMMITTED, "日本ご");

    T_CASE("▼ で ESC すると ▽ に戻る");
    start(CDK_SKK_HIRAGANA);
    type("Nihon ");
    type("\x1b");
    T_EQ_STR(cdk_skk_preedit(&S), "▽にほん");
    T_EQ_STR(COMMITTED, "");

    T_CASE("辞書に無い語は かな のまま確定する");
    T_EQ_STR(run(CDK_SKK_HIRAGANA, "Sonnakotoba "), "そんなことば");
}

static void test_okuri(void)
{
    /* mini.jisyo: おくr /送/贈/  たべr /食/  かえr /帰/変/代;かわる/ */
    T_CASE("送り仮名つきの変換");
    start(CDK_SKK_HIRAGANA);
    type("OkuRu");
    T_EQ_STR(cdk_skk_preedit(&S), "▼送る");
    type("\n");
    T_EQ_STR(COMMITTED, "送る");

    T_CASE("送りの途中は ▽…* で見える");
    start(CDK_SKK_HIRAGANA);
    type("OkuR");
    T_EQ_STR(cdk_skk_preedit(&S), "▽おく*r");
    T_EQ_STR(COMMITTED, "");

    T_CASE("送りありの候補も選べる");
    start(CDK_SKK_HIRAGANA);
    type("KaeRu");
    T_EQ_STR(cdk_skk_preedit(&S), "▼帰る");
    type(" ");
    T_EQ_STR(cdk_skk_preedit(&S), "▼変る");
    type(" \n");
    T_EQ_STR(COMMITTED, "代る");

    T_CASE("送りありの見出しは送りなしの索引では引かない");
    /* "おくr" は送りあり索引にしか無い。送りなしで引いてしまうと
     * 「変換できない」ではなく別の語が出る危険がある。 */
    start(CDK_SKK_HIRAGANA);
    type("Okur ");     /* 小文字の r なので送りにならない = 見出し "おくr" */
    T_EQ_STR(COMMITTED, "おくr");

    T_CASE("送りの途中で BS すると ▽ に戻る");
    start(CDK_SKK_HIRAGANA);
    type("OkuR");
    type("\b");
    T_EQ_STR(cdk_skk_preedit(&S), "▽おく");

    T_CASE("辞書に無い送りあり語は かな のまま確定する");
    T_EQ_STR(run(CDK_SKK_HIRAGANA, "NihonSuru"), "にほんする");
}

static void test_abbrev(void)
{
    T_CASE("/ で abbrev に入りローマ字変換しない");
    start(CDK_SKK_HIRAGANA);
    type("/nihon");
    T_EQ_STR(cdk_skk_preedit(&S), "▽nihon");
    type("\n");
    T_EQ_STR(COMMITTED, "nihon");
}

static void test_reset(void)
{
    T_CASE("reset で入力中のものを捨てる");
    start(CDK_SKK_HIRAGANA);
    type("Nihon ");
    T_EQ_STR(cdk_skk_preedit(&S), "▼日本");
    cdk_skk_reset(&S);
    T_EQ_STR(cdk_skk_preedit(&S), "");
    T_EQ_STR(COMMITTED, "");
    /* モードは残る。画面の持ち主が変わっただけなので */
    T_EQ_INT(cdk_skk_get_mode(&S), CDK_SKK_HIRAGANA);
}

int main(void)
{
    if (!load_dict()) {
        printf("skk: 辞書 %s を読めませんでした\n", CDK_SKK_DICT_PATH);
        return EXIT_FAILURE;
    }
    test_mode();
    test_kana();
    test_midashi();
    test_convert();
    test_okuri();
    test_abbrev();
    test_reset();
    return t_report("skk");
}
