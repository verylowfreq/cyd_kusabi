/* cdk_wasm — ユーザーアプリ ABI の中身（純 C）
 *
 * wasm3 のヘッダをここに入れない。この層が受け取るのは
 * **すでに境界検査を通したネイティブポインタと長さ**だけで、
 * 線形メモリのことも wasm3 のことも知らない。
 *
 * 境界検査は port_wasm_natives.c の m3ApiCheckMem が一手に引き受ける。
 * ここを純 C に保つことで、fd 表・パス検査・リングをホストで単体テストできる。
 */
#ifndef CDK_WASM_H
#define CDK_WASM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cdk_wasm_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- リング ----------------------------------------------------------
 *
 * 片側が wasm タスク、もう片側がメインループ。**単一生産者・単一消費者**。
 * wasm タスクはメインループと同じ CPU0 に固定して動かすので、
 * volatile な head/tail だけで足りる（port_wasm.c にその前提を書いてある）。
 */
typedef struct {
    uint8_t         *buf;
    int              cap;
    volatile int     head; /* 書く側だけが進める */
    volatile int     tail; /* 読む側だけが進める */
} cdk_wasm_ring_t;

void cdk_wasm_ring_init(cdk_wasm_ring_t *r, uint8_t *buf, int cap);
int  cdk_wasm_ring_used(const cdk_wasm_ring_t *r);
int  cdk_wasm_ring_free(const cdk_wasm_ring_t *r);
/* 書けるだけ書いて、書けた数を返す（部分書き込みあり）。 */
int  cdk_wasm_ring_write(cdk_wasm_ring_t *r, const void *p, int n);
/* 読めるだけ読んで、読めた数を返す。 */
int  cdk_wasm_ring_read(cdk_wasm_ring_t *r, void *p, int n);

/* ---- パス ------------------------------------------------------------ */

/* ゲストの与えたパスを絶対パスに正規化し、CDK_WASM_ROOT の下に
 * 収まっているかを判定する。相対パスは root からの相対とみなす。
 * "." ".." を解決してから判定するので、"a/../../etc" は弾かれる。
 *
 * 成功したら out に NUL 終端の絶対パスを書いて CDK_E_OK。
 * 失敗は CDK_E_PERM（外へ出た）/ CDK_E_INVAL（長すぎ・NUL 入り）。
 *
 * in は NUL 終端していなくてよい（ゲストのメモリは (ptr,len) で来る）。
 */
int cdk_wasm_path_resolve(const char *in, int in_len, char *out, int out_cap);

/* ---- 実行文脈 -------------------------------------------------------- */

typedef struct {
    const cdk_wasm_host_t *host;

    uint8_t         out_buf[CDK_WASM_OUT_BYTES];
    uint8_t         in_buf[CDK_WASM_IN_BYTES];
    cdk_wasm_ring_t out; /* アプリ → 端末 */
    cdk_wasm_ring_t in;  /* 端末 → アプリ */

    /* 引数。cdk_shell_split の argv はコマンドが返ると無効になるので、
     * 走らせる前に必ずここへ複製する（cdk_wasm_set_args）。 */
    char argv_store[CDK_WASM_ARGV_BYTES];
    int  argv_off[CDK_WASM_ARGV_MAX];
    int  argc;

    void *files[CDK_WASM_FILES_MAX];
    void *dirs[CDK_WASM_DIRS_MAX];
    int   socks[CDK_WASM_SOCKS_MAX];  /* -1 = 空き */
    int   uarts[CDK_WASM_UARTS_MAX];  /* -1 = 空き */
    bool  i2c_open;

    int cols, rows;

    /* Ctrl-C。メインループが立て、wasm タスクが見る。 */
    volatile int stop;

    /* 出力リングが溢れて捨てたバイト数。終了時に報告する。 */
    int dropped;

    /* sys_exit の記録。トラップで抜けるので、コードはここに残す。 */
    int  exit_code;
    bool exited;
} cdk_wasm_ctx_t;

void cdk_wasm_ctx_init(cdk_wasm_ctx_t *c, const cdk_wasm_host_t *host);
/* 走らせ終わったあとに、開けっぱなしのハンドルを閉じる。 */
void cdk_wasm_ctx_cleanup(cdk_wasm_ctx_t *c);

/* argv を複製する。溢れたら false（走らせない）。 */
bool cdk_wasm_set_args(cdk_wasm_ctx_t *c, int argc, char **argv);

void cdk_wasm_set_size(cdk_wasm_ctx_t *c, int cols, int rows);

/* メインループ側が使う口。 */
int  cdk_wasm_drain_out(cdk_wasm_ctx_t *c, void *buf, int cap);
int  cdk_wasm_push_in(cdk_wasm_ctx_t *c, const void *buf, int len);
void cdk_wasm_request_stop(cdk_wasm_ctx_t *c);

/* ---- API 本体 --------------------------------------------------------
 *
 * port_wasm_natives.c の各シムが、境界検査のあとにここを呼ぶ。
 * ポインタはすべて検査済み。長さは負でないことをここでも見る
 * （シム側の型が i32 なので負が来うる）。
 */

int      cdk_wasm_sys_delay(cdk_wasm_ctx_t *c, int ms);
uint64_t cdk_wasm_sys_now_ms(cdk_wasm_ctx_t *c);
int      cdk_wasm_sys_argc(cdk_wasm_ctx_t *c);
int      cdk_wasm_sys_arg(cdk_wasm_ctx_t *c, int idx, char *out, int cap);
int      cdk_wasm_sys_random(cdk_wasm_ctx_t *c, void *out, int len);

int cdk_wasm_con_write(cdk_wasm_ctx_t *c, const void *p, int n);
int cdk_wasm_con_read(cdk_wasm_ctx_t *c, void *p, int n);
int cdk_wasm_con_size(cdk_wasm_ctx_t *c, int32_t *cols, int32_t *rows);

int cdk_wasm_fs_open(cdk_wasm_ctx_t *c, const char *path, int path_len, int mode);
int cdk_wasm_fs_read(cdk_wasm_ctx_t *c, int fd, void *p, int n);
int cdk_wasm_fs_write(cdk_wasm_ctx_t *c, int fd, const void *p, int n);
int cdk_wasm_fs_seek(cdk_wasm_ctx_t *c, int fd, int off, int whence);
int cdk_wasm_fs_close(cdk_wasm_ctx_t *c, int fd);
int cdk_wasm_fs_size(cdk_wasm_ctx_t *c, const char *path, int path_len);
int cdk_wasm_fs_unlink(cdk_wasm_ctx_t *c, const char *path, int path_len);
int cdk_wasm_fs_mkdir(cdk_wasm_ctx_t *c, const char *path, int path_len);
int cdk_wasm_fs_opendir(cdk_wasm_ctx_t *c, const char *path, int path_len);
int cdk_wasm_fs_readdir(cdk_wasm_ctx_t *c, int dh, char *out, int cap);
int cdk_wasm_fs_closedir(cdk_wasm_ctx_t *c, int dh);

int cdk_wasm_gpio_mode(cdk_wasm_ctx_t *c, int pin, int mode);
int cdk_wasm_gpio_set(cdk_wasm_ctx_t *c, int pin, int level);
int cdk_wasm_gpio_get(cdk_wasm_ctx_t *c, int pin);
int cdk_wasm_adc_read(cdk_wasm_ctx_t *c, int ch);

int cdk_wasm_i2c_open(cdk_wasm_ctx_t *c, int sda, int scl, int hz);
int cdk_wasm_i2c_probe(cdk_wasm_ctx_t *c, int addr);
int cdk_wasm_i2c_write(cdk_wasm_ctx_t *c, int addr, const void *p, int n);
int cdk_wasm_i2c_read(cdk_wasm_ctx_t *c, int addr, void *p, int n);
int cdk_wasm_i2c_wrrd(cdk_wasm_ctx_t *c, int addr, const void *w, int wn, void *r, int rn);
int cdk_wasm_i2c_close(cdk_wasm_ctx_t *c);

int cdk_wasm_uart_open(cdk_wasm_ctx_t *c, int tx, int rx, int baud);
int cdk_wasm_uart_write(cdk_wasm_ctx_t *c, int h, const void *p, int n);
int cdk_wasm_uart_read(cdk_wasm_ctx_t *c, int h, void *p, int n);
int cdk_wasm_uart_close(cdk_wasm_ctx_t *c, int h);

int cdk_wasm_net_up(cdk_wasm_ctx_t *c);
int cdk_wasm_sock_open(cdk_wasm_ctx_t *c, int type);
int cdk_wasm_sock_connect(cdk_wasm_ctx_t *c, int sd, const char *host, int host_len, int port);
int cdk_wasm_sock_send(cdk_wasm_ctx_t *c, int sd, const void *p, int n);
int cdk_wasm_sock_recv(cdk_wasm_ctx_t *c, int sd, void *p, int n);
int cdk_wasm_sock_close(cdk_wasm_ctx_t *c, int sd);

#ifdef __cplusplus
}
#endif

#endif /* CDK_WASM_H */
