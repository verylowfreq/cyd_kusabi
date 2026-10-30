/* cdk_shell の単体テスト。
 *
 * 行編集は「出力バイト列」ではなく「編集後の行の中身」で確認する。
 * 描き直しの仕方は実装の都合で変わるが、行の中身とカーソル位置は仕様だから。
 */
#include "cdk_shell.h"
#include "cdk_term.h"
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

/* 伏せ字入力の受け取り先 */
static char ASK_ANSWER[256];
static int  ASK_CALLS;
static bool ASK_CANCELLED;
static const char *PROMPT_BEFORE;

static void on_ask(void *user, const char *answer)
{
    (void)user;
    ASK_CALLS++;
    ASK_CANCELLED = (answer == NULL);
    ASK_ANSWER[0] = '\0';
    if (answer != NULL) {
        strncpy(ASK_ANSWER, answer, sizeof(ASK_ANSWER) - 1);
        ASK_ANSWER[sizeof(ASK_ANSWER) - 1] = '\0';
    }
}

static void setup(void)
{
    OUTLEN = 0;
    OUT[0] = '\0';
    LAST_CMD[0] = '\0';
    LAST_ARGC = 0;
    ECHO_CALLS = 0;
    ASK_CALLS = 0;
    ASK_CANCELLED = false;
    ASK_ANSWER[0] = '\0';
    cdk_shell_init(&SH, on_out, NULL);
    cdk_shell_set_commands(&SH, CMDS, sizeof(CMDS) / sizeof(CMDS[0]), &SH);
    cdk_shell_begin(&SH);
    PROMPT_BEFORE = SH.prompt;
}

/* 伏せ字入力を始める。**呼び出し回数は消さない**（1 つのケースで
 * 何度も聞いて、そのたびに cb が来たかを数えるため）。 */
static void ask_setup(int maxlen)
{
    OUTLEN = 0;
    OUT[0] = '\0';
    cdk_shell_ask(&SH, "pass: ", maxlen, on_ask, NULL);
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

/* ---- 本物の端末に流して、画面に何が残るかで見る ----
 *
 * 折り返しの描き直しは「行の中身」では確かめられない。中身が正しくても、
 * 上の段が消えずに残る（実機で 40 桁を超えたコマンドが 1 字ごとに 1 段
 * 増えた）。cdk_term は実機と同じコードなので、ここで残れば実機でも残る。 */
#define TCOLS 20
#define TROWS 8
static cdk_term_t TERM;
static uint8_t    TERMMEM[64 * 1024];
static char       ROWBUF[TCOLS * 4 + 1];

static void on_term_out(void *user, const char *data, size_t len)
{
    (void)user;
    cdk_term_feed(&TERM, data, len);
}

static void term_setup(void)
{
    T_CHECK(cdk_term_mem_size(TCOLS, TROWS) <= sizeof(TERMMEM));
    T_CHECK(cdk_term_init(&TERM, TCOLS, TROWS, TERMMEM, sizeof(TERMMEM)));
    cdk_shell_init(&SH, on_term_out, NULL);
    cdk_shell_set_commands(&SH, CMDS, sizeof(CMDS) / sizeof(CMDS[0]), &SH);
    cdk_shell_set_cols(&SH, TCOLS);
    cdk_shell_begin(&SH);
}

/* 末尾の空白を詰めた段 */
static const char *row(int y)
{
    cdk_term_row_text(&TERM, y, ROWBUF, sizeof(ROWBUF));
    size_t n = strlen(ROWBUF);
    while (n > 0 && ROWBUF[n - 1] == ' ') {
        ROWBUF[--n] = '\0';
    }
    return ROWBUF;
}

static void eq_row(int y, const char *want)
{
    const char *got = row(y);
    t_checks++;
    if (strcmp(got, want) != 0) {
        t_failures++;
        printf("   FAIL [%s] 段 %d\n     期待 [%s]\n     実際 [%s]\n", t_current, y,
               want, got);
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

    T_CASE("scp の引数を分解する");
    {
        cdk_scp_args_t g;

        T_EQ_INT(cdk_shell_scp_parse("a.txt", "alice@box:/tmp/a.txt", &g),
                 CDK_SCP_OK);
        T_CHECK(g.upload);
        T_EQ_STR(g.user, "alice");
        T_EQ_STR(g.host, "box");
        T_EQ_STR(g.remote, "/tmp/a.txt");
        T_EQ_STR(g.local, "a.txt");
        T_CHECK(!g.remote_is_empty);

        T_EQ_INT(cdk_shell_scp_parse("alice@box:/tmp/a.txt", "a.txt", &g),
                 CDK_SCP_OK);
        T_CHECK(!g.upload);
        T_EQ_STR(g.remote, "/tmp/a.txt");
        T_EQ_STR(g.local, "a.txt");

        /* **区切りは最初の `:`。** それより後ろは向こうのパスで、
         * `:` を含んでよい（Linux も同じ）。 */
        T_EQ_INT(cdk_shell_scp_parse("a.txt", "alice@box:/tmp/x:y", &g),
                 CDK_SCP_OK);
        T_EQ_STR(g.remote, "/tmp/x:y");

        /* ホストは最後の `@` で切る（`ssh` と揃える）。 */
        T_EQ_INT(cdk_shell_scp_parse("a.txt", "a@b@c:/p", &g), CDK_SCP_OK);
        T_EQ_STR(g.user, "a@b");
        T_EQ_STR(g.host, "c");

        /* 送信でパスが空なら、手元の名前を使う合図を立てる。 */
        T_EQ_INT(cdk_shell_scp_parse("notes.txt", "alice@box:", &g), CDK_SCP_OK);
        T_CHECK(g.remote_is_empty);
        T_EQ_STR(g.remote, "");
    }

    T_CASE("scp: `/` より前の `:` だけが向こう側");
    {
        /* **コロンを含む手元のファイルを送れなくしない。** */
        cdk_scp_args_t g;
        T_EQ_INT(cdk_shell_scp_parse("/sdcard/a:b", "c.txt", &g),
                 CDK_SCP_E_BOTH_LOCAL);
        T_EQ_INT(cdk_shell_scp_parse("./a:b", "c.txt", &g),
                 CDK_SCP_E_BOTH_LOCAL);
        T_EQ_INT(cdk_shell_scp_parse("a/b:c", "c.txt", &g),
                 CDK_SCP_E_BOTH_LOCAL);
        /* 逆に、`/` より前にあれば向こう側 */
        T_EQ_INT(cdk_shell_scp_parse("x.txt", "alice@box:a/b:c", &g), CDK_SCP_OK);
        T_EQ_STR(g.remote, "a/b:c");
    }

    T_CASE("scp: 断る形");
    {
        cdk_scp_args_t g;
        T_EQ_INT(cdk_shell_scp_parse("a.txt", "b.txt", &g), CDK_SCP_E_BOTH_LOCAL);
        T_EQ_INT(cdk_shell_scp_parse("a@x:/p", "b@y:/q", &g),
                 CDK_SCP_E_BOTH_REMOTE);
        /* **`user@` が無いものは断る。** この機械に手元のユーザー名は無い。 */
        T_EQ_INT(cdk_shell_scp_parse("a.txt", "box:/tmp/a", &g),
                 CDK_SCP_E_NO_USER);
        T_EQ_INT(cdk_shell_scp_parse("box:/tmp/a", "a.txt", &g),
                 CDK_SCP_E_NO_USER);
        /* 受信元のパスが空だと、何を取るか決まらない */
        T_EQ_INT(cdk_shell_scp_parse("alice@box:", "a.txt", &g),
                 CDK_SCP_E_NO_REMOTE);
        T_EQ_INT(cdk_shell_scp_parse("", "alice@box:/p", &g),
                 CDK_SCP_E_NO_LOCAL);
        T_EQ_INT(cdk_shell_scp_parse("a.txt", "@box:/p", &g),
                 CDK_SCP_E_USERHOST);
        T_EQ_INT(cdk_shell_scp_parse("a.txt", "alice@:/p", &g),
                 CDK_SCP_E_USERHOST);
    }

    T_CASE("scp: 断ったときは out を触らない");
    {
        /* **半端に埋めない。** 宛先が取れたと誤解されると、別の相手へ
         * ファイルを送ることになる（split_userhost と同じ理由）。 */
        cdk_scp_args_t g;
        memset(&g, 0xAA, sizeof(g));
        cdk_scp_args_t before = g;

        T_EQ_INT(cdk_shell_scp_parse("a.txt", "box:/p", &g), CDK_SCP_E_NO_USER);
        T_EQ_MEM(&g, &before, sizeof(g));
        T_EQ_INT(cdk_shell_scp_parse("a.txt", "b.txt", &g), CDK_SCP_E_BOTH_LOCAL);
        T_EQ_MEM(&g, &before, sizeof(g));
        T_EQ_INT(cdk_shell_scp_parse("a@x:/p", "b@y:/q", &g),
                 CDK_SCP_E_BOTH_REMOTE);
        T_EQ_MEM(&g, &before, sizeof(g));
        T_EQ_INT(cdk_shell_scp_parse("a.txt", "@box:/p", &g),
                 CDK_SCP_E_USERHOST);
        T_EQ_MEM(&g, &before, sizeof(g));

        /* NULL でも落ちない */
        T_CHECK(cdk_shell_scp_parse(NULL, "a@b:/p", &g) != CDK_SCP_OK);
        T_CHECK(cdk_shell_scp_parse("a", NULL, &g) != CDK_SCP_OK);
        T_CHECK(cdk_shell_scp_parse("a", "a@b:/p", NULL) != CDK_SCP_OK);
    }

    T_CASE("scp: 長さの境目は切り詰めずに断る");
    {
        cdk_scp_args_t g;
        char buf[400];

        /* remote は 192 バイト。191 文字ちょうどは入る */
        memset(buf, 'r', sizeof(buf));
        snprintf(buf, sizeof(buf), "a@b:");
        memset(buf + 4, 'r', 191);
        buf[4 + 191] = '\0';
        T_EQ_INT(cdk_shell_scp_parse("x", buf, &g), CDK_SCP_OK);
        T_EQ_INT((int)strlen(g.remote), 191);

        /* 1 文字はみ出したら断る */
        memset(buf + 4, 'r', 192);
        buf[4 + 192] = '\0';
        T_EQ_INT(cdk_shell_scp_parse("x", buf, &g), CDK_SCP_E_TOOLONG);

        /* local も 192 バイト */
        memset(buf, 'l', 191);
        buf[191] = '\0';
        T_EQ_INT(cdk_shell_scp_parse(buf, "a@b:/p", &g), CDK_SCP_OK);
        memset(buf, 'l', 192);
        buf[192] = '\0';
        T_EQ_INT(cdk_shell_scp_parse(buf, "a@b:/p", &g), CDK_SCP_E_TOOLONG);
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

    /* ---- 伏せ字入力（ssh のパスワード）---- */

    T_CASE("伏せ字は画面に出ない。答えは Enter で渡る");
    {
        setup();
        ask_setup(16);
        type("hunter2");
        /* 打った字がどこにも出ていないこと。**ここが本題。** */
        T_CHECK(strstr(OUT, "hunter2") == NULL);
        T_CHECK(strstr(OUT, "*******") != NULL);
        T_CHECK(SH.asking);

        key(CDK_K_ENTER, 0, 0);
        T_EQ_INT(ASK_CALLS, 1);
        T_EQ_STR(ASK_ANSWER, "hunter2");
        T_CHECK(!ASK_CANCELLED);
        T_CHECK(!SH.asking);
        /* 渡したあとは器に残さない */
        T_EQ_INT(SH.len, 0);
        T_EQ_STR(SH.line, "");
        /* 履歴にも積まない */
        T_EQ_INT(SH.hist_count, 0);
        /* プロンプトは元に戻る */
        T_CHECK(SH.prompt == PROMPT_BEFORE);
    }

    T_CASE("伏せ字の行編集はバックスペースと Ctrl-U だけ");
    {
        setup();
        ask_setup(16);
        type("abc");
        key(CDK_K_BS, 0, 0);
        T_EQ_INT(SH.len, 2);

        /* 履歴も補完もカーソル移動も効かない */
        key(CDK_K_UP, 0, 0);
        key(CDK_K_TAB, 0, 0);
        key(CDK_K_LEFT, 0, 0);
        key(CDK_K_HOME, 0, 0);
        T_EQ_INT(SH.len, 2);
        T_EQ_INT(SH.cursor, 2);

        key(CDK_K_NONE, CDK_MOD_CTRL_BIT, 'u');
        T_EQ_INT(SH.len, 0);
        type("x y");
        T_EQ_INT(SH.len, 3); /* 空白も合言葉のうち */

        key(CDK_K_ENTER, 0, 0);
        T_EQ_STR(ASK_ANSWER, "x y");
    }

    T_CASE("Ctrl-C・Esc・空の Enter はやめる");
    {
        setup();
        ask_setup(16);
        type("abc");
        key(CDK_K_NONE, CDK_MOD_CTRL_BIT, 'c');
        T_EQ_INT(ASK_CALLS, 1);
        T_CHECK(ASK_CANCELLED);
        T_CHECK(!SH.asking);

        ask_setup(16);
        type("abc");
        key(CDK_K_ESC, 0, 0);
        T_EQ_INT(ASK_CALLS, 2);
        T_CHECK(ASK_CANCELLED);

        ask_setup(16);
        key(CDK_K_ENTER, 0, 0);
        T_EQ_INT(ASK_CALLS, 3);
        T_CHECK(ASK_CANCELLED);
    }

    T_CASE("長すぎるときは切り詰めずにやめる");
    {
        setup();
        ask_setup(8); /* 8 バイトまで */
        type("123456789012");
        T_EQ_INT(SH.len, 8);
        key(CDK_K_ENTER, 0, 0);
        T_EQ_INT(ASK_CALLS, 1);
        T_CHECK(ASK_CANCELLED); /* 切り詰めた 8 バイトは渡さない */
        T_CHECK(strstr(OUT, "長すぎます") != NULL);

        /* Ctrl-U で打ち直せば通る */
        ask_setup(8);
        type("123456789");
        key(CDK_K_NONE, CDK_MOD_CTRL_BIT, 'u');
        type("ok");
        key(CDK_K_ENTER, 0, 0);
        T_CHECK(!ASK_CANCELLED);
        T_EQ_STR(ASK_ANSWER, "ok");
    }

    T_CASE("全角は 1 文字 1 つの伏せ字。消すのも 1 文字ずつ");
    {
        setup();
        ask_setup(32);
        key(CDK_K_NONE, 0, 0x3042); /* あ */
        key(CDK_K_NONE, 0, 0x3044); /* い */
        T_EQ_INT(SH.len, 6);
        T_CHECK(strstr(OUT, "**") != NULL);
        T_CHECK(strstr(OUT, "***") == NULL);
        key(CDK_K_BS, 0, 0);
        T_EQ_INT(SH.len, 3);
    }

    T_CASE("割り込みの知らせが伏せ字を漏らさない");
    {
        setup();
        ask_setup(16);
        type("hunter2");
        OUTLEN = 0;
        OUT[0] = '\0';
        cdk_shell_notify(&SH, "wifi  : 接続 192.168.1.2");
        T_CHECK(strstr(OUT, "hunter2") == NULL);
        T_CHECK(strstr(OUT, "*******") != NULL);
    }

    T_CASE("シェルを止める・始めると聞くのをやめる");
    {
        setup();
        ask_setup(16);
        type("abc");
        cdk_shell_end(&SH);
        T_CHECK(!SH.asking);
        T_EQ_INT(ASK_CALLS, 0); /* 黙ってやめる。cb は呼ばない */
        T_EQ_INT(SH.len, 0);    /* 打ちかけは残さない */
        T_CHECK(SH.prompt == PROMPT_BEFORE);

        cdk_shell_begin(&SH);
        ask_setup(16);
        type("abc");
        cdk_shell_begin(&SH); /* SSH が切れて戻ってきた、など */
        T_CHECK(!SH.asking);
        T_EQ_INT(ASK_CALLS, 0);
        T_EQ_INT(SH.len, 0);
    }

    /* ---- uart の引数 ---- */

    T_CASE("uart: 既定は 115200・フロー制御なし");
    {
        char *av[] = {"uart"};
        cdk_uart_args_t g;
        const char *why = "触られていない";
        T_CHECK(cdk_shell_uart_parse(1, av, &g, &why));
        T_EQ_INT(g.baud, 115200);
        T_CHECK(!g.flow);
        T_CHECK(why == NULL); /* 通ったら理由は消す */
    }

    T_CASE("uart: 速度と旗を読む");
    {
        cdk_uart_args_t g;
        char *a1[] = {"uart", "9600"};
        T_CHECK(cdk_shell_uart_parse(2, a1, &g, NULL));
        T_EQ_INT(g.baud, 9600);
        T_CHECK(!g.flow);

        char *a2[] = {"uart", "115200", "--flow"};
        T_CHECK(cdk_shell_uart_parse(3, a2, &g, NULL));
        T_EQ_INT(g.baud, 115200);
        T_CHECK(g.flow);

        char *a3[] = {"uart", "-f"};
        T_CHECK(cdk_shell_uart_parse(2, a3, &g, NULL));
        T_EQ_INT(g.baud, 115200); /* 旗だけでも既定の速度で開く */
        T_CHECK(g.flow);

        /* 順番はどちらが先でもよい */
        char *a4[] = {"uart", "--flow", "4800"};
        T_CHECK(cdk_shell_uart_parse(3, a4, &g, NULL));
        T_EQ_INT(g.baud, 4800);
        T_CHECK(g.flow);

        /* 後ろの --noflow が勝つ。明示で消せる */
        char *a5[] = {"uart", "-f", "--noflow"};
        T_CHECK(cdk_shell_uart_parse(3, a5, &g, NULL));
        T_CHECK(!g.flow);
    }

    T_CASE("uart: 速度の範囲と形");
    {
        cdk_uart_args_t g;
        char *lo[] = {"uart", "299"};
        char *hi[] = {"uart", "3000001"};
        char *ok_lo[] = {"uart", "300"};
        char *ok_hi[] = {"uart", "3000000"};
        T_CHECK(!cdk_shell_uart_parse(2, lo, &g, NULL));
        T_CHECK(!cdk_shell_uart_parse(2, hi, &g, NULL));
        T_CHECK(cdk_shell_uart_parse(2, ok_lo, &g, NULL));
        T_CHECK(cdk_shell_uart_parse(2, ok_hi, &g, NULL));

        /* **`atoi` だと "9600x" が 9600 で通ってしまう。** 打ち間違いが
         * 黙って別の速度になるので、数字以外が混じったら断る。 */
        char *junk[] = {"uart", "9600x"};
        char *neg[] = {"uart", "-9600"};
        char *empty[] = {"uart", ""};
        char *huge[] = {"uart", "99999999999999999999"};
        T_CHECK(!cdk_shell_uart_parse(2, junk, &g, NULL));
        T_CHECK(!cdk_shell_uart_parse(2, neg, &g, NULL)); /* 旗として断る */
        T_CHECK(!cdk_shell_uart_parse(2, empty, &g, NULL));
        T_CHECK(!cdk_shell_uart_parse(2, huge, &g, NULL)); /* 溢れさせない */
    }

    T_CASE("uart: 知らない指定と速度の重複は断る");
    {
        cdk_uart_args_t g;
        const char *why = NULL;
        char *unk[] = {"uart", "--fast"};
        T_CHECK(!cdk_shell_uart_parse(2, unk, &g, &why));
        T_CHECK(why != NULL); /* 人に見せる理由が入る */

        char *two[] = {"uart", "9600", "115200"};
        T_CHECK(!cdk_shell_uart_parse(3, two, &g, NULL));

        char *nul[] = {"uart", NULL};
        T_CHECK(!cdk_shell_uart_parse(2, nul, &g, NULL));
        T_CHECK(!cdk_shell_uart_parse(1, NULL, &g, NULL));

        cdk_uart_args_t *nowhere = NULL;
        T_CHECK(!cdk_shell_uart_parse(1, unk, nowhere, NULL));
    }

    T_CASE("uart: 断ったときは out を触らない");
    {
        /* scp と同じ理由。半端に埋めると、速度だけ取れたと誤解されて
         * 違う速度で開くことになる。 */
        cdk_uart_args_t g;
        memset(&g, 0xAA, sizeof(g));
        const cdk_uart_args_t before = g;

        char *unk[] = {"uart", "--fast"};
        T_CHECK(!cdk_shell_uart_parse(2, unk, &g, NULL));
        T_EQ_MEM(&g, &before, sizeof(g));

        char *bad[] = {"uart", "9600x"};
        T_CHECK(!cdk_shell_uart_parse(2, bad, &g, NULL));
        T_EQ_MEM(&g, &before, sizeof(g));
    }

    T_CASE("wget: URL を確かめる");
    {
        cdk_wget_args_t g;
        T_EQ_INT(cdk_shell_wget_parse("http://example.com/a.txt", &g), CDK_WGET_OK);
        T_CHECK(!g.https);
        T_EQ_STR(g.url, "http://example.com/a.txt");
        T_EQ_INT(cdk_shell_wget_parse("HTTPS://example.com", &g), CDK_WGET_OK);
        T_CHECK(g.https);
        T_EQ_INT(cdk_shell_wget_parse("https://u@h:8443/x", &g), CDK_WGET_OK);

        T_EQ_INT(cdk_shell_wget_parse("ftp://example.com/a", &g), CDK_WGET_E_SCHEME);
        T_EQ_INT(cdk_shell_wget_parse("example.com/a", &g), CDK_WGET_E_SCHEME);
        T_EQ_INT(cdk_shell_wget_parse("http:/example.com", &g), CDK_WGET_E_SCHEME);
        T_EQ_INT(cdk_shell_wget_parse("http://", &g), CDK_WGET_E_NO_HOST);
        T_EQ_INT(cdk_shell_wget_parse("http:///a", &g), CDK_WGET_E_NO_HOST);
        T_EQ_INT(cdk_shell_wget_parse("http://u@/a", &g), CDK_WGET_E_NO_HOST);
        T_EQ_INT(cdk_shell_wget_parse("http://:80/a", &g), CDK_WGET_E_NO_HOST);
        T_EQ_INT(cdk_shell_wget_parse("http://h/a b", &g), CDK_WGET_E_BADCHAR);

        char long_url[CDK_WGET_URL_MAX + 8];
        memset(long_url, 'a', sizeof(long_url));
        memcpy(long_url, "http://h/", 9);
        long_url[sizeof(long_url) - 1] = '\0';
        T_EQ_INT(cdk_shell_wget_parse(long_url, &g), CDK_WGET_E_TOOLONG);

        /* 断ったときは out を触らない */
        memset(&g, 0xAA, sizeof(g));
        const cdk_wget_args_t before = g;
        T_CHECK(cdk_shell_wget_parse("gopher://h/", &g) != CDK_WGET_OK);
        T_EQ_MEM(&g, &before, sizeof(g));
    }

    T_CASE("wget: URL から保存名を作る");
    {
        char n[64];
        T_CHECK(cdk_shell_wget_name("http://h/dir/a.txt", n, sizeof(n)));
        T_EQ_STR(n, "a.txt");
        T_CHECK(cdk_shell_wget_name("http://h/a.txt?x=1#top", n, sizeof(n)));
        T_EQ_STR(n, "a.txt");
        /* 名前が無ければ index.html */
        T_CHECK(cdk_shell_wget_name("http://h", n, sizeof(n)));
        T_EQ_STR(n, "index.html");
        T_CHECK(cdk_shell_wget_name("http://h/", n, sizeof(n)));
        T_EQ_STR(n, "index.html");
        T_CHECK(cdk_shell_wget_name("http://h/dir/", n, sizeof(n)));
        T_EQ_STR(n, "index.html");
        T_CHECK(cdk_shell_wget_name("http://h?q=/a.txt", n, sizeof(n)));
        T_EQ_STR(n, "index.html");
        T_CHECK(cdk_shell_wget_name("http://h/..", n, sizeof(n)));
        T_EQ_STR(n, "index.html");
        /* %XX を復号する。UTF-8 はそのまま使う */
        T_CHECK(cdk_shell_wget_name("http://h/%E3%81%82.txt", n, sizeof(n)));
        T_EQ_STR(n, "\xE3\x81\x82.txt");
        /* FAT で使えない文字は _。%2F で / を作っても階層にしない */
        T_CHECK(cdk_shell_wget_name("http://h/a:b*c%3F%22%3C%3E%7C%5C%2Fd", n, sizeof(n)));
        T_EQ_STR(n, "a_b_c_______d");
        T_CHECK(cdk_shell_wget_name("http://h/a%00b%0Ac", n, sizeof(n)));
        T_EQ_STR(n, "a_b_c");
        /* 壊れた % はそのまま、壊れた UTF-8 は _ */
        T_CHECK(cdk_shell_wget_name("http://h/100%25%zz%4", n, sizeof(n)));
        T_EQ_STR(n, "100%%zz%4");
        T_CHECK(cdk_shell_wget_name("http://h/a%FFb%E3%81", n, sizeof(n)));
        T_EQ_STR(n, "a_b__");
        /* 先頭の空白、末尾の . と空白は削る */
        T_CHECK(cdk_shell_wget_name("http://h/%20%20a.txt.%20.", n, sizeof(n)));
        T_EQ_STR(n, "a.txt");
        T_CHECK(cdk_shell_wget_name("http://h/.bashrc", n, sizeof(n)));
        T_EQ_STR(n, ".bashrc");

        /* 長ければ拡張子を残し、文字境界で切る */
        char small[12]; /* 11 バイトまで */
        T_CHECK(cdk_shell_wget_name("http://h/abcdefghijklmnop.txt", small, sizeof(small)));
        T_EQ_STR(small, "abcdefg.txt");
        /* あ(3) × 4 + .txt → 前半 7 バイトに「あ」は 2 つしか入らない */
        T_CHECK(cdk_shell_wget_name("http://h/%E3%81%82%E3%81%82%E3%81%82%E3%81%82.txt",
                                    small, sizeof(small)));
        T_EQ_STR(small, "\xE3\x81\x82\xE3\x81\x82.txt");
        /* 拡張子が長すぎれば、ただ切る */
        T_CHECK(cdk_shell_wget_name("http://h/ab.cdefghijklmnopqrstuvwxyz", small,
                                    sizeof(small)));
        T_EQ_STR(small, "ab.cdefghij");

        /* 255 バイトを超えない */
        char url[700];
        char big[400];
        snprintf(url, sizeof(url), "http://h/");
        memset(url + 9, 'x', 300);
        memcpy(url + 309, ".bin", 5);
        T_CHECK(cdk_shell_wget_name(url, big, sizeof(big)));
        T_EQ_INT((int)strlen(big), 255);
        T_CHECK(strcmp(big + 251, ".bin") == 0);

        char tiny[8];
        T_CHECK(!cdk_shell_wget_name("http://h/a", tiny, sizeof(tiny)));
    }

    /* ---- 桁を超えて折り返した行（20 桁。プロンプト `$ ` で 1 段目は 18 字）---- */
    char a18[19], a12[13], a17[18];
    memset(a18, 'a', 18);
    a18[18] = '\0';
    memset(a12, 'a', 12);
    a12[12] = '\0';
    memset(a17, 'a', 17);
    a17[17] = '\0';
    char p18[24], p17[24];
    snprintf(p18, sizeof(p18), "$ %s", a18);
    snprintf(p17, sizeof(p17), "$ %s", a17);

    T_CASE("折り返し: 打つたびに段が増えない");
    {
        term_setup();
        type("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"); /* 30 字 */
        eq_row(0, p18);
        eq_row(1, a12);
        eq_row(2, ""); /* 前は 1 字ごとに 1 段ずつ上に残った */
        T_EQ_INT(TERM.cy, 1);
        T_EQ_INT(TERM.cx, 12);
    }

    T_CASE("折り返し: 消して 1 段に戻ると下の段も消える");
    {
        term_setup();
        type("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        for (int i = 0; i < 13; i++) {
            key(CDK_K_BS, 0, 0);
        }
        eq_row(0, p17);
        eq_row(1, "");
        T_EQ_INT(TERM.cy, 0);
        T_EQ_INT(TERM.cx, 19);
    }

    T_CASE("折り返し: ちょうど右端で終わったらカーソルは次の段の頭");
    {
        term_setup();
        type(a18);
        eq_row(0, p18);
        T_EQ_INT(TERM.cy, 1);
        T_EQ_INT(TERM.cx, 0);
        type("b");
        eq_row(1, "b");
        T_EQ_INT(TERM.cx, 1);
        key(CDK_K_BS, 0, 0);
        key(CDK_K_BS, 0, 0);
        eq_row(0, p17);
        eq_row(1, "");
        T_EQ_INT(TERM.cy, 0);
        T_EQ_INT(TERM.cx, 19);
    }

    T_CASE("折り返し: 1 段目に戻って書き足す");
    {
        term_setup();
        type("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        key(CDK_K_NONE, CDK_MOD_CTRL_BIT, 'a');
        T_EQ_INT(TERM.cy, 0);
        T_EQ_INT(TERM.cx, 2);
        type("X");
        eq_row(0, "$ Xaaaaaaaaaaaaaaaaa");
        eq_row(1, "aaaaaaaaaaaaa");
        eq_row(2, "");
        T_EQ_INT(TERM.cy, 0);
        T_EQ_INT(TERM.cx, 3);
        key(CDK_K_NONE, CDK_MOD_CTRL_BIT, 'e');
        T_EQ_INT(TERM.cy, 1);
        T_EQ_INT(TERM.cx, 13);
    }

    T_CASE("折り返し: 右端に入らない全角は次の段へ（カーソルも）");
    {
        term_setup();
        type(a17);
        key(CDK_K_NONE, 0, 0x3042); /* あ */
        eq_row(0, p17);
        eq_row(1, "\xE3\x81\x82");
        T_EQ_INT(TERM.cy, 1);
        T_EQ_INT(TERM.cx, 2);
        key(CDK_K_LEFT, 0, 0); /* あ の上 = 次の段の頭。右端の空きではない */
        T_EQ_INT(TERM.cy, 1);
        T_EQ_INT(TERM.cx, 0);
    }

    T_CASE("折り返し: 1 段目にカーソルがあっても出力は行の下から");
    {
        term_setup();
        type("echo bbbbbbbbbbbbbbbbbbbb"); /* 25 字 = 2 段 */
        key(CDK_K_NONE, CDK_MOD_CTRL_BIT, 'a');
        key(CDK_K_ENTER, 0, 0);
        T_EQ_INT(ECHO_CALLS, 1);
        eq_row(0, "$ echo bbbbbbbbbbbbb");
        eq_row(1, "bbbbbbb");
        eq_row(2, "$");
        T_EQ_INT(TERM.cy, 2);
    }

    T_CASE("折り返し: Ctrl-C の ^C は行の末尾に付く");
    {
        term_setup();
        type("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        key(CDK_K_NONE, CDK_MOD_CTRL_BIT, 'a');
        key(CDK_K_NONE, CDK_MOD_CTRL_BIT, 'c');
        eq_row(0, p18);
        eq_row(1, "aaaaaaaaaaaa^C");
        eq_row(2, "$");
    }

    T_CASE("折り返し: 知らせは行の全段を消してから書く");
    {
        term_setup();
        type("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        key(CDK_K_NONE, CDK_MOD_CTRL_BIT, 'a'); /* 1 段目にいても */
        cdk_shell_notify(&SH, "hello");
        eq_row(0, "hello");
        eq_row(1, p18);
        eq_row(2, a12);
        eq_row(3, "");
        T_EQ_INT(TERM.cy, 1);
        T_EQ_INT(TERM.cx, 2);
    }

    T_CASE("外から書くとき: プロンプトの後ろに続けず、終わったら出し直す");
    {
        /* 再接続の知らせ（main.c が term_printf で直に書く）。前は
         * `$ ssh   : 再接続 1 回目...` と続いて、プロンプトが消えたままだった。 */
        term_setup();
        cdk_shell_hide_line(&SH);
        cdk_term_feed(&TERM, "msg1\r\n", 6);
        cdk_term_feed(&TERM, "msg2\r\n", 6);
        cdk_shell_show_line(&SH);
        eq_row(0, "msg1");
        eq_row(1, "msg2");
        eq_row(2, "$");
        T_EQ_INT(TERM.cy, 2);
        T_EQ_INT(TERM.cx, 2);
    }

    T_CASE("外から書くとき: 折り返した打ちかけの行も全段どけて、下に描き直す");
    {
        term_setup();
        type("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        key(CDK_K_NONE, CDK_MOD_CTRL_BIT, 'a');
        cdk_shell_hide_line(&SH);
        cdk_term_feed(&TERM, "msg\r\n", 5);
        cdk_shell_show_line(&SH);
        eq_row(0, "msg");
        eq_row(1, p18);
        eq_row(2, a12);
        eq_row(3, "");
        T_EQ_INT(TERM.cy, 1);
        T_EQ_INT(TERM.cx, 2); /* カーソルの位置も保つ */
        type("X"); /* そのあとの描き直しも正しい段から */
        eq_row(1, "$ Xaaaaaaaaaaaaaaaaa");
        eq_row(2, "aaaaaaaaaaaaa");
        eq_row(3, "");
    }

    T_CASE("外から書くとき: シェルが止まっていれば何もしない");
    {
        term_setup();
        cdk_shell_end(&SH);
        OUTLEN = 0;
        cdk_term_feed(&TERM, "\033[2J\033[H", 7);
        cdk_shell_hide_line(&SH);
        cdk_shell_show_line(&SH);
        eq_row(0, ""); /* SSH の画面にプロンプトを描かない */
    }

    T_CASE("折り返し: 画面の最下段で折り返して流れても戻れる");
    {
        term_setup();
        for (int i = 0; i < TROWS - 1; i++) {
            key(CDK_K_ENTER, 0, 0);
        }
        T_EQ_INT(TERM.cy, TROWS - 1);
        type("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        eq_row(TROWS - 2, p18);
        eq_row(TROWS - 1, a12);
        for (int i = 0; i < 13; i++) {
            key(CDK_K_BS, 0, 0);
        }
        eq_row(TROWS - 2, p17);
        eq_row(TROWS - 1, "");
        eq_row(TROWS - 3, "$"); /* 上の空のプロンプトは消していない */
    }

    T_CASE("折り返し: 伏せ字も折り返して描き直せる");
    {
        term_setup();
        cdk_shell_ask(&SH, "pw: ", 64, on_ask, NULL);
        type("xxxxxxxxxxxxxxxxxxxxxxxx"); /* 24 字。1 段目は 16 */
        eq_row(0, "pw: ****************");
        eq_row(1, "********");
        eq_row(2, "");
        key(CDK_K_ENTER, 0, 0);
        T_CHECK(strcmp(ASK_ANSWER, "xxxxxxxxxxxxxxxxxxxxxxxx") == 0);
    }

    T_CASE("wget: 同名の逃がし先");
    {
        char n[16];
        T_CHECK(cdk_shell_wget_numbered("/sdcard/a.txt", 1, n, sizeof(n)));
        T_EQ_STR(n, "/sdcard/a.txt.1");
        T_CHECK(!cdk_shell_wget_numbered("/sdcard/a.txt", 10, n, sizeof(n)));
    }

    return t_report("test_shell");
}
