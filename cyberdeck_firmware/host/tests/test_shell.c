/* cdk_shell の単体テスト。
 *
 * 行編集は「出力バイト列」ではなく「編集後の行の中身」で確認する。
 * 描き直しの仕方は実装の都合で変わるが、行の中身とカーソル位置は仕様だから。
 */
#include "cdk_shell.h"
#include "t.h"

#include <stdio.h>

static char OUT[8192];
static size_t OUTLEN;

static void on_out(void *user, const char *data, size_t len)
{
    (void)user;
    if (OUTLEN + len < sizeof(OUT)) {
        memcpy(OUT + OUTLEN, data, len);
        OUTLEN += len;
        OUT[OUTLEN] = '\0';
    }
}

static cdk_shell_t SH;

/* 実行されたコマンドを記録する */
static char LAST_CMD[256];
static int  LAST_ARGC;
static int  ECHO_CALLS;

static int cmd_echo(int argc, char **argv, void *user)
{
    (void)user;
    ECHO_CALLS++;
    LAST_ARGC = argc;
    LAST_CMD[0] = '\0';
    for (int i = 0; i < argc; i++) {
        if (i > 0) {
            strncat(LAST_CMD, "|", sizeof(LAST_CMD) - strlen(LAST_CMD) - 1);
        }
        strncat(LAST_CMD, argv[i], sizeof(LAST_CMD) - strlen(LAST_CMD) - 1);
    }
    return 0;
}

static int cmd_fail(int argc, char **argv, void *user)
{
    (void)argc;
    (void)argv;
    (void)user;
    return 3;
}

static int cmd_quit(int argc, char **argv, void *user)
{
    (void)argc;
    (void)argv;
    cdk_shell_end((cdk_shell_t *)user);
    return 0;
}

static const cdk_shell_cmd_t CMDS[] = {
    {"echo", "文字を出す", cmd_echo},
    {"exit", "抜ける", cmd_quit},
    {"expand", "補完テスト用", cmd_echo},
    {"fail", "失敗する", cmd_fail},
};

static void setup(void)
{
    OUTLEN = 0;
    OUT[0] = '\0';
    LAST_CMD[0] = '\0';
    LAST_ARGC = 0;
    ECHO_CALLS = 0;
    cdk_shell_init(&SH, on_out, NULL);
    cdk_shell_set_commands(&SH, CMDS, sizeof(CMDS) / sizeof(CMDS[0]), &SH);
    cdk_shell_begin(&SH);
}

static void key(uint16_t code, uint8_t mods, uint32_t cp)
{
    const cdk_key_t k = {code, mods, cp};
    cdk_shell_key(&SH, &k);
}

static void type(const char *s)
{
    for (const char *p = s; *p != '\0'; p++) {
        key(CDK_K_NONE, 0, (uint32_t)(unsigned char)*p);
    }
}

static void eq_line(const char *want, const char *what)
{
    t_checks++;
    if (strcmp(SH.line, want) != 0) {
        t_failures++;
        printf("   FAIL %s: 期待 [%s] 実際 [%s]\n", what, want, SH.line);
    }
}

int main(void)
{
    T_CASE("文字を打つ");
    {
        setup();
        type("hello");
        eq_line("hello", "hello");
        T_EQ_INT(SH.cursor, 5);
    }

    T_CASE("バックスペースとカーソル移動");
    {
        setup();
        type("hello");
        key(CDK_K_BS, 0, 0);
        eq_line("hell", "backspace");

        key(CDK_K_LEFT, 0, 0);
        key(CDK_K_LEFT, 0, 0);
        T_EQ_INT(SH.cursor, 2);
        type("X");
        eq_line("heXll", "途中に挿入");

        key(CDK_K_HOME, 0, 0);
        T_EQ_INT(SH.cursor, 0);
        key(CDK_K_END, 0, 0);
        T_EQ_INT(SH.cursor, 5);

        key(CDK_K_HOME, 0, 0);
        key(CDK_K_DELETE, 0, 0);
        eq_line("eXll", "delete は前へ詰める");
    }

    T_CASE("全角文字は 1 文字単位で消える");
    {
        setup();
        /* あい = 6 バイト */
        key(CDK_K_NONE, 0, 0x3042);
        key(CDK_K_NONE, 0, 0x3044);
        T_EQ_INT(SH.len, 6);
        key(CDK_K_BS, 0, 0);
        T_EQ_INT(SH.len, 3); /* 3 バイトまとめて消える */
        eq_line("\xE3\x81\x82", "あ だけ残る");

        /* 左右移動も 1 文字単位 */
        key(CDK_K_NONE, 0, 0x3044);
        key(CDK_K_LEFT, 0, 0);
        T_EQ_INT(SH.cursor, 3);
        key(CDK_K_RIGHT, 0, 0);
        T_EQ_INT(SH.cursor, 6);
    }

    T_CASE("Ctrl の行編集");
    {
        setup();
        type("one two three");
        key(CDK_K_NONE, CDK_MOD_CTRL_BIT, 'w');
        eq_line("one two ", "ctrl+w で直前の単語を消す");

        key(CDK_K_NONE, CDK_MOD_CTRL_BIT, 'a');
        T_EQ_INT(SH.cursor, 0);
        key(CDK_K_NONE, CDK_MOD_CTRL_BIT, 'e');
        T_EQ_INT(SH.cursor, 8);

        key(CDK_K_NONE, CDK_MOD_CTRL_BIT, 'u');
        eq_line("", "ctrl+u で行頭まで消す");

        setup();
        type("abcdef");
        key(CDK_K_LEFT, 0, 0);
        key(CDK_K_LEFT, 0, 0);
        key(CDK_K_NONE, CDK_MOD_CTRL_BIT, 'k');
        eq_line("abcd", "ctrl+k で行末まで消す");
    }

    T_CASE("Ctrl+C は行を捨てる");
    {
        setup();
        type("half typed");
        key(CDK_K_NONE, CDK_MOD_CTRL_BIT, 'c');
        eq_line("", "行が消える");
        T_CHECK(strstr(OUT, "^C") != NULL);
    }

    T_CASE("コマンドを実行する");
    {
        setup();
        type("echo hello world");
        key(CDK_K_ENTER, 0, 0);
        T_EQ_INT(ECHO_CALLS, 1);
        T_EQ_INT(LAST_ARGC, 3);
        T_CHECK(strcmp(LAST_CMD, "echo|hello|world") == 0);
        eq_line("", "実行後は行が空になる");
    }

    T_CASE("知らないコマンド");
    {
        setup();
        type("nosuch");
        key(CDK_K_ENTER, 0, 0);
        T_CHECK(strstr(OUT, "そんなコマンドはありません") != NULL);
        T_EQ_INT(SH.last_status, 127);
    }

    T_CASE("終了コードを覚えている");
    {
        setup();
        type("fail");
        key(CDK_K_ENTER, 0, 0);
        T_EQ_INT(SH.last_status, 3);
    }

    T_CASE("空行は何もしない");
    {
        setup();
        key(CDK_K_ENTER, 0, 0);
        T_EQ_INT(ECHO_CALLS, 0);
        T_CHECK(strstr(OUT, "そんなコマンド") == NULL);
    }

    T_CASE("履歴");
    {
        setup();
        type("echo one");
        key(CDK_K_ENTER, 0, 0);
        type("echo two");
        key(CDK_K_ENTER, 0, 0);

        key(CDK_K_UP, 0, 0);
        eq_line("echo two", "上で直前");
        key(CDK_K_UP, 0, 0);
        eq_line("echo one", "もう一度上で 2 つ前");
        key(CDK_K_UP, 0, 0);
        eq_line("echo one", "それ以上は遡らない");
        key(CDK_K_DOWN, 0, 0);
        eq_line("echo two", "下で戻る");
        key(CDK_K_DOWN, 0, 0);
        eq_line("", "編集中だった空行に戻る");
    }

    T_CASE("履歴を辿る前の入力が復元される");
    {
        setup();
        type("echo saved");
        key(CDK_K_ENTER, 0, 0);
        type("typing");
        key(CDK_K_UP, 0, 0);
        eq_line("echo saved", "履歴に移る");
        key(CDK_K_DOWN, 0, 0);
        eq_line("typing", "打ちかけが戻る");
    }

    T_CASE("同じコマンドを連続で積まない");
    {
        setup();
        type("echo x");
        key(CDK_K_ENTER, 0, 0);
        type("echo x");
        key(CDK_K_ENTER, 0, 0);
        T_EQ_INT(SH.hist_count, 1);
    }

    T_CASE("補完");
    {
        setup();
        type("ec");
        key(CDK_K_TAB, 0, 0);
        eq_line("echo ", "候補が 1 つなら確定して空白を足す");

        /* 候補が複数なら一覧を出して行はそのまま */
        setup();
        type("e");
        key(CDK_K_TAB, 0, 0);
        eq_line("e", "行は変わらない");
        T_CHECK(strstr(OUT, "echo") != NULL);
        T_CHECK(strstr(OUT, "exit") != NULL);
        T_CHECK(strstr(OUT, "expand") != NULL);

        /* 2 語目は補完しない */
        setup();
        type("echo ec");
        key(CDK_K_TAB, 0, 0);
        eq_line("echo ec", "引数は補完しない");
    }

    T_CASE("コマンドがシェルを止められる");
    {
        setup();
        type("exit");
        key(CDK_K_ENTER, 0, 0);
        T_CHECK(!cdk_shell_active(&SH));

        /* 止まったあとのキーは無視される */
        const size_t before = OUTLEN;
        type("ignored");
        T_EQ_INT(OUTLEN, before);
        eq_line("", "行は変わらない");
    }

    T_CASE("割り込みの知らせは打ちかけの行を残す");
    {
        /* WiFi の接続結果のように後から届く知らせ。**打っていた文字を
         * 消してはいけない。** 消えると打ち直しになる。 */
        setup();
        type("ssh ");
        OUTLEN = 0;
        OUT[0] = '\0';
        cdk_shell_notify(&SH, "wifi  : 接続 192.168.1.97");

        eq_line("ssh ", "行はそのまま");
        T_EQ_INT(SH.cursor, 4);
        T_CHECK(strstr(OUT, "wifi  : 接続 192.168.1.97") != NULL);
        /* 行頭へ戻して消してから書く（プロンプトの途中に挟まらない） */
        T_CHECK(strncmp(OUT, "\r\033[K", 3) == 0);
        /* 知らせのあとにプロンプトと行が描き直されている */
        T_CHECK(strstr(OUT, "ssh ") != NULL);
        T_CHECK(strstr(OUT, "ssh ") > strstr(OUT, "wifi  :"));
    }

    T_CASE("シェルが止まっている間は知らせを出さない");
    {
        /* SSH やエディタに入っている間に書くと、そちらの画面が壊れる。 */
        setup();
        cdk_shell_end(&SH);
        OUTLEN = 0;
        OUT[0] = '\0';
        cdk_shell_notify(&SH, "wifi  : 接続 192.168.1.97");
        T_EQ_INT((int)OUTLEN, 0);

        /* NULL でも落ちない */
        cdk_shell_begin(&SH);
        cdk_shell_notify(&SH, NULL);
    }

    T_CASE("行の分割");
    {
        char buf[128];
        char *argv[8];

        strcpy(buf, "a b c");
        T_EQ_INT(cdk_shell_split(buf, argv, 8), 3);
        T_CHECK(strcmp(argv[0], "a") == 0);
        T_CHECK(strcmp(argv[2], "c") == 0);

        strcpy(buf, "  spaced   out  ");
        T_EQ_INT(cdk_shell_split(buf, argv, 8), 2);
        T_CHECK(strcmp(argv[0], "spaced") == 0);
        T_CHECK(strcmp(argv[1], "out") == 0);

        strcpy(buf, "cmd \"hello world\" tail");
        T_EQ_INT(cdk_shell_split(buf, argv, 8), 3);
        T_CHECK(strcmp(argv[1], "hello world") == 0);
        T_CHECK(strcmp(argv[2], "tail") == 0);

        strcpy(buf, "cmd 'single quoted' x");
        T_EQ_INT(cdk_shell_split(buf, argv, 8), 3);
        T_CHECK(strcmp(argv[1], "single quoted") == 0);

        strcpy(buf, "");
        T_EQ_INT(cdk_shell_split(buf, argv, 8), 0);

        /* 引数の数に上限がある */
        strcpy(buf, "a b c d e f g h i j");
        T_EQ_INT(cdk_shell_split(buf, argv, 4), 4);
    }

    T_CASE("user@host を分ける");
    {
        char u[48];
        char h[80];

        T_CHECK(cdk_shell_split_userhost("alice@box", u, sizeof(u), h, sizeof(h)));
        T_EQ_STR(u, "alice");
        T_EQ_STR(h, "box");

        T_CHECK(cdk_shell_split_userhost("a@192.168.1.9", u, sizeof(u), h, sizeof(h)));
        T_EQ_STR(u, "a");
        T_EQ_STR(h, "192.168.1.9");

        /* **区切りは最後の `@`**（Linux の ssh と同じ）。 */
        T_CHECK(cdk_shell_split_userhost("a@b@c", u, sizeof(u), h, sizeof(h)));
        T_EQ_STR(u, "a@b");
        T_EQ_STR(h, "c");
    }

    T_CASE("user@host にならないものは断る");
    {
        char u[48];
        char h[80];
        /* **半端に埋めない。** ユーザーだけ取れたと誤解されると、
         * 知らない相手へ繋ぎに行く。 */
        T_CHECK(!cdk_shell_split_userhost("@box", u, sizeof(u), h, sizeof(h)));
        T_CHECK(!cdk_shell_split_userhost("alice@", u, sizeof(u), h, sizeof(h)));
        T_CHECK(!cdk_shell_split_userhost("@", u, sizeof(u), h, sizeof(h)));
        T_CHECK(!cdk_shell_split_userhost("", u, sizeof(u), h, sizeof(h)));
        T_CHECK(!cdk_shell_split_userhost("nohost", u, sizeof(u), h, sizeof(h)));
        T_CHECK(!cdk_shell_split_userhost(NULL, u, sizeof(u), h, sizeof(h)));
    }

    T_CASE("user@host の長さの境目");
    {
        char u[8];
        char h[8];
        /* ちょうど入る: 7 文字 + 終端 */
        T_CHECK(cdk_shell_split_userhost("1234567@abc", u, sizeof(u), h, sizeof(h)));
        T_EQ_STR(u, "1234567");
        /* 1 文字はみ出す。**切り詰めずに断る。** */
        T_CHECK(!cdk_shell_split_userhost("12345678@abc", u, sizeof(u), h, sizeof(h)));
        T_CHECK(cdk_shell_split_userhost("a@1234567", u, sizeof(u), h, sizeof(h)));
        T_EQ_STR(h, "1234567");
        T_CHECK(!cdk_shell_split_userhost("a@12345678", u, sizeof(u), h, sizeof(h)));
    }

    T_CASE("表示幅の計算");
    {
        /* 色付きプロンプトの幅を間違えるとカーソルがずれる。
         * 実機で 7 桁ずれて初めて気づいた不具合なので、ここで固定する。 */
        T_EQ_INT(cdk_shell_visible_width("abc", 3), 3);
        T_EQ_INT(cdk_shell_visible_width("\033[33m$\033[0m ", 12), 2);
        T_EQ_INT(cdk_shell_visible_width("\033[1;36m", 7), 0);
        /* 全角は 2 桁 */
        T_EQ_INT(cdk_shell_visible_width("\xE3\x81\x82", 3), 2);
        T_EQ_INT(cdk_shell_visible_width("a\xE3\x81\x82" "b", 5), 4);
        /* OSC も飛ばす */
        T_EQ_INT(cdk_shell_visible_width("\033]0;title\007x", 11), 1);
        /* 途中で切れていても落ちない */
        T_EQ_INT(cdk_shell_visible_width("\033[3", 3), 0);
        T_EQ_INT(cdk_shell_visible_width("", 0), 0);
    }

    T_CASE("カーソルが正しい桁に置かれる");
    {
        /* redraw の最後に出す ESC[<n>C の n を見る。
         * プロンプト "$ " は 2 桁なので、行頭なら 2、1 文字目の後なら 3。 */
        setup();
        OUTLEN = 0;
        OUT[0] = '\0';
        type("X");
        const char *last = strrchr(OUT, '\033');
        t_checks++;
        if (last == NULL || strncmp(last, "\033[3C", 4) != 0) {
            t_failures++;
            printf("   FAIL カーソル位置: 末尾が ESC[3C ではない\n");
        }

        /* 行頭へ戻すと 2 桁目 */
        OUTLEN = 0;
        OUT[0] = '\0';
        key(CDK_K_HOME, 0, 0);
        last = strrchr(OUT, '\033');
        t_checks++;
        if (last == NULL || strncmp(last, "\033[2C", 4) != 0) {
            t_failures++;
            printf("   FAIL 行頭のカーソル位置: 末尾が ESC[2C ではない\n");
        }
    }

    T_CASE("行が長すぎても壊れない");
    {
        setup();
        for (int i = 0; i < CDK_SHELL_LINE_MAX * 2; i++) {
            key(CDK_K_NONE, 0, 'x');
        }
        T_CHECK(SH.len < CDK_SHELL_LINE_MAX);
        T_EQ_INT((int)strlen(SH.line), SH.len);
    }

    return t_report("test_shell");
}
