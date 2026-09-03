/* 本物の wasm3 で、コミット済みの .wasm を実機と同じコードで走らせる。
 *
 * リンクしているのは port_wasm_vm.c と port_wasm_natives.c そのもの。
 * つまりゲストの境界検査（m3ApiCheckMem）を、実機に載るのと同じ形で見ている。
 *
 * fixture は host/fixtures/wasm の .wasm。作り直しは
 *   python host/fixtures/wasm/build.py
 */
#include "cdk_wasm_vm.h"
#include "t.h"

#include <stdio.h>

/* ---- 偽バックエンド（メモリ上の小さなファイル置き場つき） ------------ */

typedef struct {
    char name[64];
    char data[64];
    int  len;
    int  used;
} fakefile_t;

typedef struct {
    fakefile_t *f;
    int         pos;
} fakeopen_t;

typedef struct {
    fakefile_t  files[4];
    fakeopen_t  open[8];
    int         nopen;
    char        last_path[CDK_WASM_PATH_MAX];
} fakefs_t;

static uint64_t b_now(void *u) { (void)u; return 4242; }
static int      b_delay(void *u, int ms) { (void)u; (void)ms; return CDK_E_OK; }

static fakefile_t *find_or_make(fakefs_t *fs, const char *path, int create)
{
    for (int i = 0; i < 4; i++)
        if (fs->files[i].used && strcmp(fs->files[i].name, path) == 0)
            return &fs->files[i];
    if (!create) return NULL;
    for (int i = 0; i < 4; i++)
        if (!fs->files[i].used) {
            fs->files[i].used = 1;
            fs->files[i].len  = 0;
            snprintf(fs->files[i].name, sizeof fs->files[i].name, "%s", path);
            return &fs->files[i];
        }
    return NULL;
}

static void *b_open(void *u, const char *path, int mode)
{
    fakefs_t *fs = (fakefs_t *)u;
    snprintf(fs->last_path, sizeof fs->last_path, "%s", path);

    fakefile_t *f = find_or_make(fs, path, mode != CDK_OPEN_READ);
    if (!f) return NULL;
    if (mode == CDK_OPEN_WRITE) f->len = 0;

    for (int i = 0; i < 8; i++)
        if (!fs->open[i].f) {
            fs->open[i].f   = f;
            fs->open[i].pos = (mode == CDK_OPEN_APPEND) ? f->len : 0;
            fs->nopen++;
            return &fs->open[i];
        }
    return NULL;
}

static int b_close(void *u, void *h)
{
    fakefs_t *fs = (fakefs_t *)u;
    ((fakeopen_t *)h)->f = NULL;
    fs->nopen--;
    return CDK_E_OK;
}

static int b_read(void *u, void *h, void *buf, int len)
{
    (void)u;
    fakeopen_t *o = (fakeopen_t *)h;
    int n = o->f->len - o->pos;
    if (n > len) n = len;
    if (n <= 0) return 0;
    memcpy(buf, o->f->data + o->pos, (size_t)n);
    o->pos += n;
    return n;
}

static int b_write(void *u, void *h, const void *buf, int len)
{
    (void)u;
    fakeopen_t *o = (fakeopen_t *)h;
    int room = (int)sizeof o->f->data - o->pos;
    if (len > room) len = room;
    if (len <= 0) return 0;
    memcpy(o->f->data + o->pos, buf, (size_t)len);
    o->pos += len;
    if (o->pos > o->f->len) o->f->len = o->pos;
    return len;
}

static cdk_wasm_host_t make_backend(fakefs_t *fs)
{
    cdk_wasm_host_t h;
    memset(&h, 0, sizeof h);
    h.user       = fs;
    h.now_ms     = b_now;
    h.delay_ms   = b_delay;
    h.file_open  = b_open;
    h.file_close = b_close;
    h.file_read  = b_read;
    h.file_write = b_write;
    return h;
}

/* ---- fixture を読む -------------------------------------------------- */

static uint8_t  g_mod[64 * 1024];
static int      g_mod_len;

static int load_fixture(const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", CDK_WASM_FIXTURE_DIR, name);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("   fixture が開けません: %s\n", path); return 0; }
    g_mod_len = (int)fread(g_mod, 1, sizeof g_mod, f);
    fclose(f);
    return g_mod_len > 0;
}

/* fixture を 1 本走らせて、コンソールに出たものを out に集める。 */
static cdk_wasm_result_t run(const char *name, cdk_wasm_ctx_t *c,
                             char *out, int out_cap, int *ret, char *err, int err_cap)
{
    if (!load_fixture(name)) return CDK_WASM_BADMODULE;

    const cdk_wasm_result_t r =
        cdk_wasm_vm_run(c, g_mod, g_mod_len, ret, err, err_cap);

    const int n = cdk_wasm_drain_out(c, out, out_cap - 1);
    out[n > 0 ? n : 0] = '\0';
    return r;
}

/* ---- テスト ---------------------------------------------------------- */

int main(void)
{
    fakefs_t        fs;
    cdk_wasm_host_t host;
    cdk_wasm_ctx_t  c;
    char            out[512];
    char            err[256];
    int             ret;

    memset(&fs, 0, sizeof fs);
    host = make_backend(&fs);

    /* ---- ここが最初のケース。Ctrl-C の後始末が効くかの土台になる ---- */
    T_CASE("トラップしたあとに、同じプロセスでもう一度走る");
    {
        /* これが通らないと、Ctrl-C で止めたあと 2 本目が走らない。 */
        for (int round = 0; round < 3; round++) {
            cdk_wasm_ctx_init(&c, &host);
            T_EQ_INT(run("f_trap.wasm", &c, out, sizeof out, &ret, err, sizeof err),
                     CDK_WASM_TRAPPED);
            /* トラップで巻き戻るので、2 行目まで進んでいないこと */
            T_EQ_STR(out, "");

            cdk_wasm_ctx_init(&c, &host);
            T_EQ_INT(run("f_hello.wasm", &c, out, sizeof out, &ret, err, sizeof err),
                     CDK_WASM_DONE);
            T_EQ_STR(out, "hello");
        }
    }

    T_CASE("cdk_main が呼ばれ、引数と戻り値が通る");
    {
        cdk_wasm_ctx_init(&c, &host);
        char  a0[] = "wasm";
        char  a1[] = "f_hello.wasm";
        char *argv[] = {a0, a1};
        T_CHECK(cdk_wasm_set_args(&c, 2, argv));
        T_EQ_INT(run("f_hello.wasm", &c, out, sizeof out, &ret, err, sizeof err),
                 CDK_WASM_DONE);
        T_EQ_STR(out, "hello");
        T_EQ_INT(ret, 2 + 7); /* cdk_main(argc) は argc+7 を返す */
    }

    T_CASE("線形メモリの外は必ずトラップする");
    {
        cdk_wasm_ctx_init(&c, &host);
        T_EQ_INT(run("f_trap.wasm", &c, out, sizeof out, &ret, err, sizeof err),
                 CDK_WASM_TRAPPED);
        T_EQ_STR(out, "");
        T_CHECK(strstr(err, "out of bounds") != NULL);
    }

    T_CASE("長さに負を渡してもトラップする（i32 なので渡せてしまう）");
    {
        cdk_wasm_ctx_init(&c, &host);
        T_EQ_INT(run("f_neglen.wasm", &c, out, sizeof out, &ret, err, sizeof err),
                 CDK_WASM_TRAPPED);
        T_EQ_STR(out, "");
    }

    T_CASE("sys_exit のコードが拾える");
    {
        cdk_wasm_ctx_init(&c, &host);
        T_EQ_INT(run("f_exit.wasm", &c, out, sizeof out, &ret, err, sizeof err),
                 CDK_WASM_EXITED);
        T_EQ_STR(out, "bye");
        T_EQ_INT(ret, 42);
    }

    T_CASE("cdk_main の無いモジュールは走らせない");
    {
        cdk_wasm_ctx_init(&c, &host);
        T_EQ_INT(run("f_noentry.wasm", &c, out, sizeof out, &ret, err, sizeof err),
                 CDK_WASM_BADMODULE);
        T_CHECK(strstr(err, "cdk_main") != NULL);
    }

    T_CASE("wasm でないバイト列は読めないと言って終わる（落ちない）");
    {
        cdk_wasm_ctx_init(&c, &host);
        const char junk[] = "これは wasm ではありません";
        T_EQ_INT(cdk_wasm_vm_run(&c, junk, (int)sizeof junk - 1, &ret, err, sizeof err),
                 CDK_WASM_BADMODULE);
        T_CHECK(err[0] != '\0');

        /* 空・NULL でも落ちない */
        T_EQ_INT(cdk_wasm_vm_run(&c, junk, 0, &ret, err, sizeof err), CDK_WASM_BADMODULE);
        T_EQ_INT(cdk_wasm_vm_run(&c, NULL, 10, &ret, err, sizeof err), CDK_WASM_BADMODULE);
    }

    T_CASE("引数がゲストまで正しく届く");
    {
        cdk_wasm_ctx_init(&c, &host);
        char  a0[] = "wasm";
        char  a1[] = "run";
        char  a2[] = "xyz";
        char *argv[] = {a0, a1, a2};
        T_CHECK(cdk_wasm_set_args(&c, 3, argv));
        /* シェルは argv のあと即 return する。壊れても平気なことを確かめる */
        memset(a1, 'Q', sizeof a1 - 1);
        memset(a2, 'Q', sizeof a2 - 1);

        T_EQ_INT(run("f_args.wasm", &c, out, sizeof out, &ret, err, sizeof err),
                 CDK_WASM_DONE);
        T_EQ_STR(out, "argc=3 [0]=wasm(4) [1]=run(3) [2]=xyz(3)");
    }

    T_CASE("ファイルとサンドボックスがゲストから見て期待どおり");
    {
        memset(&fs, 0, sizeof fs);
        cdk_wasm_ctx_init(&c, &host);
        T_EQ_INT(run("f_fs.wasm", &c, out, sizeof out, &ret, err, sizeof err),
                 CDK_WASM_DONE);
        /* 書けて読める / 外へは出られない (-2) / 5 本目は断られる (-5) */
        T_EQ_STR(out,
                 "open=0 wrote=6 close=0 read=6:abcdef esc1=-2 esc2=-2 fifth=-5");
        /* サンドボックス外の呼び出しはバックエンドに届いていない */
        T_EQ_STR(fs.last_path, "/sdcard/note.txt");
        /* アプリが閉じ忘れても cleanup で全部返っている */
        T_EQ_INT(fs.nopen, 0);
    }

    T_CASE("予約したままの gfx_* は NOSYS を返す");
    {
        cdk_wasm_ctx_init(&c, &host);
        T_EQ_INT(run("f_nosys.wasm", &c, out, sizeof out, &ret, err, sizeof err),
                 CDK_WASM_DONE);
        T_EQ_STR(out, "present=-7 clear=-7");
    }

    T_CASE("Ctrl-C は native の入口で効く");
    {
        cdk_wasm_ctx_init(&c, &host);
        cdk_wasm_request_stop(&c); /* 走り出す前から立てておく */
        T_EQ_INT(run("f_hello.wasm", &c, out, sizeof out, &ret, err, sizeof err),
                 CDK_WASM_STOPPED);
        T_EQ_STR(out, "");
    }

    T_CASE("Rust の SDK が同じ ABI を話している");
    {
        /* **Rust クレートがファームと同じ ABI を話していることの証拠がこれ。**
         * 実機と同じ port_wasm_vm.c / port_wasm_natives.c にリンクして、
         * コミット済みの f_rust.wasm を走らせている。
         * 作り直しは python host/fixtures/wasm/build.py（cargo が要る）。 */
        memset(&fs, 0, sizeof fs);
        cdk_wasm_ctx_init(&c, &host);

        char  a0[] = "rs";
        char  a1[] = "one";
        char *argv[] = {a0, a1};
        T_CHECK(cdk_wasm_set_args(&c, 2, argv));
        memset(a1, 'Z', sizeof a1 - 1); /* 複製が効いていないと化ける */

        T_EQ_INT(run("f_rust.wasm", &c, out, sizeof out, &ret, err, sizeof err),
                 CDK_WASM_DONE);
        T_EQ_STR(out,
                 "hello argc=2 [0]=rs [1]=one size=80x26 read=abcdef "
                 "esc1=Perm esc2=Perm gfx=NoSys sum=15");
        T_EQ_INT(fs.nopen, 0); /* Drop と cleanup でハンドルが返っている */
    }

    T_CASE("線形メモリを 1MB より多く宣言したモジュールは断る");
    {
        /* ファームは d_m3MaxLinearMemoryPages=16 で打ち止めている。
         * ここが効いていないと、PSRAM を食い潰すモジュールが通ってしまう。 */
        cdk_wasm_ctx_init(&c, &host);
        T_EQ_INT(run("f_bigmem.wasm", &c, out, sizeof out, &ret, err, sizeof err),
                 CDK_WASM_BADMODULE);
        T_EQ_STR(out, "");
        T_CHECK(strstr(err, "linear memory") != NULL);
    }

    T_CASE("ABI の関数は全部リンク表に載っている");
    {
        /* 表から落とすと、そのゲストは「未知のインポート」で動かなくなる。
         * 数を固定しておいて、足すときにここも見るようにする。 */
        T_EQ_INT(cdk_wasm_native_count(), 46);
    }

    return t_report("test_wasm_run");
}
