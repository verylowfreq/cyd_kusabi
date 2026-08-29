#include "cdk_retry.h"
#include "t.h"

int main(void)
{
    cdk_retry_t R;

    T_CASE("始めるまでは動かない");
    {
        cdk_retry_init(&R, 1000, 15000, 5);
        T_CHECK(!cdk_retry_active(&R));
        T_CHECK(!cdk_retry_due(&R, 999999));
        T_EQ_U32(cdk_retry_remaining_ms(&R, 0), 0u);
    }

    T_CASE("待ってから 1 回目を試す");
    {
        cdk_retry_init(&R, 1000, 15000, 5);
        cdk_retry_begin(&R, 10000);
        T_CHECK(cdk_retry_active(&R));
        /* **切れた直後に即試さない。** 相手もこちらも整っていないことが多い。 */
        T_CHECK(!cdk_retry_due(&R, 10000));
        T_CHECK(!cdk_retry_due(&R, 10999));
        T_CHECK(cdk_retry_due(&R, 11000));
        T_EQ_INT(cdk_retry_next_attempt(&R), 1);
        T_EQ_U32(cdk_retry_remaining_ms(&R, 10400), 600u);
        T_EQ_U32(cdk_retry_remaining_ms(&R, 12000), 0u);
    }

    T_CASE("待ち時間が倍々になる");
    {
        cdk_retry_init(&R, 1000, 15000, 0); /* 無制限 */
        uint32_t t = 0;
        cdk_retry_begin(&R, t);

        const uint32_t want[] = {1000, 2000, 4000, 8000, 15000, 15000, 15000};
        for (int i = 0; i < 7; i++) {
            T_EQ_U32(cdk_retry_remaining_ms(&R, t), want[i]);
            t += want[i];
            T_CHECK(cdk_retry_due(&R, t));
            cdk_retry_failed(&R, t);
        }
        /* 上限で頭打ちになり、無制限なので止まらない */
        T_CHECK(cdk_retry_active(&R));
    }

    T_CASE("上限まで試したら諦める");
    {
        cdk_retry_init(&R, 100, 1000, 3);
        uint32_t t = 0;
        cdk_retry_begin(&R, t);
        for (int i = 0; i < 3; i++) {
            T_CHECK(cdk_retry_active(&R));
            t += 5000;
            cdk_retry_failed(&R, t);
        }
        T_CHECK(!cdk_retry_active(&R));
        T_CHECK(cdk_retry_exhausted(&R));
        T_CHECK(!cdk_retry_due(&R, t + 100000));
    }

    T_CASE("止めたら諦めた扱いにはしない");
    {
        /* 人が中止したのと、試し尽くしたのは別。画面に出す文言が変わる。 */
        cdk_retry_init(&R, 100, 1000, 3);
        cdk_retry_begin(&R, 0);
        cdk_retry_stop(&R);
        T_CHECK(!cdk_retry_active(&R));
        T_CHECK(!cdk_retry_exhausted(&R));
    }

    T_CASE("時刻が一周しても止まらない");
    {
        /* **符号なしで比べると、一周した瞬間に「まだ来ていない」と判断して
         * 永久に再接続しなくなる。** SSH は何日も繋ぎっぱなしにするので、
         * 49 日で一周する ms のカウンタは実際にまたぐ。 */
        cdk_retry_init(&R, 1000, 15000, 0);
        const uint32_t base = 0xFFFFFC00u; /* あと 1024ms で一周 */
        cdk_retry_begin(&R, base);
        T_CHECK(!cdk_retry_due(&R, base + 999u));
        T_CHECK(cdk_retry_due(&R, base + 1000u)); /* ここで一周をまたぐ */
        T_EQ_U32(cdk_retry_remaining_ms(&R, base + 400u), 600u);
    }

    T_CASE("上限が base より小さくても壊れない");
    {
        cdk_retry_init(&R, 5000, 1000, 0);
        cdk_retry_begin(&R, 0);
        /* cap は base 未満にできない。0 待ちで連打になるのを防ぐ。 */
        T_EQ_U32(cdk_retry_remaining_ms(&R, 0), 5000u);
        cdk_retry_failed(&R, 5000);
        T_EQ_U32(cdk_retry_remaining_ms(&R, 5000), 5000u);
    }

    T_CASE("大きな上限でも溢れない");
    {
        /* 倍にしてから上限で止めると 32bit を溢れて待ちが短くなる。
         * 上限を上げたのに連打になる、という逆の壊れ方をする。
         *
         * **上限自体も 2^31 ms で頭打ちにしている。** 一周をまたぐ判定を
         * 符号付きの差でやっているので、それより長い間隔は表現できない。 */
        cdk_retry_init(&R, 1000, 0xF0000000u, 0);
        T_EQ_U32(R.cap_ms, 0x7FFFFFFFu);

        cdk_retry_begin(&R, 0);
        uint32_t t = 0, prev = 0;
        for (int i = 0; i < 40; i++) {
            const uint32_t d = cdk_retry_remaining_ms(&R, t);
            T_CHECK(d >= prev); /* 単調非減少。縮んだら溢れている */
            T_CHECK(d <= 0x7FFFFFFFu);
            prev = d;
            t += d;
            cdk_retry_failed(&R, t);
        }
    }

    T_CASE("NULL で落ちない");
    {
        cdk_retry_init(NULL, 1, 2, 3);
        cdk_retry_begin(NULL, 0);
        cdk_retry_stop(NULL);
        T_CHECK(!cdk_retry_active(NULL));
        T_CHECK(!cdk_retry_due(NULL, 0));
        cdk_retry_failed(NULL, 0);
        T_EQ_U32(cdk_retry_remaining_ms(NULL, 0), 0u);
        T_CHECK(!cdk_retry_exhausted(NULL));
    }

    return t_report("test_retry");
}
