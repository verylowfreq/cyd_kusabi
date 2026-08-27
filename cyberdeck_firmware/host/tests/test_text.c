#include "cdk_text.h"
#include "t.h"

static uint32_t dec1(const char *s, size_t *used, bool *ok)
{
    uint32_t cp = 0;
    *used = cdk_utf8_decode((const uint8_t *)s, strlen(s), &cp, ok);
    return cp;
}

int main(void)
{
    T_CASE("UTF-8 デコード");
    {
        size_t n;
        bool ok;
        T_EQ_U32(dec1("A", &n, &ok), 0x41u);
        T_EQ_INT(n, 1);
        T_CHECK(ok);

        T_EQ_U32(dec1("\xC3\xA9", &n, &ok), 0xE9u); /* é */
        T_EQ_INT(n, 2);

        T_EQ_U32(dec1("\xE3\x81\x82", &n, &ok), 0x3042u); /* あ */
        T_EQ_INT(n, 3);

        T_EQ_U32(dec1("\xF0\x9F\x98\x80", &n, &ok), 0x1F600u); /* 😀 */
        T_EQ_INT(n, 4);
    }

    T_CASE("UTF-8 の不正な入力");
    {
        size_t n;
        bool ok;
        /* 継続バイトが単独で来た */
        T_EQ_U32(dec1("\x80", &n, &ok), CDK_REPLACEMENT);
        T_CHECK(!ok);
        T_EQ_INT(n, 1);

        /* 冗長符号化 (C0 80 は U+0000 の 2 バイト表現) */
        T_EQ_U32(dec1("\xC0\x80", &n, &ok), CDK_REPLACEMENT);
        T_CHECK(!ok);

        /* サロゲート U+D800 */
        T_EQ_U32(dec1("\xED\xA0\x80", &n, &ok), CDK_REPLACEMENT);
        T_CHECK(!ok);

        /* 途中で切れた */
        T_EQ_INT(dec1("\xE3\x81", &n, &ok), 0);
        T_EQ_INT(n, 0);
        T_CHECK(!ok);
    }

    T_CASE("不正バイトの次が正しい文字の先頭なら消費しない");
    {
        /* E3 の後に 'A' が来た。'A' は次の文字なので retry になる。 */
        cdk_utf8_t d;
        cdk_utf8_reset(&d);
        uint32_t cp = 0;
        bool retry = false;
        T_EQ_INT(cdk_utf8_feed(&d, 0xE3, &cp, &retry), CDK_UTF8_NEED);
        T_EQ_INT(cdk_utf8_feed(&d, 'A', &cp, &retry), CDK_UTF8_INVALID);
        T_CHECK(retry);
        T_EQ_U32(cp, CDK_REPLACEMENT);
        /* もう一度食わせれば 'A' が取れる */
        T_EQ_INT(cdk_utf8_feed(&d, 'A', &cp, &retry), CDK_UTF8_OK);
        T_EQ_U32(cp, 'A');
    }

    T_CASE("バイトを分割して食わせても組み立てられる");
    {
        cdk_utf8_t d;
        cdk_utf8_reset(&d);
        uint32_t cp = 0;
        const uint8_t seq[] = {0xE3, 0x81, 0x82};
        T_EQ_INT(cdk_utf8_feed(&d, seq[0], &cp, NULL), CDK_UTF8_NEED);
        T_EQ_INT(cdk_utf8_feed(&d, seq[1], &cp, NULL), CDK_UTF8_NEED);
        T_EQ_INT(cdk_utf8_feed(&d, seq[2], &cp, NULL), CDK_UTF8_OK);
        T_EQ_U32(cp, 0x3042u);
    }

    T_CASE("UTF-8 エンコード");
    {
        uint8_t b[4];
        T_EQ_INT(cdk_utf8_encode('A', b), 1);
        T_EQ_INT(b[0], 'A');
        T_EQ_INT(cdk_utf8_encode(0x3042u, b), 3);
        T_EQ_INT(b[0], 0xE3);
        T_EQ_INT(b[1], 0x81);
        T_EQ_INT(b[2], 0x82);
        T_EQ_INT(cdk_utf8_encode(0x1F600u, b), 4);
        /* 往復 */
        for (uint32_t cp = 1; cp < 0x11000u; cp += 7u) {
            if (cp >= 0xD800u && cp <= 0xDFFFu) {
                continue;
            }
            const size_t n = cdk_utf8_encode(cp, b);
            uint32_t back = 0;
            bool ok = false;
            T_EQ_INT(cdk_utf8_decode(b, n, &back, &ok), n);
            T_CHECK(ok);
            T_EQ_U32(back, cp);
        }
    }

    T_CASE("文字幅");
    {
        /* 制御文字はセルを使わない */
        T_EQ_INT(cdk_char_width(0x00), 0);
        T_EQ_INT(cdk_char_width('\n'), 0);
        T_EQ_INT(cdk_char_width(0x7F), 0);

        /* ASCII は半角 */
        T_EQ_INT(cdk_char_width(' '), 1);
        T_EQ_INT(cdk_char_width('A'), 1);
        T_EQ_INT(cdk_char_width('~'), 1);

        /* かな・漢字・全角記号は全角 */
        T_EQ_INT(cdk_char_width(0x3042), 2); /* あ */
        T_EQ_INT(cdk_char_width(0x30A2), 2); /* ア */
        T_EQ_INT(cdk_char_width(0x6F22), 2); /* 漢 */
        T_EQ_INT(cdk_char_width(0x3000), 2); /* 全角空白 */
        T_EQ_INT(cdk_char_width(0xFF21), 2); /* Ａ */
        T_EQ_INT(cdk_char_width(0xAC00), 2); /* ハングル */

        /* 半角カナは半角 */
        T_EQ_INT(cdk_char_width(0xFF71), 1); /* ｱ */
        T_EQ_INT(cdk_char_width(0xFF61), 1); /* ｡ */

        /* 罫線素片は Ambiguous。端末の既定に合わせて半角。
         * ここが 2 になると CLI の枠が必ず崩れる。 */
        T_EQ_INT(cdk_char_width(0x2500), 1);
        T_EQ_INT(cdk_char_width(0x2502), 1);
        T_EQ_INT(cdk_char_width(0x253C), 1);
        T_EQ_INT(cdk_char_width(0x2588), 1);

        /* 結合文字はゼロ幅 */
        T_EQ_INT(cdk_char_width(0x0301), 0); /* 合成用アクセント */
        T_EQ_INT(cdk_char_width(0x200B), 0); /* ZWSP */
        T_EQ_INT(cdk_char_width(0xFE0F), 0); /* 異体字セレクタ */

        /* 絵文字はグリフを持たないが幅は 2 */
        T_EQ_INT(cdk_char_width(0x1F600), 2);
        T_EQ_INT(cdk_char_width(0x1F680), 2);

        /* CJK 拡張 B */
        T_EQ_INT(cdk_char_width(0x20000), 2);
    }

    T_CASE("Ambiguous 幅の判定");
    {
        T_CHECK(cdk_char_ambiguous(0x2500));  /* ─ */
        T_CHECK(cdk_char_ambiguous(0x03B1));  /* α */
        T_CHECK(cdk_char_ambiguous(0x2460));  /* ① */
        T_CHECK(!cdk_char_ambiguous('A'));
        T_CHECK(!cdk_char_ambiguous(0x3042)); /* あ は Wide であって Ambiguous ではない */
    }

    T_CASE("全コードポイントで幅が 0/1/2 に収まる");
    {
        /* 二分探索の表が壊れていれば、ここで範囲外の値が出る。 */
        int bad = 0;
        for (uint32_t cp = 0; cp <= 0x10FFFFu; cp++) {
            const int w = cdk_char_width(cp);
            if (w < 0 || w > 2) {
                bad++;
            }
        }
        T_EQ_INT(bad, 0);
    }

    return t_report("test_text");
}
