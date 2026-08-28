#include "cdk_hid.h"
#include "t.h"

/* 出てきたキーを溜める */
#define MAX_EV 64
static cdk_key_t EV[MAX_EV];
static int       NEV;

static void sink(void *user, const cdk_key_t *k)
{
    (void)user;
    if (NEV < MAX_EV) {
        EV[NEV++] = *k;
    }
}

static void reset(void)
{
    NEV = 0;
}

/* ブートレポートを組み立てる。可変長引数のかわりに固定 6 個。 */
static void rep(cdk_hid_t *h, uint8_t mods, uint8_t a, uint8_t b, uint8_t c,
                uint32_t now)
{
    const uint8_t r[8] = {mods, 0, a, b, c, 0, 0, 0};
    cdk_hid_report(h, r, sizeof(r), now, sink, NULL);
}

/* 押して離す */
static void tap(cdk_hid_t *h, uint8_t mods, uint8_t usage, uint32_t now)
{
    rep(h, mods, usage, 0, 0, now);
    rep(h, mods, 0, 0, 0, now + 1u);
}

static uint32_t cp_of(int i)
{
    return EV[i].codepoint;
}

int main(void)
{
    cdk_hid_t H;

    T_CASE("英字を打つ");
    {
        cdk_hid_init(&H, CDK_HID_LAYOUT_US);
        reset();
        tap(&H, 0, 0x04, 100); /* a */
        tap(&H, 0, 0x05, 200); /* b */
        tap(&H, 0, 0x06, 300); /* c */
        T_EQ_INT(NEV, 3);
        T_EQ_U32(cp_of(0), 'a');
        T_EQ_U32(cp_of(1), 'b');
        T_EQ_U32(cp_of(2), 'c');
        /* 文字キーは keycode を 0 にする。cdk_keys_encode が
         * codepoint 側の経路（Ctrl 変換や UTF-8）を通るのはこのとき。 */
        T_EQ_INT(EV[0].keycode, 0);
    }

    T_CASE("Shift で大文字");
    {
        cdk_hid_init(&H, CDK_HID_LAYOUT_US);
        reset();
        tap(&H, 0x02, 0x04, 100); /* LShift + a */
        T_EQ_INT(NEV, 1);
        T_EQ_U32(cp_of(0), 'A');
        T_CHECK((EV[0].mods & CDK_MOD_SHIFT_BIT) != 0u);

        /* 右 Shift も同じ */
        reset();
        tap(&H, 0x20, 0x04, 200);
        T_EQ_U32(cp_of(0), 'A');
    }

    T_CASE("CapsLock は Shift と XOR");
    {
        cdk_hid_init(&H, CDK_HID_LAYOUT_US);
        reset();
        tap(&H, 0, 0x39, 100); /* CapsLock 自体は何も出さない */
        T_EQ_INT(NEV, 0);

        tap(&H, 0, 0x04, 200);
        T_EQ_U32(cp_of(0), 'A'); /* caps だけ → 大文字 */

        reset();
        tap(&H, 0x02, 0x04, 300);
        T_EQ_U32(cp_of(0), 'a'); /* caps + shift → 小文字に戻る */

        reset();
        tap(&H, 0, 0x39, 400); /* 解除 */
        tap(&H, 0, 0x04, 500);
        T_EQ_U32(cp_of(0), 'a');
    }

    T_CASE("Ctrl は修飾子として乗る");
    {
        cdk_hid_init(&H, CDK_HID_LAYOUT_US);
        reset();
        tap(&H, 0x01, 0x06, 100); /* Ctrl + c */
        T_EQ_INT(NEV, 1);
        T_EQ_U32(cp_of(0), 'c');
        T_CHECK((EV[0].mods & CDK_MOD_CTRL_BIT) != 0u);
        /* **ここで 0x03 に変換してはいけない。** 制御コードへの変換は
         * cdk_keys_encode の仕事で、SKK はその前に生のキーを見る。 */
    }

    T_CASE("特殊キーは usage id をそのまま keycode に");
    {
        cdk_hid_init(&H, CDK_HID_LAYOUT_US);
        reset();
        tap(&H, 0, 0x28, 100); /* Enter */
        tap(&H, 0, 0x2A, 200); /* BS */
        tap(&H, 0, 0x2B, 300); /* Tab */
        tap(&H, 0, 0x29, 400); /* Esc */
        tap(&H, 0, 0x2C, 500); /* Space */
        tap(&H, 0, 0x52, 600); /* Up */
        T_EQ_INT(NEV, 6);
        T_EQ_INT(EV[0].keycode, CDK_K_ENTER);
        T_EQ_INT(EV[1].keycode, CDK_K_BS);
        T_EQ_INT(EV[2].keycode, CDK_K_TAB);
        T_EQ_INT(EV[3].keycode, CDK_K_ESC);
        T_EQ_INT(EV[4].keycode, CDK_K_SPACE);
        T_EQ_INT(EV[5].keycode, CDK_K_UP);
        /* 特殊キーは codepoint を 0 にしておく（両方入れると
         * encode がどちらを使うか呼び出し順に依存する） */
        T_EQ_U32(EV[0].codepoint, 0u);

        /* テンキーの Enter も普通の Enter として扱う */
        reset();
        tap(&H, 0, 0x58, 700);
        T_EQ_INT(EV[0].keycode, CDK_K_ENTER);
    }

    T_CASE("押しっぱなしは 1 回しか出ない");
    {
        cdk_hid_init(&H, CDK_HID_LAYOUT_US);
        reset();
        rep(&H, 0, 0x04, 0, 0, 100);
        rep(&H, 0, 0x04, 0, 0, 110); /* 同じ状態のレポートが再送されても */
        rep(&H, 0, 0x04, 0, 0, 120);
        T_EQ_INT(NEV, 1);
    }

    T_CASE("同時押しは押された順に 1 回ずつ");
    {
        cdk_hid_init(&H, CDK_HID_LAYOUT_US);
        reset();
        rep(&H, 0, 0x04, 0, 0, 100);    /* a */
        rep(&H, 0, 0x04, 0x05, 0, 110); /* a 押したまま b */
        rep(&H, 0, 0x05, 0, 0, 120);    /* a を離す */
        T_EQ_INT(NEV, 2);
        T_EQ_U32(cp_of(0), 'a');
        T_EQ_U32(cp_of(1), 'b');
    }

    T_CASE("ロールオーバ超過は捨てる");
    {
        const uint8_t over[8] = {0, 0, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01};
        cdk_hid_init(&H, CDK_HID_LAYOUT_US);
        reset();
        cdk_hid_report(&H, over, sizeof(over), 100, sink, NULL);
        T_EQ_INT(NEV, 0);
        /* 超過から戻ったら、また普通に打てる */
        tap(&H, 0, 0x04, 200);
        T_EQ_INT(NEV, 1);
        T_EQ_U32(cp_of(0), 'a');
    }

    T_CASE("短いレポートは無視する");
    {
        const uint8_t shortrep[4] = {0, 0, 0x04, 0};
        cdk_hid_init(&H, CDK_HID_LAYOUT_US);
        reset();
        cdk_hid_report(&H, shortrep, sizeof(shortrep), 100, sink, NULL);
        T_EQ_INT(NEV, 0);
    }

    T_CASE("US の記号");
    {
        cdk_hid_init(&H, CDK_HID_LAYOUT_US);
        reset();
        tap(&H, 0x02, 0x1F, 100); /* Shift+2 */
        T_EQ_U32(cp_of(0), '@');
        reset();
        tap(&H, 0, 0x2F, 200); /* P の右 */
        T_EQ_U32(cp_of(0), '[');
        reset();
        tap(&H, 0, 0x33, 300);
        T_EQ_U32(cp_of(0), ';');
        reset();
        tap(&H, 0x02, 0x34, 400); /* Shift+' */
        T_EQ_U32(cp_of(0), '"');
    }

    T_CASE("JIS の記号は同じキーでも違う字になる");
    {
        cdk_hid_init(&H, CDK_HID_LAYOUT_JIS);
        reset();
        /* **配列を見分ける決め手**: @ がどのキーから出るか。
         * US は Shift+2、JIS は P の右のキー。 */
        tap(&H, 0, 0x2F, 100);
        T_EQ_U32(cp_of(0), '@');
        reset();
        tap(&H, 0x02, 0x1F, 200); /* JIS の Shift+2 は " */
        T_EQ_U32(cp_of(0), '"');
        reset();
        tap(&H, 0x02, 0x34, 300); /* JIS の Shift+: は * */
        T_EQ_U32(cp_of(0), '*');
        reset();
        tap(&H, 0, 0x87, 400); /* ろ */
        T_EQ_U32(cp_of(0), '\\');
        reset();
        tap(&H, 0, 0x89, 500); /* ￥ */
        T_EQ_U32(cp_of(0), 0xA5u);
    }

    T_CASE("配列は実行時に切り替わる");
    {
        cdk_hid_init(&H, CDK_HID_LAYOUT_US);
        reset();
        tap(&H, 0, 0x2F, 100);
        T_EQ_U32(cp_of(0), '[');
        cdk_hid_set_layout(&H, CDK_HID_LAYOUT_JIS);
        reset();
        tap(&H, 0, 0x2F, 200);
        T_EQ_U32(cp_of(0), '@');
    }

    T_CASE("JIS にしか無いキーを見分けられる");
    {
        T_CHECK(cdk_hid_usage_is_jis_only(0x87));
        T_CHECK(cdk_hid_usage_is_jis_only(0x89));
        T_CHECK(cdk_hid_usage_is_jis_only(0x8A));
        T_CHECK(!cdk_hid_usage_is_jis_only(0x04));
        T_CHECK(!cdk_hid_usage_is_jis_only(0x2F));
    }

    T_CASE("オートリピートは時計で進む");
    {
        cdk_hid_init(&H, CDK_HID_LAYOUT_US);
        reset();
        rep(&H, 0, 0x04, 0, 0, 1000); /* a を押しっぱなし */
        T_EQ_INT(NEV, 1);

        /* **レポートが来なくても繰り返す。** ブートキーボードは状態が
         * 変わったときしか送ってこないので、ここがレポート駆動だと
         * 「別のキーを押したときだけ連射される」ことになる。 */
        cdk_hid_tick(&H, 1000 + CDK_HID_REPEAT_DELAY_MS - 1u, sink, NULL);
        T_EQ_INT(NEV, 1); /* まだ */

        cdk_hid_tick(&H, 1000 + CDK_HID_REPEAT_DELAY_MS, sink, NULL);
        T_EQ_INT(NEV, 2);
        T_EQ_U32(cp_of(1), 'a');

        /* そのあとは短い間隔で続く */
        uint32_t t = 1000 + CDK_HID_REPEAT_DELAY_MS + CDK_HID_REPEAT_RATE_MS;
        cdk_hid_tick(&H, t, sink, NULL);
        T_EQ_INT(NEV, 3);

        /* 離したら止まる */
        rep(&H, 0, 0, 0, 0, t + 1u);
        const int after = NEV;
        cdk_hid_tick(&H, t + 10000u, sink, NULL);
        T_EQ_INT(NEV, after);
    }

    T_CASE("リピート中に時刻が一周しても止まらない");
    {
        cdk_hid_init(&H, CDK_HID_LAYOUT_US);
        reset();
        /* あと少しで一周する時刻で押す */
        const uint32_t base = 0xFFFFFF00u;
        rep(&H, 0, 0x04, 0, 0, base);
        T_EQ_INT(NEV, 1);
        /* 一周をまたいだ時刻。符号なしの引き算なので正しく「過ぎた」と判定される */
        cdk_hid_tick(&H, base + CDK_HID_REPEAT_DELAY_MS, sink, NULL);
        T_EQ_INT(NEV, 2);
    }

    T_CASE("修飾キー単体では何も出ない");
    {
        cdk_hid_init(&H, CDK_HID_LAYOUT_US);
        reset();
        rep(&H, 0x02, 0, 0, 0, 100); /* Shift を押しただけ */
        rep(&H, 0x01, 0, 0, 0, 200); /* Ctrl を押しただけ */
        T_EQ_INT(NEV, 0);
    }

    return t_report("test_hid");
}
