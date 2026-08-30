/* cdk_wasm の純 C 部分。リング・パスのサンドボックス・ハンドル表。
 *
 * ここは wasm3 を通さずに直接叩く。実際にゲストから呼んだときの経路は
 * test_wasm_run.c が見る。 */
#include "cdk_wasm.h"
#include "t.h"

/* ---- 偽バックエンド --------------------------------------------------
 * 実機に触らずに、この層の判断だけを見る。 */

typedef struct {
    char last_path[CDK_WASM_PATH_MAX];
    int  opens;
    int  closes;
    int  fake_files[8]; /* file_open が返すハンドルの実体 */
    int  delay_total;
} fake_t;

static uint64_t f_now(void *u) { (void)u; return 1000; }

static int f_delay(void *u, int ms)
{
    ((fake_t *)u)->delay_total += ms;
    return CDK_E_OK;
}

/* 「待っている間にメインループが汲む」を再現する delay。
 * 実機で con_write が待たされたときに起きることと同じ形。 */
static cdk_wasm_ctx_t *g_drain_ctx;
static char            g_drained[CDK_WASM_OUT_BYTES * 4];
static int             g_drained_len;

static int f_delay_drains(void *u, int ms)
{
    ((fake_t *)u)->delay_total += ms;
    if (g_drain_ctx) {
        /* メインループと同じく、1 回で全部は汲まない */
        const int n = cdk_wasm_drain_out(g_drain_ctx, g_drained + g_drained_len, 256);
        if (n > 0) g_drained_len += n;
    }
    return CDK_E_OK;
}

/* 待っている最中に Ctrl-C が来る場合。 */
static int f_delay_stops(void *u, int ms)
{
    ((fake_t *)u)->delay_total += ms;
    if (g_drain_ctx) cdk_wasm_request_stop(g_drain_ctx);
    return CDK_E_ABORT;
}

static void *f_open(void *u, const char *path, int mode)
{
    fake_t *f = (fake_t *)u;
    (void)mode;
    snprintf(f->last_path, sizeof f->last_path, "%s", path);
    if (f->opens >= 8) return NULL;
    return &f->fake_files[f->opens++];
}

static int f_close(void *u, void *h)
{
    (void)h;
    ((fake_t *)u)->closes++;
    return CDK_E_OK;
}

static int f_read(void *u, void *h, void *buf, int len)
{
    (void)u; (void)h;
    if (len > 3) len = 3;
    memcpy(buf, "abc", (size_t)len);
    return len;
}

static int f_write(void *u, void *h, const void *buf, int len)
{
    (void)u; (void)h; (void)buf;
    return len;
}

static int f_seek(void *u, void *h, int off, int whence)
{
    (void)u; (void)h; (void)whence;
    return off;
}

static int f_gpio_mode(void *u, int pin, int mode)
{
    (void)u; (void)mode;
    return (pin == 35) ? CDK_E_PERM : CDK_E_OK; /* 予約ピンの真似 */
}

static cdk_wasm_host_t make_host(fake_t *f)
{
    cdk_wasm_host_t h;
    memset(&h, 0, sizeof h);
    h.user       = f;
    h.now_ms     = f_now;
    h.delay_ms   = f_delay;
    h.file_open  = f_open;
    h.file_close = f_close;
    h.file_read  = f_read;
    h.file_write = f_write;
    h.file_seek  = f_seek;
    h.gpio_mode  = f_gpio_mode;
    return h;
}

/* ---- パス ------------------------------------------------------------ */

static int resolve(const char *in, char *out, int cap)
{
    return cdk_wasm_path_resolve(in, (int)strlen(in), out, cap);
}

static void test_path(void)
{
    char out[CDK_WASM_PATH_MAX];

    T_CASE("相対パスは /sdcard からの相対になる");
    {
        T_EQ_INT(resolve("a.txt", out, sizeof out), CDK_E_OK);
        T_EQ_STR(out, "/sdcard/a.txt");

        T_EQ_INT(resolve("apps/hello.wasm", out, sizeof out), CDK_E_OK);
        T_EQ_STR(out, "/sdcard/apps/hello.wasm");
    }

    T_CASE("絶対パスでも /sdcard の下なら通る");
    {
        T_EQ_INT(resolve("/sdcard/a.txt", out, sizeof out), CDK_E_OK);
        T_EQ_STR(out, "/sdcard/a.txt");

        T_EQ_INT(resolve("/sdcard", out, sizeof out), CDK_E_OK);
        T_EQ_STR(out, "/sdcard");
    }

    T_CASE("余計な区切りと . は畳まれる");
    {
        T_EQ_INT(resolve("a///b//", out, sizeof out), CDK_E_OK);
        T_EQ_STR(out, "/sdcard/a/b");

        T_EQ_INT(resolve("./a/./b", out, sizeof out), CDK_E_OK);
        T_EQ_STR(out, "/sdcard/a/b");

        T_EQ_INT(resolve("/sdcard//a///b//", out, sizeof out), CDK_E_OK);
        T_EQ_STR(out, "/sdcard/a/b");

        /* 先頭が / なら絶対パス。畳んだ結果が root の外なので断る。
         * 「区切りが多いだけ」に見えても相対には落とさない。 */
        T_EQ_INT(resolve("//a///b//", out, sizeof out), CDK_E_PERM);
    }

    T_CASE(".. は畳んだうえで判定する（ここが破れると外が見える）");
    {
        /* 中で戻るぶんには構わない */
        T_EQ_INT(resolve("a/b/../c", out, sizeof out), CDK_E_OK);
        T_EQ_STR(out, "/sdcard/a/c");

        /* root より上へは出せない */
        T_EQ_INT(resolve("..", out, sizeof out), CDK_E_PERM);
        T_EQ_INT(resolve("../secret", out, sizeof out), CDK_E_PERM);
        T_EQ_INT(resolve("a/../../secret", out, sizeof out), CDK_E_PERM);
        T_EQ_INT(resolve("/sdcard/../etc/passwd", out, sizeof out), CDK_E_PERM);
        T_EQ_INT(resolve("/sdcard/a/../../etc", out, sizeof out), CDK_E_PERM);
    }

    T_CASE("root の外の絶対パスは弾く");
    {
        T_EQ_INT(resolve("/etc/passwd", out, sizeof out), CDK_E_PERM);
        T_EQ_INT(resolve("/", out, sizeof out), CDK_E_PERM);
        /* 接頭辞だけ一致するものに騙されないこと */
        T_EQ_INT(resolve("/sdcardX/a", out, sizeof out), CDK_E_PERM);
        T_EQ_INT(resolve("/sdcard2", out, sizeof out), CDK_E_PERM);
    }

    T_CASE("長さと NUL の扱い");
    {
        T_EQ_INT(cdk_wasm_path_resolve("a", 0, out, sizeof out), CDK_E_INVAL);
        T_EQ_INT(cdk_wasm_path_resolve("a", -1, out, sizeof out), CDK_E_INVAL);
        /* ゲストのメモリは (ptr,len)。途中に NUL があるものは受けない */
        T_EQ_INT(cdk_wasm_path_resolve("a\0b", 3, out, sizeof out), CDK_E_INVAL);

        char big[CDK_WASM_PATH_MAX + 32];
        memset(big, 'x', sizeof big);
        T_EQ_INT(cdk_wasm_path_resolve(big, (int)sizeof big, out, sizeof out),
                 CDK_E_INVAL);
    }
}

/* ---- リング ---------------------------------------------------------- */

static void test_ring(void)
{
    uint8_t         mem[8];
    cdk_wasm_ring_t r;

    T_CASE("空と満杯");
    {
        cdk_wasm_ring_init(&r, mem, (int)sizeof mem);
        T_EQ_INT(cdk_wasm_ring_used(&r), 0);
        /* 1 バイトは空と満杯の区別に使うので書けない */
        T_EQ_INT(cdk_wasm_ring_free(&r), 7);
        T_EQ_INT(cdk_wasm_ring_write(&r, "abcdefghij", 10), 7);
        T_EQ_INT(cdk_wasm_ring_free(&r), 0);
        T_EQ_INT(cdk_wasm_ring_write(&r, "z", 1), 0);
    }

    T_CASE("読んだぶんだけ空く");
    {
        char buf[16];
        T_EQ_INT(cdk_wasm_ring_read(&r, buf, 3), 3);
        T_EQ_MEM(buf, "abc", 3);
        T_EQ_INT(cdk_wasm_ring_used(&r), 4);
        T_EQ_INT(cdk_wasm_ring_read(&r, buf, 16), 4);
        T_EQ_MEM(buf, "defg", 4);
        T_EQ_INT(cdk_wasm_ring_used(&r), 0);
        T_EQ_INT(cdk_wasm_ring_read(&r, buf, 1), 0);
    }

    T_CASE("端をまたぐ書き込みと読み出し");
    {
        char buf[16];
        /* いまの head は 7。ここから 5 バイト書くと 4 バイト目で回り込む */
        T_EQ_INT(cdk_wasm_ring_write(&r, "12345", 5), 5);
        T_EQ_INT(cdk_wasm_ring_read(&r, buf, 5), 5);
        T_EQ_MEM(buf, "12345", 5);

        /* 何周しても壊れない */
        for (int i = 0; i < 100; i++) {
            T_EQ_INT(cdk_wasm_ring_write(&r, "xyz", 3), 3);
            T_EQ_INT(cdk_wasm_ring_read(&r, buf, 3), 3);
            T_EQ_MEM(buf, "xyz", 3);
        }
        T_EQ_INT(cdk_wasm_ring_used(&r), 0);
    }

    T_CASE("負や 0 の長さは何もしない");
    {
        char buf[4];
        T_EQ_INT(cdk_wasm_ring_write(&r, "a", 0), 0);
        T_EQ_INT(cdk_wasm_ring_write(&r, "a", -5), 0);
        T_EQ_INT(cdk_wasm_ring_read(&r, buf, -5), 0);
        T_EQ_INT(cdk_wasm_ring_write(&r, NULL, 3), 0);
    }
}

/* ---- ハンドル表 ------------------------------------------------------ */

static void test_handles(void)
{
    fake_t          f;
    cdk_wasm_host_t host;
    cdk_wasm_ctx_t  c;

    memset(&f, 0, sizeof f);
    host = make_host(&f);
    cdk_wasm_ctx_init(&c, &host);

    T_CASE("fd は 0 から順に配られ、上限で NOMEM");
    {
        for (int i = 0; i < CDK_WASM_FILES_MAX; i++)
            T_EQ_INT(cdk_wasm_fs_open(&c, "a", 1, CDK_OPEN_READ), i);
        T_EQ_INT(cdk_wasm_fs_open(&c, "a", 1, CDK_OPEN_READ), CDK_E_NOMEM);
        /* FATFS の max_files=5 のうち 1 本は本体に残す約束 */
        T_EQ_INT(CDK_WASM_FILES_MAX, 4);
    }

    T_CASE("閉じた fd は再利用され、二重 close は INVAL");
    {
        T_EQ_INT(cdk_wasm_fs_close(&c, 1), CDK_E_OK);
        T_EQ_INT(cdk_wasm_fs_close(&c, 1), CDK_E_INVAL);
        T_EQ_INT(cdk_wasm_fs_open(&c, "b", 1, CDK_OPEN_READ), 1);
    }

    T_CASE("表の外を指す fd は INVAL");
    {
        char buf[4];
        T_EQ_INT(cdk_wasm_fs_read(&c, -1, buf, 4), CDK_E_INVAL);
        T_EQ_INT(cdk_wasm_fs_read(&c, 99, buf, 4), CDK_E_INVAL);
        T_EQ_INT(cdk_wasm_fs_close(&c, 99), CDK_E_INVAL);
        T_EQ_INT(cdk_wasm_fs_seek(&c, 0, 0, 99), CDK_E_INVAL);
    }

    T_CASE("cleanup が開けっぱなしを全部閉じる");
    {
        const int before = f.closes;
        cdk_wasm_ctx_cleanup(&c);
        /* 開いていた 4 本ぶん閉じている */
        T_EQ_INT(f.closes - before, 4);
        /* もう一度呼んでも増えない */
        cdk_wasm_ctx_cleanup(&c);
        T_EQ_INT(f.closes - before, 4);
    }

    T_CASE("サンドボックス外はバックエンドまで届かない");
    {
        memset(&f, 0, sizeof f);
        cdk_wasm_ctx_init(&c, &host);
        T_EQ_INT(cdk_wasm_fs_open(&c, "../x", 4, CDK_OPEN_READ), CDK_E_PERM);
        T_EQ_INT(f.opens, 0);
        T_EQ_STR(f.last_path, "");
    }

    T_CASE("バックエンドが埋めていない口は NOSYS");
    {
        /* make_host は net_* も i2c_* も埋めていない */
        T_EQ_INT(cdk_wasm_net_up(&c), CDK_E_NOSYS);
        T_EQ_INT(cdk_wasm_sock_open(&c, CDK_SOCK_TCP), CDK_E_NOSYS);
        T_EQ_INT(cdk_wasm_i2c_open(&c, 1, 2, 100000), CDK_E_NOSYS);
        T_EQ_INT(cdk_wasm_adc_read(&c, 0), CDK_E_NOSYS);
    }

    T_CASE("引数の検査");
    {
        T_EQ_INT(cdk_wasm_fs_open(&c, "a", 1, 99), CDK_E_INVAL);
        T_EQ_INT(cdk_wasm_gpio_mode(&c, 1, 99), CDK_E_INVAL);
        T_EQ_INT(cdk_wasm_gpio_mode(&c, 1, -1), CDK_E_INVAL);
        /* 予約ピンはバックエンドが弾く */
        T_EQ_INT(cdk_wasm_gpio_mode(&c, 35, CDK_GPIO_OUT), CDK_E_PERM);
        T_EQ_INT(cdk_wasm_gpio_mode(&c, 1, CDK_GPIO_OUT), CDK_E_OK);
        T_EQ_INT(cdk_wasm_sock_open(&c, 42), CDK_E_INVAL);
        T_EQ_INT(cdk_wasm_sys_delay(&c, -1), CDK_E_INVAL);
    }

    T_CASE("i2c は開ける前には使えない");
    {
        char buf[4];
        T_EQ_INT(cdk_wasm_i2c_read(&c, 0x40, buf, 4), CDK_E_INVAL);
        T_EQ_INT(cdk_wasm_i2c_close(&c), CDK_E_INVAL);
    }
}

/* ---- 引数 ------------------------------------------------------------ */

static void test_args(void)
{
    fake_t          f;
    cdk_wasm_host_t host;
    cdk_wasm_ctx_t  c;

    memset(&f, 0, sizeof f);
    host = make_host(&f);
    cdk_wasm_ctx_init(&c, &host);

    T_CASE("argv は複製される（シェルのスタックを指したままにしない）");
    {
        /* cdk_shell_split の argv は execute() のスタックを指しているので、
         * ここで写しておかないとアプリが読むころには消えている。 */
        char  a0[] = "wasm";
        char  a1[] = "hello.wasm";
        char  a2[] = "arg";
        char *argv[] = {a0, a1, a2};

        T_CHECK(cdk_wasm_set_args(&c, 3, argv));

        /* 元を壊しても影響が無いこと＝写っていること */
        memset(a1, 'X', sizeof a1 - 1);

        char out[32];
        T_EQ_INT(cdk_wasm_sys_argc(&c), 3);
        T_EQ_INT(cdk_wasm_sys_arg(&c, 1, out, sizeof out), 10);
        T_EQ_STR(out, "hello.wasm");
    }

    T_CASE("範囲外と入りきらない場合");
    {
        char out[4];
        T_EQ_INT(cdk_wasm_sys_arg(&c, -1, out, sizeof out), CDK_E_INVAL);
        T_EQ_INT(cdk_wasm_sys_arg(&c, 3, out, sizeof out), CDK_E_INVAL);
        /* "hello.wasm" は 4 バイトには入らない */
        T_EQ_INT(cdk_wasm_sys_arg(&c, 1, out, sizeof out), CDK_E_NOMEM);
    }

    T_CASE("多すぎる・長すぎる引数は走らせる前に断る");
    {
        char *many[CDK_WASM_ARGV_MAX + 1];
        char  word[] = "x";
        for (int i = 0; i <= CDK_WASM_ARGV_MAX; i++) many[i] = word;
        T_CHECK(!cdk_wasm_set_args(&c, CDK_WASM_ARGV_MAX + 1, many));
        T_EQ_INT(cdk_wasm_sys_argc(&c), 0);

        static char big[CDK_WASM_ARGV_BYTES + 8];
        memset(big, 'y', sizeof big - 1);
        big[sizeof big - 1] = '\0';
        char *one[] = {big};
        T_CHECK(!cdk_wasm_set_args(&c, 1, one));
        T_EQ_INT(cdk_wasm_sys_argc(&c), 0);
    }
}

/* ---- コンソール ------------------------------------------------------ */

static void test_console(void)
{
    fake_t          f;
    cdk_wasm_host_t host;
    cdk_wasm_ctx_t  c;

    memset(&f, 0, sizeof f);
    host = make_host(&f);
    cdk_wasm_ctx_init(&c, &host);

    T_CASE("書いたものはメインループが引き取れる");
    {
        char buf[64];
        T_EQ_INT(cdk_wasm_con_write(&c, "hi", 2), 2);
        T_EQ_INT(cdk_wasm_drain_out(&c, buf, sizeof buf), 2);
        T_EQ_MEM(buf, "hi", 2);
    }

    T_CASE("メインループが押し込んだキーをアプリが読む");
    {
        char buf[8];
        T_EQ_INT(cdk_wasm_con_read(&c, buf, sizeof buf), 0); /* 待たない */
        T_EQ_INT(cdk_wasm_push_in(&c, "\033[A", 3), 3);
        T_EQ_INT(cdk_wasm_con_read(&c, buf, sizeof buf), 3);
        T_EQ_MEM(buf, "\033[A", 3);
    }

    T_CASE("リングより大きい出力でも 1 バイトも落とさない");
    {
        /* メインループは 1 周でリングの一部しか汲まない。追い越した
         * ぶんを捨てると、ファイルを読んで出し続けるアプリの出力が
         * 画面から静かに消える。捨てずに待つのが正しい。
         *
         * 偽の delay_ms が「メインループが汲む」役をする。実機で
         * 起きることと同じ形にしてある。 */
        g_drain_ctx = &c;
        g_drained_len = 0;
        host.delay_ms = f_delay_drains;
        cdk_wasm_ctx_init(&c, &host);

        static char big[CDK_WASM_OUT_BYTES * 3 + 77];
        for (size_t i = 0; i < sizeof big; i++) big[i] = (char)('a' + (i % 26));

        T_EQ_INT(cdk_wasm_con_write(&c, big, (int)sizeof big), (int)sizeof big);

        /* 残っているぶんも汲み切る */
        for (;;) {
            const int n = cdk_wasm_drain_out(&c, g_drained + g_drained_len,
                                             (int)(sizeof g_drained) - g_drained_len);
            if (n <= 0) break;
            g_drained_len += n;
        }

        T_EQ_INT(c.dropped, 0);
        T_EQ_INT(g_drained_len, (int)sizeof big);
        T_EQ_MEM(g_drained, big, sizeof big);

        host.delay_ms = f_delay; /* 元に戻す */
    }

    T_CASE("待つ手段が無いときだけ捨て、捨てた量を数える");
    {
        /* delay_ms を持たないバックエンドでは待てない。そのときは
         * 捨てるしかないが、黙って捨てずに数えて終了時に報告する。 */
        cdk_wasm_host_t nodelay = host;
        nodelay.delay_ms = NULL;
        cdk_wasm_ctx_init(&c, &nodelay);

        static char big[CDK_WASM_OUT_BYTES * 2];
        memset(big, 'z', sizeof big);
        T_EQ_INT(cdk_wasm_con_write(&c, big, (int)sizeof big), (int)sizeof big);
        T_EQ_INT(c.dropped, (int)sizeof big - (CDK_WASM_OUT_BYTES - 1));
    }

    T_CASE("待っている間に Ctrl-C が来たら ABORT で返る");
    {
        cdk_wasm_host_t stopping = host;
        stopping.delay_ms = f_delay_stops;
        cdk_wasm_ctx_init(&c, &stopping);
        g_drain_ctx = &c;

        static char big[CDK_WASM_OUT_BYTES * 2];
        memset(big, 'z', sizeof big);
        T_EQ_INT(cdk_wasm_con_write(&c, big, (int)sizeof big), CDK_E_ABORT);
    }

    cdk_wasm_ctx_init(&c, &host); /* 以降のケースのために作り直す */

    T_CASE("端末の大きさ");
    {
        int32_t cols = 0, rows = 0;
        cdk_wasm_set_size(&c, 53, 20);
        T_EQ_INT(cdk_wasm_con_size(&c, &cols, &rows), CDK_E_OK);
        T_EQ_INT(cols, 53);
        T_EQ_INT(rows, 20);
    }

    T_CASE("Ctrl-C は delay を ABORT で返す");
    {
        T_EQ_INT(cdk_wasm_sys_delay(&c, 10), CDK_E_OK);
        cdk_wasm_request_stop(&c);
        T_EQ_INT(cdk_wasm_sys_delay(&c, 10), CDK_E_ABORT);
    }
}

int main(void)
{
    test_path();
    test_ring();
    test_handles();
    test_args();
    test_console();
    return t_report("test_wasm_abi");
}
