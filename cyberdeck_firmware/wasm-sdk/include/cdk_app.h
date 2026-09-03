/* cdk_app.h — サイバーデックのユーザーアプリを書くためのヘッダ
 *
 * WASI でも POSIX でもない。この機械だけの小さな API。
 *
 *   clang --target=wasm32 で自由形式（-nostdlib）にビルドする。
 *   libc は無い。要るものは wasm-sdk/src/cdk_app.c が最小限だけ持っている。
 *
 * 使い方:
 *
 *   #include "cdk_app.h"
 *   CDK_MAIN int cdk_main(int argc) { cdk_print("やあ\r\n"); return 0; }
 *
 * 端末は CR+LF を要る。"\n" だけだと行頭に戻らない。
 *
 * 戻り値は 0 以上が成功（長さや値）、負が失敗（CDK_E_*）。
 */
#ifndef CDK_APP_H
#define CDK_APP_H

typedef unsigned long  cdk_size_t;
typedef long long      cdk_i64;

#define CDK_IMPORT(n) __attribute__((import_module("cdk"), import_name(#n)))
#define CDK_MAIN      __attribute__((export_name("cdk_main")))

/* エラー。components/cdk_wasm/include/cdk_wasm_abi.h と対で持つ。 */
#define CDK_E_OK     0
#define CDK_E_INVAL (-1)
#define CDK_E_PERM  (-2)
#define CDK_E_NOENT (-3)
#define CDK_E_IO    (-4)
#define CDK_E_NOMEM (-5)
#define CDK_E_AGAIN (-6)
#define CDK_E_NOSYS (-7)
#define CDK_E_TIME  (-8)
#define CDK_E_ABORT (-9)

#define CDK_OPEN_READ   0
#define CDK_OPEN_WRITE  1
#define CDK_OPEN_APPEND 2

#define CDK_SEEK_SET 0
#define CDK_SEEK_CUR 1
#define CDK_SEEK_END 2

#define CDK_GPIO_IN    0
#define CDK_GPIO_IN_PU 1
#define CDK_GPIO_IN_PD 2
#define CDK_GPIO_OUT   3

#define CDK_SOCK_TCP 0
#define CDK_SOCK_UDP 1
/* TLS。**中身はファーム側 (ESP-IDF) が終端する。** 開いたあとは
 * sock_connect / sock_send / sock_recv / sock_close をそのまま使う。
 * 証明書はファームが持つバンドルで検証する。
 * _TLS_INSECURE は検証しないので、自己署名の相手を試すとき以外に使わない。
 * **TLS は同時 2 本まで。** 3 本目の sock_open は CDK_E_NOMEM。 */
#define CDK_SOCK_TLS          2
#define CDK_SOCK_TLS_INSECURE 3

/* ---- sys ------------------------------------------------------------- */

CDK_IMPORT(sys_exit)   void    sys_exit(int code);
CDK_IMPORT(sys_now_ms) cdk_i64 sys_now_ms(void);
/* 待つ。Ctrl-C が来たらここでアプリごと止まる（戻ってこない）。 */
CDK_IMPORT(sys_delay)  int     sys_delay(int ms);
CDK_IMPORT(sys_argc)   int     sys_argc(void);
CDK_IMPORT(sys_arg)    int     sys_arg(int idx, char *out, int cap);
CDK_IMPORT(sys_random) int     sys_random(void *out, int len);

/* ---- コンソール ------------------------------------------------------ */

/* 端末へ。UTF-8 と VT エスケープ列はそのまま通る。 */
CDK_IMPORT(con_write) int con_write(const void *p, int n);
/* キーの入力。**待たない。** 何も無ければ 0。
 * 中身は普通の端末と同じバイト列（矢印なら "\033[A" など）。 */
CDK_IMPORT(con_read)  int con_read(void *p, int n);
CDK_IMPORT(con_size)  int con_size(int *cols, int *rows);

/* ---- ファイル --------------------------------------------------------
 *
 * 触れるのは microSD (/sdcard) の中だけ。相対パスは /sdcard からの相対。
 * ".." で外へ出ようとすると CDK_E_PERM。同時に開けるのは 4 本まで。
 */
CDK_IMPORT(fs_open)     int fs_open(const char *path, int plen, int mode);
CDK_IMPORT(fs_read)     int fs_read(int fd, void *p, int n);
CDK_IMPORT(fs_write)    int fs_write(int fd, const void *p, int n);
CDK_IMPORT(fs_seek)     int fs_seek(int fd, int off, int whence);
CDK_IMPORT(fs_close)    int fs_close(int fd);
CDK_IMPORT(fs_size)     int fs_size(const char *path, int plen);
CDK_IMPORT(fs_unlink)   int fs_unlink(const char *path, int plen);
CDK_IMPORT(fs_mkdir)    int fs_mkdir(const char *path, int plen);
CDK_IMPORT(fs_opendir)  int fs_opendir(const char *path, int plen);
/* 名前を書いて長さを返す。0 なら終わり。 */
CDK_IMPORT(fs_readdir)  int fs_readdir(int dh, char *out, int cap);
CDK_IMPORT(fs_closedir) int fs_closedir(int dh);

/* ---- GPIO / ADC ------------------------------------------------------
 * 予約ピン（液晶・SD・USB・フラッシュ・デバッグ UART）は CDK_E_PERM。
 */
CDK_IMPORT(gpio_mode) int gpio_mode(int pin, int mode);
CDK_IMPORT(gpio_set)  int gpio_set(int pin, int level);
CDK_IMPORT(gpio_get)  int gpio_get(int pin);
CDK_IMPORT(adc_read)  int adc_read(int ch); /* mV */

/* ---- I2C ------------------------------------------------------------- */

CDK_IMPORT(i2c_open)  int i2c_open(int sda, int scl, int hz);
/* 1 アドレスだけ叩いて居るかを見る。**走査はこれで書く。**
 * 居なければ CDK_E_NOENT。「アドレス + 書き 0 バイト」しか出さないので
 * 相手は 1 ビットも吐き出さない。i2c_read で走査すると、予期しない読みを
 * 投げられた相手が来ない続きを待って SCL を握り続け、電源を抜くまで
 * バスごと死ぬ（実機の SHT30 で再現した）。 */
CDK_IMPORT(i2c_probe) int i2c_probe(int addr);
CDK_IMPORT(i2c_write) int i2c_write(int addr, const void *p, int n);
CDK_IMPORT(i2c_read)  int i2c_read(int addr, void *p, int n);
/* 書いてから読む。レジスタ読みはこれを使う。 */
CDK_IMPORT(i2c_wrrd)  int i2c_wrrd(int addr, const void *w, int wn, void *r, int rn);
CDK_IMPORT(i2c_close) int i2c_close(void);

/* ---- UART ------------------------------------------------------------ */

CDK_IMPORT(uart_open)  int uart_open(int tx, int rx, int baud);
CDK_IMPORT(uart_write) int uart_write(int h, const void *p, int n);
/* 待たない。何も無ければ 0。 */
CDK_IMPORT(uart_read)  int uart_read(int h, void *p, int n);
CDK_IMPORT(uart_close) int uart_close(int h);

/* ---- ネットワーク ---------------------------------------------------- */

CDK_IMPORT(net_up)       int net_up(void);
CDK_IMPORT(sock_open)    int sock_open(int type);
CDK_IMPORT(sock_connect) int sock_connect(int sd, const char *host, int hlen, int port);
CDK_IMPORT(sock_send)    int sock_send(int sd, const void *p, int n);
/* 200ms 待って何も来なければ 0 を返す。相手が閉じたら CDK_E_IO。 */
CDK_IMPORT(sock_recv)    int sock_recv(int sd, void *p, int n);
CDK_IMPORT(sock_close)   int sock_close(int sd);

/* ---- 画面描画（予約。まだ CDK_E_NOSYS が返る） ----------------------- */

CDK_IMPORT(gfx_size)      int gfx_size(int *w, int *h);
CDK_IMPORT(gfx_clear)     int gfx_clear(int rgb565);
CDK_IMPORT(gfx_fill_rect) int gfx_fill_rect(int x, int y, int w, int h, int rgb565);
CDK_IMPORT(gfx_blit)      int gfx_blit(int x, int y, int w, int h, const void *px, int n);
CDK_IMPORT(gfx_text)      int gfx_text(int x, int y, const void *s, int n, int fg, int bg);
CDK_IMPORT(gfx_present)   int gfx_present(void);

/* ---- SDK が用意する最小限（wasm-sdk/src/cdk_app.c） ------------------ */

void      *memcpy(void *d, const void *s, cdk_size_t n);
void      *memset(void *d, int c, cdk_size_t n);
void      *memmove(void *d, const void *s, cdk_size_t n);
int        memcmp(const void *a, const void *b, cdk_size_t n);
cdk_size_t strlen(const char *s);
int        strcmp(const char *a, const char *b);

/* NUL 終端の文字列を端末へ。 */
int cdk_print(const char *s);
/* %d %u %x %c %s %% だけ。幅指定もゼロ埋めも無い。 */
int cdk_printf(const char *fmt, ...);
/* 解放できないバンプ確保。線形メモリの残りから取る。 */
void *cdk_alloc(cdk_size_t n);

/* パスやホスト名を (ptr,len) で渡すときの短縮形。 */
#define CDK_S(lit) (lit), (int)(sizeof(lit) - 1)

#endif /* CDK_APP_H */
