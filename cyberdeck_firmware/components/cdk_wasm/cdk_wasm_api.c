/* ユーザーアプリ ABI の本体。
 *
 * ここに来るポインタは **すべて境界検査済み**（port_wasm_natives.c の
 * m3ApiCheckMem が通してある）。この層は線形メモリを知らない。
 *
 * ハンドルはすべてこの層が持つ小さな表の添字で、ゲストに実体
 * （FILE* / socket fd / UART 番号）は渡らない。
 */
#include "cdk_wasm.h"

#include <assert.h>
#include <string.h>

/* ---- 文脈 ------------------------------------------------------------ */

void cdk_wasm_ctx_init(cdk_wasm_ctx_t *c, const cdk_wasm_host_t *host)
{
    assert(c && host);
    memset(c, 0, sizeof *c);
    c->host = host;
    cdk_wasm_ring_init(&c->out, c->out_buf, (int)sizeof c->out_buf);
    cdk_wasm_ring_init(&c->in, c->in_buf, (int)sizeof c->in_buf);
    for (int i = 0; i < CDK_WASM_SOCKS_MAX; i++) c->socks[i] = -1;
    for (int i = 0; i < CDK_WASM_UARTS_MAX; i++) c->uarts[i] = -1;
    c->cols = 80;
    c->rows = 26;
}

void cdk_wasm_ctx_cleanup(cdk_wasm_ctx_t *c)
{
    assert(c);
    const cdk_wasm_host_t *h = c->host;
    if (!h) return;

    /* アプリが閉じ忘れたものを片づける。閉じ忘れは普通に起きるので、
     * 「開けたら閉じる」をアプリの行儀に任せない。 */
    for (int i = 0; i < CDK_WASM_FILES_MAX; i++) {
        if (c->files[i] && h->file_close) h->file_close(h->user, c->files[i]);
        c->files[i] = NULL;
    }
    for (int i = 0; i < CDK_WASM_DIRS_MAX; i++) {
        if (c->dirs[i] && h->dir_close) h->dir_close(h->user, c->dirs[i]);
        c->dirs[i] = NULL;
    }
    for (int i = 0; i < CDK_WASM_SOCKS_MAX; i++) {
        if (c->socks[i] >= 0 && h->sock_close) h->sock_close(h->user, c->socks[i]);
        c->socks[i] = -1;
    }
    for (int i = 0; i < CDK_WASM_UARTS_MAX; i++) {
        if (c->uarts[i] >= 0 && h->uart_close) h->uart_close(h->user, c->uarts[i]);
        c->uarts[i] = -1;
    }
    if (c->i2c_open && h->i2c_close) h->i2c_close(h->user);
    c->i2c_open = false;
}

bool cdk_wasm_set_args(cdk_wasm_ctx_t *c, int argc, char **argv)
{
    assert(c);
    c->argc = 0;
    if (argc < 0) return false;
    if (argc > CDK_WASM_ARGV_MAX) return false;

    int n = 0;
    for (int i = 0; i < argc; i++) {
        if (!argv[i]) return false;
        const size_t len = strlen(argv[i]);
        if (n + (int)len + 1 > (int)sizeof c->argv_store) return false;
        c->argv_off[i] = n;
        memcpy(c->argv_store + n, argv[i], len + 1);
        n += (int)len + 1;
    }
    c->argc = argc;
    return true;
}

void cdk_wasm_set_size(cdk_wasm_ctx_t *c, int cols, int rows)
{
    assert(c);
    if (cols > 0) c->cols = cols;
    if (rows > 0) c->rows = rows;
}

int cdk_wasm_drain_out(cdk_wasm_ctx_t *c, void *buf, int cap)
{
    assert(c);
    return cdk_wasm_ring_read(&c->out, buf, cap);
}

int cdk_wasm_push_in(cdk_wasm_ctx_t *c, const void *buf, int len)
{
    assert(c);
    return cdk_wasm_ring_write(&c->in, buf, len);
}

void cdk_wasm_request_stop(cdk_wasm_ctx_t *c)
{
    assert(c);
    c->stop = 1;
}

/* ---- ハンドル表 ------------------------------------------------------ */

static int slot_file(cdk_wasm_ctx_t *c)
{
    for (int i = 0; i < CDK_WASM_FILES_MAX; i++)
        if (!c->files[i]) return i;
    return -1;
}

static int slot_dir(cdk_wasm_ctx_t *c)
{
    for (int i = 0; i < CDK_WASM_DIRS_MAX; i++)
        if (!c->dirs[i]) return i;
    return -1;
}

static bool fd_ok(const cdk_wasm_ctx_t *c, int fd)
{
    return fd >= 0 && fd < CDK_WASM_FILES_MAX && c->files[fd] != NULL;
}

static bool dh_ok(const cdk_wasm_ctx_t *c, int dh)
{
    return dh >= 0 && dh < CDK_WASM_DIRS_MAX && c->dirs[dh] != NULL;
}

static bool sd_ok(const cdk_wasm_ctx_t *c, int sd)
{
    return sd >= 0 && sd < CDK_WASM_SOCKS_MAX && c->socks[sd] >= 0;
}

static bool uh_ok(const cdk_wasm_ctx_t *c, int h)
{
    return h >= 0 && h < CDK_WASM_UARTS_MAX && c->uarts[h] >= 0;
}

/* ---- sys ------------------------------------------------------------- */

uint64_t cdk_wasm_sys_now_ms(cdk_wasm_ctx_t *c)
{
    return c->host->now_ms ? c->host->now_ms(c->host->user) : 0;
}

int cdk_wasm_sys_delay(cdk_wasm_ctx_t *c, int ms)
{
    if (ms < 0) return CDK_E_INVAL;
    if (c->stop) return CDK_E_ABORT;
    if (!c->host->delay_ms) return CDK_E_NOSYS;
    return c->host->delay_ms(c->host->user, ms);
}

int cdk_wasm_sys_argc(cdk_wasm_ctx_t *c) { return c->argc; }

int cdk_wasm_sys_arg(cdk_wasm_ctx_t *c, int idx, char *out, int cap)
{
    if (idx < 0 || idx >= c->argc) return CDK_E_INVAL;
    if (!out || cap <= 0) return CDK_E_INVAL;
    const char *s = c->argv_store + c->argv_off[idx];
    const int   n = (int)strlen(s);
    if (n + 1 > cap) return CDK_E_NOMEM;
    memcpy(out, s, (size_t)n + 1);
    return n;
}

int cdk_wasm_sys_random(cdk_wasm_ctx_t *c, void *out, int len)
{
    if (!out || len <= 0) return CDK_E_INVAL;
    if (!c->host->random) return CDK_E_NOSYS;
    return c->host->random(c->host->user, out, len);
}

/* ---- con ------------------------------------------------------------- */

int cdk_wasm_con_write(cdk_wasm_ctx_t *c, const void *p, int n)
{
    if (!p || n < 0) return CDK_E_INVAL;
    if (n == 0) return 0;

    /* リングが埋まっているのは、メインループがまだ引き取っていないだけ。
     *
     * **捨てない。譲って待つ。** リングは 4KB で、メインループが 1 周で
     * 汲むのは 2KB。ファイルを読んで出し続けるアプリは簡単に追い越すので、
     * 溢れたぶんを捨てると `cat` のようなアプリの出力が画面から静かに
     * 消える（ログにしか残らない）。端末が本来やるのは流量制御のほうで、
     * アプリを少し待たせるのが正しい。
     *
     * 待つ間も Ctrl-C は効く（delay_ms が刻んで見ている）。 */
    const uint8_t *src = (const uint8_t *)p;
    int            done = 0;

    while (done < n) {
        done += cdk_wasm_ring_write(&c->out, src + done, n - done);
        if (done >= n) break;

        if (c->stop) { c->dropped += n - done; return CDK_E_ABORT; }
        if (!c->host->delay_ms) {
            /* 待つ手段が無いバックエンド（テストの偽物など）では、
             * 待てないぶんを捨てるしかない。数えて終了時に報告する。 */
            c->dropped += n - done;
            return n;
        }
        if (c->host->delay_ms(c->host->user, 2) == CDK_E_ABORT) {
            c->dropped += n - done;
            return CDK_E_ABORT;
        }
    }
    return n;
}

int cdk_wasm_con_read(cdk_wasm_ctx_t *c, void *p, int n)
{
    if (!p || n < 0) return CDK_E_INVAL;
    if (n == 0) return 0;
    return cdk_wasm_ring_read(&c->in, p, n);
}

int cdk_wasm_con_size(cdk_wasm_ctx_t *c, int32_t *cols, int32_t *rows)
{
    if (!cols || !rows) return CDK_E_INVAL;
    *cols = (int32_t)c->cols;
    *rows = (int32_t)c->rows;
    return CDK_E_OK;
}

/* ---- fs -------------------------------------------------------------- */

int cdk_wasm_fs_open(cdk_wasm_ctx_t *c, const char *path, int path_len, int mode)
{
    if (mode != CDK_OPEN_READ && mode != CDK_OPEN_WRITE && mode != CDK_OPEN_APPEND)
        return CDK_E_INVAL;

    char abs[CDK_WASM_PATH_MAX];
    const int r = cdk_wasm_path_resolve(path, path_len, abs, (int)sizeof abs);
    if (r != CDK_E_OK) return r;

    const int slot = slot_file(c);
    if (slot < 0) return CDK_E_NOMEM;
    if (!c->host->file_open) return CDK_E_NOSYS;

    void *h = c->host->file_open(c->host->user, abs, mode);
    if (!h) return CDK_E_NOENT;
    c->files[slot] = h;
    return slot;
}

int cdk_wasm_fs_read(cdk_wasm_ctx_t *c, int fd, void *p, int n)
{
    if (!fd_ok(c, fd) || !p || n < 0) return CDK_E_INVAL;
    if (n == 0) return 0;
    return c->host->file_read(c->host->user, c->files[fd], p, n);
}

int cdk_wasm_fs_write(cdk_wasm_ctx_t *c, int fd, const void *p, int n)
{
    if (!fd_ok(c, fd) || !p || n < 0) return CDK_E_INVAL;
    if (n == 0) return 0;
    return c->host->file_write(c->host->user, c->files[fd], p, n);
}

int cdk_wasm_fs_seek(cdk_wasm_ctx_t *c, int fd, int off, int whence)
{
    if (!fd_ok(c, fd)) return CDK_E_INVAL;
    if (whence != CDK_SEEK_SET && whence != CDK_SEEK_CUR && whence != CDK_SEEK_END)
        return CDK_E_INVAL;
    return c->host->file_seek(c->host->user, c->files[fd], off, whence);
}

int cdk_wasm_fs_close(cdk_wasm_ctx_t *c, int fd)
{
    if (!fd_ok(c, fd)) return CDK_E_INVAL;
    const int r = c->host->file_close(c->host->user, c->files[fd]);
    c->files[fd] = NULL; /* 失敗しても表からは外す。二重 close を作らない。 */
    return r;
}

int cdk_wasm_fs_size(cdk_wasm_ctx_t *c, const char *path, int path_len)
{
    char abs[CDK_WASM_PATH_MAX];
    const int r = cdk_wasm_path_resolve(path, path_len, abs, (int)sizeof abs);
    if (r != CDK_E_OK) return r;
    if (!c->host->file_size) return CDK_E_NOSYS;
    return c->host->file_size(c->host->user, abs);
}

int cdk_wasm_fs_unlink(cdk_wasm_ctx_t *c, const char *path, int path_len)
{
    char abs[CDK_WASM_PATH_MAX];
    const int r = cdk_wasm_path_resolve(path, path_len, abs, (int)sizeof abs);
    if (r != CDK_E_OK) return r;
    if (!c->host->file_unlink) return CDK_E_NOSYS;
    return c->host->file_unlink(c->host->user, abs);
}

int cdk_wasm_fs_mkdir(cdk_wasm_ctx_t *c, const char *path, int path_len)
{
    char abs[CDK_WASM_PATH_MAX];
    const int r = cdk_wasm_path_resolve(path, path_len, abs, (int)sizeof abs);
    if (r != CDK_E_OK) return r;
    if (!c->host->file_mkdir) return CDK_E_NOSYS;
    return c->host->file_mkdir(c->host->user, abs);
}

int cdk_wasm_fs_opendir(cdk_wasm_ctx_t *c, const char *path, int path_len)
{
    char abs[CDK_WASM_PATH_MAX];
    const int r = cdk_wasm_path_resolve(path, path_len, abs, (int)sizeof abs);
    if (r != CDK_E_OK) return r;

    const int slot = slot_dir(c);
    if (slot < 0) return CDK_E_NOMEM;
    if (!c->host->dir_open) return CDK_E_NOSYS;

    void *h = c->host->dir_open(c->host->user, abs);
    if (!h) return CDK_E_NOENT;
    c->dirs[slot] = h;
    return slot;
}

int cdk_wasm_fs_readdir(cdk_wasm_ctx_t *c, int dh, char *out, int cap)
{
    if (!dh_ok(c, dh) || !out || cap <= 0) return CDK_E_INVAL;
    return c->host->dir_read(c->host->user, c->dirs[dh], out, cap);
}

int cdk_wasm_fs_closedir(cdk_wasm_ctx_t *c, int dh)
{
    if (!dh_ok(c, dh)) return CDK_E_INVAL;
    const int r = c->host->dir_close(c->host->user, c->dirs[dh]);
    c->dirs[dh] = NULL;
    return r;
}

/* ---- gpio / adc ------------------------------------------------------ */

int cdk_wasm_gpio_mode(cdk_wasm_ctx_t *c, int pin, int mode)
{
    if (mode < CDK_GPIO_IN || mode > CDK_GPIO_OUT) return CDK_E_INVAL;
    if (!c->host->gpio_mode) return CDK_E_NOSYS;
    return c->host->gpio_mode(c->host->user, pin, mode);
}

int cdk_wasm_gpio_set(cdk_wasm_ctx_t *c, int pin, int level)
{
    if (!c->host->gpio_set) return CDK_E_NOSYS;
    return c->host->gpio_set(c->host->user, pin, level ? 1 : 0);
}

int cdk_wasm_gpio_get(cdk_wasm_ctx_t *c, int pin)
{
    if (!c->host->gpio_get) return CDK_E_NOSYS;
    return c->host->gpio_get(c->host->user, pin);
}

int cdk_wasm_adc_read(cdk_wasm_ctx_t *c, int ch)
{
    if (!c->host->adc_read) return CDK_E_NOSYS;
    return c->host->adc_read(c->host->user, ch);
}

/* ---- i2c ------------------------------------------------------------- */

int cdk_wasm_i2c_open(cdk_wasm_ctx_t *c, int sda, int scl, int hz)
{
    if (c->i2c_open) return CDK_E_NOMEM; /* 1 本だけ */
    if (hz <= 0) return CDK_E_INVAL;
    if (!c->host->i2c_open) return CDK_E_NOSYS;
    const int r = c->host->i2c_open(c->host->user, sda, scl, hz);
    if (r == CDK_E_OK) c->i2c_open = true;
    return r;
}

static bool addr_ok(int addr) { return addr >= 0x08 && addr <= 0x77; }

int cdk_wasm_i2c_write(cdk_wasm_ctx_t *c, int addr, const void *p, int n)
{
    if (!c->i2c_open) return CDK_E_INVAL;
    if (!addr_ok(addr) || !p || n <= 0) return CDK_E_INVAL;
    return c->host->i2c_write(c->host->user, addr, p, n);
}

int cdk_wasm_i2c_read(cdk_wasm_ctx_t *c, int addr, void *p, int n)
{
    if (!c->i2c_open) return CDK_E_INVAL;
    if (!addr_ok(addr) || !p || n <= 0) return CDK_E_INVAL;
    return c->host->i2c_read(c->host->user, addr, p, n);
}

int cdk_wasm_i2c_wrrd(cdk_wasm_ctx_t *c, int addr, const void *w, int wn, void *r, int rn)
{
    if (!c->i2c_open) return CDK_E_INVAL;
    if (!addr_ok(addr) || !w || wn <= 0 || !r || rn <= 0) return CDK_E_INVAL;
    return c->host->i2c_wrrd(c->host->user, addr, w, wn, r, rn);
}

int cdk_wasm_i2c_close(cdk_wasm_ctx_t *c)
{
    if (!c->i2c_open) return CDK_E_INVAL;
    const int r = c->host->i2c_close(c->host->user);
    c->i2c_open = false;
    return r;
}

/* ---- uart ------------------------------------------------------------ */

int cdk_wasm_uart_open(cdk_wasm_ctx_t *c, int tx, int rx, int baud)
{
    if (baud <= 0) return CDK_E_INVAL;
    if (!c->host->uart_open) return CDK_E_NOSYS;

    int slot = -1;
    for (int i = 0; i < CDK_WASM_UARTS_MAX; i++)
        if (c->uarts[i] < 0) { slot = i; break; }
    if (slot < 0) return CDK_E_NOMEM;

    const int h = c->host->uart_open(c->host->user, tx, rx, baud);
    if (h < 0) return h;
    c->uarts[slot] = h;
    return slot;
}

int cdk_wasm_uart_write(cdk_wasm_ctx_t *c, int h, const void *p, int n)
{
    if (!uh_ok(c, h) || !p || n < 0) return CDK_E_INVAL;
    if (n == 0) return 0;
    return c->host->uart_write(c->host->user, c->uarts[h], p, n);
}

int cdk_wasm_uart_read(cdk_wasm_ctx_t *c, int h, void *p, int n)
{
    if (!uh_ok(c, h) || !p || n < 0) return CDK_E_INVAL;
    if (n == 0) return 0;
    return c->host->uart_read(c->host->user, c->uarts[h], p, n);
}

int cdk_wasm_uart_close(cdk_wasm_ctx_t *c, int h)
{
    if (!uh_ok(c, h)) return CDK_E_INVAL;
    const int r = c->host->uart_close(c->host->user, c->uarts[h]);
    c->uarts[h] = -1;
    return r;
}

/* ---- net ------------------------------------------------------------- */

int cdk_wasm_net_up(cdk_wasm_ctx_t *c)
{
    if (!c->host->net_up) return CDK_E_NOSYS;
    return c->host->net_up(c->host->user);
}

int cdk_wasm_sock_open(cdk_wasm_ctx_t *c, int type)
{
    if (type != CDK_SOCK_TCP && type != CDK_SOCK_UDP) return CDK_E_INVAL;
    if (!c->host->sock_open) return CDK_E_NOSYS;

    int slot = -1;
    for (int i = 0; i < CDK_WASM_SOCKS_MAX; i++)
        if (c->socks[i] < 0) { slot = i; break; }
    if (slot < 0) return CDK_E_NOMEM;

    const int fd = c->host->sock_open(c->host->user, type);
    if (fd < 0) return fd;
    c->socks[slot] = fd;
    return slot;
}

int cdk_wasm_sock_connect(cdk_wasm_ctx_t *c, int sd, const char *host, int host_len, int port)
{
    if (!sd_ok(c, sd) || !host || host_len <= 0) return CDK_E_INVAL;
    if (port <= 0 || port > 65535) return CDK_E_INVAL;

    /* ホスト名は NUL 終端していないので、ここで写して終端する。 */
    char name[128];
    if (host_len >= (int)sizeof name) return CDK_E_INVAL;
    if (memchr(host, 0, (size_t)host_len) != NULL) return CDK_E_INVAL;
    memcpy(name, host, (size_t)host_len);
    name[host_len] = '\0';

    return c->host->sock_connect(c->host->user, c->socks[sd], name, port);
}

int cdk_wasm_sock_send(cdk_wasm_ctx_t *c, int sd, const void *p, int n)
{
    if (!sd_ok(c, sd) || !p || n < 0) return CDK_E_INVAL;
    if (n == 0) return 0;
    return c->host->sock_send(c->host->user, c->socks[sd], p, n);
}

int cdk_wasm_sock_recv(cdk_wasm_ctx_t *c, int sd, void *p, int n)
{
    if (!sd_ok(c, sd) || !p || n < 0) return CDK_E_INVAL;
    if (n == 0) return 0;
    return c->host->sock_recv(c->host->user, c->socks[sd], p, n);
}

int cdk_wasm_sock_close(cdk_wasm_ctx_t *c, int sd)
{
    if (!sd_ok(c, sd)) return CDK_E_INVAL;
    const int r = c->host->sock_close(c->host->user, c->socks[sd]);
    c->socks[sd] = -1;
    return r;
}
