/* ローマ字 → かな 変換表と状態機械。詳細は cdk_skk_romkana.h。
 *
 * 表は ddskk の skk-rom-kana-base-rule-list に相当するものを
 * 「入力 → 出力 + 打ち直し」の 3 つ組で持つ。
 *
 * 促音（っ）は子音の重なりを表に直接書く。"kk" → "っ" + 打ち直し "k"。
 * 規則で書こうとすると「n は重ねても っ にならない」「ch は tch」といった
 * 例外を条件分岐で並べることになり、表で書いたほうが読める。
 *
 * 撥音（ん）だけは表で書けない。"n" の次に来る文字が母音でも y でもない
 * ときに限って確定するので、後読みが要る。ここだけ特別扱いする。
 */
#include "cdk_skk_romkana.h"

#include <string.h>

typedef struct {
    const char *roma; /* 入力（小文字） */
    const char *kana; /* 出力（ひらがな UTF-8） */
    const char *rest; /* 変換後に打ち直す残り */
} rom_rule_t;

/* 長いものが先に並ぶ必要はない。完全一致でしか引かないため。 */
static const rom_rule_t RULES[] = {
    /* 母音 */
    {"a", "あ", ""}, {"i", "い", ""}, {"u", "う", ""}, {"e", "え", ""}, {"o", "お", ""},

    /* か行 */
    {"ka", "か", ""}, {"ki", "き", ""}, {"ku", "く", ""}, {"ke", "け", ""}, {"ko", "こ", ""},
    {"kya", "きゃ", ""}, {"kyi", "きぃ", ""}, {"kyu", "きゅ", ""},
    {"kye", "きぇ", ""}, {"kyo", "きょ", ""},
    {"ca", "か", ""}, {"cu", "く", ""}, {"co", "こ", ""},
    /* くぁ〜くぉ は kw 系で打つ。**q は使わない。**
     * q は SKK ではカタカナ切り替えのキーで、文字の役目を持たせると
     * 打ちかけが残ったときにモードを変えられなくなる。 */
    {"kwa", "くぁ", ""}, {"kwi", "くぃ", ""}, {"kwu", "くぅ", ""},
    {"kwe", "くぇ", ""}, {"kwo", "くぉ", ""},

    /* が行 */
    {"ga", "が", ""}, {"gi", "ぎ", ""}, {"gu", "ぐ", ""}, {"ge", "げ", ""}, {"go", "ご", ""},
    {"gya", "ぎゃ", ""}, {"gyi", "ぎぃ", ""}, {"gyu", "ぎゅ", ""},
    {"gye", "ぎぇ", ""}, {"gyo", "ぎょ", ""},

    /* さ行 */
    {"sa", "さ", ""}, {"si", "し", ""}, {"su", "す", ""}, {"se", "せ", ""}, {"so", "そ", ""},
    {"sya", "しゃ", ""}, {"syi", "しぃ", ""}, {"syu", "しゅ", ""},
    {"sye", "しぇ", ""}, {"syo", "しょ", ""},
    {"sha", "しゃ", ""}, {"shi", "し", ""}, {"shu", "しゅ", ""},
    {"she", "しぇ", ""}, {"sho", "しょ", ""},
    {"ci", "し", ""}, {"ce", "せ", ""},

    /* ざ行 */
    {"za", "ざ", ""}, {"zi", "じ", ""}, {"zu", "ず", ""}, {"ze", "ぜ", ""}, {"zo", "ぞ", ""},
    {"zya", "じゃ", ""}, {"zyi", "じぃ", ""}, {"zyu", "じゅ", ""},
    {"zye", "じぇ", ""}, {"zyo", "じょ", ""},
    {"ja", "じゃ", ""}, {"ji", "じ", ""}, {"ju", "じゅ", ""},
    {"je", "じぇ", ""}, {"jo", "じょ", ""},
    {"jya", "じゃ", ""}, {"jyi", "じぃ", ""}, {"jyu", "じゅ", ""},
    {"jye", "じぇ", ""}, {"jyo", "じょ", ""},

    /* た行 */
    {"ta", "た", ""}, {"ti", "ち", ""}, {"tu", "つ", ""}, {"te", "て", ""}, {"to", "と", ""},
    {"tya", "ちゃ", ""}, {"tyi", "ちぃ", ""}, {"tyu", "ちゅ", ""},
    {"tye", "ちぇ", ""}, {"tyo", "ちょ", ""},
    {"cha", "ちゃ", ""}, {"chi", "ち", ""}, {"chu", "ちゅ", ""},
    {"che", "ちぇ", ""}, {"cho", "ちょ", ""},
    {"cya", "ちゃ", ""}, {"cyi", "ちぃ", ""}, {"cyu", "ちゅ", ""},
    {"cye", "ちぇ", ""}, {"cyo", "ちょ", ""},
    {"tsu", "つ", ""}, {"tsa", "つぁ", ""}, {"tsi", "つぃ", ""},
    {"tse", "つぇ", ""}, {"tso", "つぉ", ""},
    {"tha", "てゃ", ""}, {"thi", "てぃ", ""}, {"thu", "てゅ", ""},
    {"the", "てぇ", ""}, {"tho", "てょ", ""},
    {"twu", "とぅ", ""},

    /* だ行 */
    {"da", "だ", ""}, {"di", "ぢ", ""}, {"du", "づ", ""}, {"de", "で", ""}, {"do", "ど", ""},
    {"dya", "ぢゃ", ""}, {"dyi", "ぢぃ", ""}, {"dyu", "ぢゅ", ""},
    {"dye", "ぢぇ", ""}, {"dyo", "ぢょ", ""},
    {"dha", "でゃ", ""}, {"dhi", "でぃ", ""}, {"dhu", "でゅ", ""},
    {"dhe", "でぇ", ""}, {"dho", "でょ", ""},
    {"dwu", "どぅ", ""},

    /* な行 */
    {"na", "な", ""}, {"ni", "に", ""}, {"nu", "ぬ", ""}, {"ne", "ね", ""}, {"no", "の", ""},
    {"nya", "にゃ", ""}, {"nyi", "にぃ", ""}, {"nyu", "にゅ", ""},
    {"nye", "にぇ", ""}, {"nyo", "にょ", ""},
    {"n'", "ん", ""}, {"xn", "ん", ""},
    /* "nn" はここに書けない。cdk_skk_romkana_feed の後読みを見ること。 */

    /* は行 */
    {"ha", "は", ""}, {"hi", "ひ", ""}, {"hu", "ふ", ""}, {"he", "へ", ""}, {"ho", "ほ", ""},
    {"hya", "ひゃ", ""}, {"hyi", "ひぃ", ""}, {"hyu", "ひゅ", ""},
    {"hye", "ひぇ", ""}, {"hyo", "ひょ", ""},
    {"fa", "ふぁ", ""}, {"fi", "ふぃ", ""}, {"fu", "ふ", ""},
    {"fe", "ふぇ", ""}, {"fo", "ふぉ", ""},
    {"fya", "ふゃ", ""}, {"fyu", "ふゅ", ""}, {"fyo", "ふょ", ""},

    /* ば行 */
    {"ba", "ば", ""}, {"bi", "び", ""}, {"bu", "ぶ", ""}, {"be", "べ", ""}, {"bo", "ぼ", ""},
    {"bya", "びゃ", ""}, {"byi", "びぃ", ""}, {"byu", "びゅ", ""},
    {"bye", "びぇ", ""}, {"byo", "びょ", ""},

    /* ぱ行 */
    {"pa", "ぱ", ""}, {"pi", "ぴ", ""}, {"pu", "ぷ", ""}, {"pe", "ぺ", ""}, {"po", "ぽ", ""},
    {"pya", "ぴゃ", ""}, {"pyi", "ぴぃ", ""}, {"pyu", "ぴゅ", ""},
    {"pye", "ぴぇ", ""}, {"pyo", "ぴょ", ""},

    /* ま行 */
    {"ma", "ま", ""}, {"mi", "み", ""}, {"mu", "む", ""}, {"me", "め", ""}, {"mo", "も", ""},
    {"mya", "みゃ", ""}, {"myi", "みぃ", ""}, {"myu", "みゅ", ""},
    {"mye", "みぇ", ""}, {"myo", "みょ", ""},

    /* や行 */
    {"ya", "や", ""}, {"yi", "い", ""}, {"yu", "ゆ", ""}, {"ye", "いぇ", ""}, {"yo", "よ", ""},

    /* ら行 */
    {"ra", "ら", ""}, {"ri", "り", ""}, {"ru", "る", ""}, {"re", "れ", ""}, {"ro", "ろ", ""},
    {"rya", "りゃ", ""}, {"ryi", "りぃ", ""}, {"ryu", "りゅ", ""},
    {"rye", "りぇ", ""}, {"ryo", "りょ", ""},

    /* わ行 */
    {"wa", "わ", ""}, {"wi", "うぃ", ""}, {"wu", "う", ""},
    {"we", "うぇ", ""}, {"wo", "を", ""},
    {"wha", "うぁ", ""}, {"whi", "うぃ", ""}, {"whu", "う", ""},
    {"whe", "うぇ", ""}, {"who", "うぉ", ""},

    /* ゔ */
    {"va", "ゔぁ", ""}, {"vi", "ゔぃ", ""}, {"vu", "ゔ", ""},
    {"ve", "ゔぇ", ""}, {"vo", "ゔぉ", ""},

    /* 小書き */
    {"xa", "ぁ", ""}, {"xi", "ぃ", ""}, {"xu", "ぅ", ""}, {"xe", "ぇ", ""}, {"xo", "ぉ", ""},
    {"xya", "ゃ", ""}, {"xyu", "ゅ", ""}, {"xyo", "ょ", ""},
    {"xtu", "っ", ""}, {"xtsu", "っ", ""}, {"xwa", "ゎ", ""},
    {"xka", "ヵ", ""}, {"xke", "ヶ", ""},
    /* **`l` で始まる綴りは置かない。** 小書き文字は上の x 系で全部打てる。
     * `l` に文字の役目があると、打ちかけが残ったときに英数へ戻れなくなる
     * （`l` 自身が打ちかけになり、次の `l` が `ll` = っ になって堂々巡り）。 */

    /* 促音。同じ子音が重なったら っ を出して 1 つぶん打ち直す。
     * n は重ねても ん になるので、ここには入れない。 */
    {"kk", "っ", "k"}, {"ss", "っ", "s"}, {"tt", "っ", "t"}, {"hh", "っ", "h"},
    {"mm", "っ", "m"}, {"yy", "っ", "y"}, {"rr", "っ", "r"}, {"ww", "っ", "w"},
    {"gg", "っ", "g"}, {"zz", "っ", "z"}, {"dd", "っ", "d"}, {"bb", "っ", "b"},
    {"pp", "っ", "p"}, {"cc", "っ", "c"}, {"jj", "っ", "j"}, {"ff", "っ", "f"},
    {"vv", "っ", "v"}, {"xx", "っ", "x"},
    {"tch", "っ", "ch"},

    /* 記号 */
    {"-", "ー", ""}, {",", "、", ""}, {".", "。", ""},
    {"[", "「", ""}, {"]", "」", ""},
    {"z-", "〜", ""}, {"z,", "‥", ""}, {"z.", "…", ""}, {"z/", "・", ""},
    {"zh", "←", ""}, {"zj", "↓", ""}, {"zk", "↑", ""}, {"zl", "→", ""},
    {"z[", "『", ""}, {"z]", "』", ""},
};

#define RULE_COUNT ((int)(sizeof(RULES) / sizeof(RULES[0])))

static const rom_rule_t *find_exact(const char *s)
{
    for (int i = 0; i < RULE_COUNT; i++) {
        if (strcmp(RULES[i].roma, s) == 0) {
            return &RULES[i];
        }
    }
    return NULL;
}

static bool is_prefix(const char *s)
{
    const size_t n = strlen(s);
    if (n == 0) {
        return true;
    }
    for (int i = 0; i < RULE_COUNT; i++) {
        if (strncmp(RULES[i].roma, s, n) == 0 && RULES[i].roma[n] != '\0') {
            return true;
        }
    }
    return false;
}

void cdk_skk_romkana_reset(cdk_skk_romkana_t *r)
{
    r->pend[0] = '\0';
}

const char *cdk_skk_romkana_pending(const cdk_skk_romkana_t *r)
{
    return r->pend;
}

static size_t append(char *out, size_t out_cap, size_t used, const char *s)
{
    const size_t n = strlen(s);
    if (used + n >= out_cap) {
        return used; /* 入らないものは捨てる。切れた UTF-8 を出すよりまし */
    }
    memcpy(out + used, s, n);
    out[used + n] = '\0';
    return used + n;
}

/* "n" が残っているときに c が来た。ん を確定すべきか。
 * 母音・y・' は "na" "nya" "n'" になるので確定しない。 */
static bool n_terminates(char c)
{
    return strchr("aiueoy'", c) == NULL;
}

size_t cdk_skk_romkana_feed(cdk_skk_romkana_t *r, char c, char *out, size_t out_cap,
                            bool *consumed)
{
    size_t used = 0;
    out[0] = '\0';
    if (consumed != NULL) {
        *consumed = true;
    }

    if ((unsigned char)c < 0x20 || c == 0x7F) {
        if (consumed != NULL) {
            *consumed = false;
        }
        return 0;
    }
    if (c >= 'A' && c <= 'Z') {
        c = (char)(c - 'A' + 'a');
    }

    /* 撥音の後読み。表では書けないのでここだけ特別扱いする。
     *
     *   n  + n         → まだ決めない（"nn" として持つ）
     *   n  + 子音・記号 → ん を確定して打ち直し
     *   nn + 母音・y   → ん + 打ち直し "n"   （konna → こんな）
     *   nn + その他    → ん
     *
     * "nn" を単に ん と決め打つと konna が「こんあ」になる。
     * 2 つ目の n が「ん」なのか「な行の頭」なのかは、その次を見ないと
     * 決まらないため。 */
    if (strcmp(r->pend, "nn") == 0) {
        used = append(out, out_cap, used, "ん");
        strcpy(r->pend, (strchr("aiueoy", c) != NULL) ? "n" : "");
    } else if (strcmp(r->pend, "n") == 0) {
        if (c == 'n') {
            strcpy(r->pend, "nn");
            return used;
        }
        if (n_terminates(c)) {
            used = append(out, out_cap, used, "ん");
            r->pend[0] = '\0';
        }
    }

    for (;;) {
        const size_t n = strlen(r->pend);
        if (n >= CDK_SKK_ROM_MAX) {
            /* 表に無い並びが伸び続けている。先頭を捨てて追いつく。 */
            memmove(r->pend, r->pend + 1, n);
        }
        const size_t len = strlen(r->pend);
        r->pend[len] = c;
        r->pend[len + 1] = '\0';

        const rom_rule_t *hit = find_exact(r->pend);
        if (hit != NULL) {
            used = append(out, out_cap, used, hit->kana);
            /* 打ち直しは 1 文字ずつ食べ直す。"kk" → っ + "k" の "k" は
             * それ自体では確定しないので、pend に置いて次の入力を待つ。 */
            strcpy(r->pend, hit->rest);
            return used;
        }
        if (is_prefix(r->pend)) {
            return used; /* まだ伸びる可能性がある */
        }

        /* どの規則にも当たらない。先頭 1 文字を諦めて出す。 */
        if (strlen(r->pend) <= 1) {
            char lit[2] = {r->pend[0], '\0'};
            used = append(out, out_cap, used, lit);
            r->pend[0] = '\0';
            return used;
        }
        char lit[2] = {r->pend[0], '\0'};
        used = append(out, out_cap, used, lit);
        memmove(r->pend, r->pend + 1, strlen(r->pend));
        /* 末尾の c を外して、残りを持ったまま c を入れ直す。 */
        r->pend[strlen(r->pend) - 1] = '\0';
    }
}

size_t cdk_skk_romkana_flush(cdk_skk_romkana_t *r, char *out, size_t out_cap)
{
    size_t used = 0;
    out[0] = '\0';
    if (r->pend[0] == '\0') {
        return 0;
    }
    if (strcmp(r->pend, "n") == 0 || strcmp(r->pend, "nn") == 0) {
        used = append(out, out_cap, used, "ん");
    } else {
        used = append(out, out_cap, used, r->pend);
    }
    r->pend[0] = '\0';
    return used;
}

/* ------------------------------------------------------------------ */
/* かなの種類を変える                                                  */
/* ------------------------------------------------------------------ */

/* UTF-8 を 1 文字読む。長さを *len に返す。壊れていたら 0 を返す。 */
static unsigned decode(const char *s, size_t avail, size_t *len)
{
    const unsigned char *p = (const unsigned char *)s;
    if (avail == 0) {
        *len = 0;
        return 0;
    }
    if (p[0] < 0x80) {
        *len = 1;
        return p[0];
    }
    if ((p[0] & 0xE0) == 0xC0 && avail >= 2) {
        *len = 2;
        return (unsigned)((p[0] & 0x1F) << 6 | (p[1] & 0x3F));
    }
    if ((p[0] & 0xF0) == 0xE0 && avail >= 3) {
        *len = 3;
        return (unsigned)((p[0] & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F));
    }
    if ((p[0] & 0xF8) == 0xF0 && avail >= 4) {
        *len = 4;
        return (unsigned)((p[0] & 0x07) << 18 | (p[1] & 0x3F) << 12 |
                          (p[2] & 0x3F) << 6 | (p[3] & 0x3F));
    }
    *len = 1;
    return 0xFFFD;
}

static size_t encode(unsigned cp, char *out, size_t out_cap, size_t used)
{
    char buf[5];
    size_t n = 0;
    if (cp < 0x80) {
        buf[n++] = (char)cp;
    } else if (cp < 0x800) {
        buf[n++] = (char)(0xC0 | (cp >> 6));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        buf[n++] = (char)(0xE0 | (cp >> 12));
        buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        buf[n++] = (char)(0xF0 | (cp >> 18));
        buf[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    }
    buf[n] = '\0';
    return append(out, out_cap, used, buf);
}

size_t cdk_skk_hira_to_kata(const char *in, size_t len, char *out, size_t out_cap)
{
    size_t used = 0;
    size_t i = 0;
    out[0] = '\0';
    while (i < len) {
        size_t clen = 0;
        unsigned cp = decode(in + i, len - i, &clen);
        if (clen == 0) {
            break;
        }
        if (cp >= 0x3041 && cp <= 0x3096) {
            cp += 0x60;
        } else if (cp == 0x309D || cp == 0x309E) {
            cp += 0x60; /* ゝゞ → ヽヾ */
        }
        used = encode(cp, out, out_cap, used);
        i += clen;
    }
    return used;
}

/* ひらがな → 半角カナ。索引はコードポイント順。
 * 濁点・半濁点つきは 2 文字（例: が → ｶﾞ）になる。 */
typedef struct {
    unsigned    cp;
    const char *han;
} han_map_t;

static const han_map_t HAN[] = {
    {0x3001, "､"}, {0x3002, "｡"}, {0x300C, "｢"}, {0x300D, "｣"},
    {0x30FB, "･"}, {0x30FC, "ｰ"},
    {0x3041, "ｧ"}, {0x3042, "ｱ"}, {0x3043, "ｨ"}, {0x3044, "ｲ"},
    {0x3045, "ｩ"}, {0x3046, "ｳ"}, {0x3047, "ｪ"}, {0x3048, "ｴ"},
    {0x3049, "ｫ"}, {0x304A, "ｵ"},
    {0x304B, "ｶ"}, {0x304C, "ｶﾞ"}, {0x304D, "ｷ"}, {0x304E, "ｷﾞ"},
    {0x304F, "ｸ"}, {0x3050, "ｸﾞ"}, {0x3051, "ｹ"}, {0x3052, "ｹﾞ"},
    {0x3053, "ｺ"}, {0x3054, "ｺﾞ"},
    {0x3055, "ｻ"}, {0x3056, "ｻﾞ"}, {0x3057, "ｼ"}, {0x3058, "ｼﾞ"},
    {0x3059, "ｽ"}, {0x305A, "ｽﾞ"}, {0x305B, "ｾ"}, {0x305C, "ｾﾞ"},
    {0x305D, "ｿ"}, {0x305E, "ｿﾞ"},
    {0x305F, "ﾀ"}, {0x3060, "ﾀﾞ"}, {0x3061, "ﾁ"}, {0x3062, "ﾁﾞ"},
    {0x3063, "ｯ"}, {0x3064, "ﾂ"}, {0x3065, "ﾂﾞ"}, {0x3066, "ﾃ"},
    {0x3067, "ﾃﾞ"}, {0x3068, "ﾄ"}, {0x3069, "ﾄﾞ"},
    {0x306A, "ﾅ"}, {0x306B, "ﾆ"}, {0x306C, "ﾇ"}, {0x306D, "ﾈ"}, {0x306E, "ﾉ"},
    {0x306F, "ﾊ"}, {0x3070, "ﾊﾞ"}, {0x3071, "ﾊﾟ"},
    {0x3072, "ﾋ"}, {0x3073, "ﾋﾞ"}, {0x3074, "ﾋﾟ"},
    {0x3075, "ﾌ"}, {0x3076, "ﾌﾞ"}, {0x3077, "ﾌﾟ"},
    {0x3078, "ﾍ"}, {0x3079, "ﾍﾞ"}, {0x307A, "ﾍﾟ"},
    {0x307B, "ﾎ"}, {0x307C, "ﾎﾞ"}, {0x307D, "ﾎﾟ"},
    {0x307E, "ﾏ"}, {0x307F, "ﾐ"}, {0x3080, "ﾑ"}, {0x3081, "ﾒ"}, {0x3082, "ﾓ"},
    {0x3083, "ｬ"}, {0x3084, "ﾔ"}, {0x3085, "ｭ"}, {0x3086, "ﾕ"},
    {0x3087, "ｮ"}, {0x3088, "ﾖ"},
    {0x3089, "ﾗ"}, {0x308A, "ﾘ"}, {0x308B, "ﾙ"}, {0x308C, "ﾚ"}, {0x308D, "ﾛ"},
    {0x308E, "ﾜ"}, {0x308F, "ﾜ"}, {0x3090, "ｲ"}, {0x3091, "ｴ"}, {0x3092, "ｦ"},
    {0x3093, "ﾝ"}, {0x3094, "ｳﾞ"},
};

#define HAN_COUNT ((int)(sizeof(HAN) / sizeof(HAN[0])))

size_t cdk_skk_hira_to_hankaku(const char *in, size_t len, char *out, size_t out_cap)
{
    size_t used = 0;
    size_t i = 0;
    out[0] = '\0';
    while (i < len) {
        size_t clen = 0;
        unsigned cp = decode(in + i, len - i, &clen);
        if (clen == 0) {
            break;
        }
        /* カタカナで来ることもあるので、ひらがなに寄せてから引く。 */
        if (cp >= 0x30A1 && cp <= 0x30F6) {
            cp -= 0x60;
        }
        const char *han = NULL;
        for (int k = 0; k < HAN_COUNT; k++) {
            if (HAN[k].cp == cp) {
                han = HAN[k].han;
                break;
            }
        }
        if (han != NULL) {
            used = append(out, out_cap, used, han);
        } else {
            used = encode(cp, out, out_cap, used);
        }
        i += clen;
    }
    return used;
}

size_t cdk_skk_ascii_to_zenkaku(const char *in, size_t len, char *out, size_t out_cap)
{
    size_t used = 0;
    out[0] = '\0';
    for (size_t i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)in[i];
        if (c == ' ') {
            used = encode(0x3000, out, out_cap, used);
        } else if (c >= 0x21 && c <= 0x7E) {
            used = encode(0xFF01u + (c - 0x21u), out, out_cap, used);
        } else {
            used = encode(c, out, out_cap, used);
        }
    }
    return used;
}
