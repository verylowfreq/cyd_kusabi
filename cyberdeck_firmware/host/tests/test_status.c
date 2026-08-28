#include "cdk_status.h"
#include "t.h"

static char BUF[256];

static cdk_status_t full(void)
{
    cdk_status_t st;
    memset(&st, 0, sizeof(st));
    st.wifi_up = true;
    st.ip = "192.168.1.97";
    st.sd_ok = true;
    st.sd_mb = 15193;
    st.ime = "かな";
    st.kbd = true;
    st.ssh_up = true;
    st.batt_mv = 2450;
    return st;
}

int main(void)
{
    T_CASE("表示桁数の数え方");
    {
        T_EQ_INT(cdk_status_width(""), 0);
        T_EQ_INT(cdk_status_width("abc"), 3);
        /* **全角は 2 桁。**バイト数で数えると 6 になって #21 と同じ壊れ方をする */
        T_EQ_INT(cdk_status_width("かな"), 4);
        T_EQ_INT(cdk_status_width("SD15G かな"), 10);
        T_EQ_INT(cdk_status_width(NULL), 0);
    }

    T_CASE("80 桁なら全部入る");
    {
        const cdk_status_t st = full();
        const size_t n = cdk_status_format(&st, 80, BUF, sizeof(BUF));
        T_CHECK(n > 0);
        T_CHECK(strstr(BUF, "192.168.1.97") != NULL);
        T_CHECK(strstr(BUF, "SSH") != NULL);
        T_CHECK(strstr(BUF, "かな") != NULL);
        T_CHECK(strstr(BUF, "SD14G") != NULL); /* 15193MB = 14GB */
        T_CHECK(strstr(BUF, "KBD") != NULL);
        T_CHECK(strstr(BUF, "2.45V") != NULL);
        T_CHECK(cdk_status_width(BUF) <= 80);
    }

    T_CASE("40 桁でも桁を超えない");
    {
        /* **クリップが実際に効くのはここ**（2 倍表示は 40 桁）。 */
        const cdk_status_t st = full();
        const size_t n = cdk_status_format(&st, 40, BUF, sizeof(BUF));
        T_CHECK(n > 0);
        T_CHECK(cdk_status_width(BUF) <= 40);
        /* 大事なものから入る。IP は必ず残る */
        T_CHECK(strstr(BUF, "192.168.1.97") != NULL);
    }

    T_CASE("狭いほど後ろから落ちる");
    {
        const cdk_status_t st = full();
        int prev = 0;
        for (int cols = 12; cols <= 60; cols += 4) {
            cdk_status_format(&st, cols, BUF, sizeof(BUF));
            const int w = cdk_status_width(BUF);
            T_CHECK(w <= cols);
            /* 桁を広げると情報が減ることはない */
            T_CHECK(w >= prev);
            prev = w;
        }
    }

    T_CASE("項目は途中で切らない");
    {
        /* IP がちょうど入らない桁数。**半端に切れた IP を出さない。** */
        const cdk_status_t st = full();
        cdk_status_format(&st, 5, BUF, sizeof(BUF));
        T_CHECK(strstr(BUF, "192.168") == NULL);
        T_CHECK(cdk_status_width(BUF) <= 5);
    }

    T_CASE("繋がっていないときの表示");
    {
        cdk_status_t st;
        memset(&st, 0, sizeof(st));
        st.ime = "英数";
        cdk_status_format(&st, 80, BUF, sizeof(BUF));
        T_CHECK(strstr(BUF, "WiFi--") != NULL);
        T_CHECK(strstr(BUF, "SD--") != NULL);
        T_CHECK(strstr(BUF, "SSH") == NULL);
        T_CHECK(strstr(BUF, "KBD") == NULL);
        /* 電池電圧は 0 なら出さない（読めていないのに 0.00V と出すと嘘になる） */
        T_CHECK(strstr(BUF, "V") == NULL);
    }

    T_CASE("MB と GB の切り替え");
    {
        cdk_status_t st;
        memset(&st, 0, sizeof(st));
        st.sd_ok = true;
        st.sd_mb = 512;
        cdk_status_format(&st, 80, BUF, sizeof(BUF));
        T_CHECK(strstr(BUF, "SD512M") != NULL);
        st.sd_mb = 2048;
        cdk_status_format(&st, 80, BUF, sizeof(BUF));
        T_CHECK(strstr(BUF, "SD2G") != NULL);
    }

    T_CASE("不正な引数で落ちない");
    {
        const cdk_status_t st = full();
        T_EQ_INT((int)cdk_status_format(NULL, 80, BUF, sizeof(BUF)), 0);
        T_EQ_INT((int)cdk_status_format(&st, 0, BUF, sizeof(BUF)), 0);
        T_EQ_INT((int)cdk_status_format(&st, -5, BUF, sizeof(BUF)), 0);
        T_EQ_INT((int)cdk_status_format(&st, 80, NULL, 10), 0);
        /* 出力が極端に短くても壊れない */
        char tiny[4];
        cdk_status_format(&st, 80, tiny, sizeof(tiny));
        T_CHECK(strlen(tiny) < sizeof(tiny));
    }

    return t_report("test_status");
}
