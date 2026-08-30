/* cdk_wasm_abi — ユーザーアプリ（WebAssembly）と本体の間の約束事
 *
 * WASI でも POSIX 互換でもない。この機械のための小さな独自 ABI。
 * 名乗らないので「実装していない関数」が「壊れている関数」にならない。
 *
 * 設計の縛り:
 *  - 引数は i32 と (ptr,len) の隣接ペアだけ。構造体は境界を跨がない。
 *  - ゲストへポインタを返さない。ハンドルは必ず小さな添字。
 *  - i64 は sys_now_ms の戻り値のみ。
 *
 * このヘッダは ESP-IDF に依存しない。ホスト側でもそのまま読む。
 */
#ifndef CDK_WASM_ABI_H
#define CDK_WASM_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* インポートモジュール名とエントリ。ゲスト SDK (wasm-sdk/) と対で持つ。 */
#define CDK_WASM_IMPORT_MODULE "cdk"
#define CDK_WASM_ENTRY         "cdk_main"

/* ゲストが触ってよいファイルの根。ここから外へは出られない。 */
#define CDK_WASM_ROOT     "/sdcard"
#define CDK_WASM_ROOT_LEN 7

/* エラーは負の戻り値。0 以上は成功（長さや値）。 */
enum {
    CDK_E_OK    =  0,
    CDK_E_INVAL = -1, /* 引数がおかしい */
    CDK_E_PERM  = -2, /* 禁止（予約ピン / サンドボックス外のパス） */
    CDK_E_NOENT = -3, /* 無い */
    CDK_E_IO    = -4, /* 入出力の失敗 */
    CDK_E_NOMEM = -5, /* 資源が足りない（ハンドル表・バッファ） */
    CDK_E_AGAIN = -6, /* いまは何も無い */
    CDK_E_NOSYS = -7, /* 未実装（gfx_* はこれ） */
    CDK_E_TIME  = -8, /* 時間切れ */
    CDK_E_ABORT = -9  /* Ctrl-C で止められた */
};

/* fs_open のモード */
enum { CDK_OPEN_READ = 0, CDK_OPEN_WRITE = 1, CDK_OPEN_APPEND = 2 };

/* fs_seek の基準 */
enum { CDK_SEEK_SET = 0, CDK_SEEK_CUR = 1, CDK_SEEK_END = 2 };

/* gpio_mode */
enum { CDK_GPIO_IN = 0, CDK_GPIO_IN_PU = 1, CDK_GPIO_IN_PD = 2, CDK_GPIO_OUT = 3 };

/* sock_open */
enum { CDK_SOCK_TCP = 0, CDK_SOCK_UDP = 1 };

/* 上限。ゲストから見える資源はすべて有限で、起動時に決まる。 */
enum {
    CDK_WASM_FILES_MAX  = 4,   /* FATFS の max_files=5 のうち 1 本は本体に残す */
    CDK_WASM_DIRS_MAX   = 2,
    CDK_WASM_SOCKS_MAX  = 4,
    CDK_WASM_UARTS_MAX  = 2,
    CDK_WASM_ARGV_MAX   = 8,
    CDK_WASM_ARGV_BYTES = 256,
    CDK_WASM_OUT_BYTES  = 4096, /* アプリ → 端末 */
    CDK_WASM_IN_BYTES   = 512,  /* 端末 → アプリ */
    CDK_WASM_PATH_MAX   = 192,
    CDK_WASM_NAME_MAX   = 128   /* fs_readdir が返す名前 */
};

/* ---- バックエンド ----------------------------------------------------
 *
 * 実機では port_wasm_backend.c が、ホストのテストでは偽物が埋める。
 * cdk_wasm はこの表しか叩かないので、ESP-IDF に依存しない。
 *
 * 約束:
 *  - path は cdk_wasm がサンドボックスを通した絶対パスだけを渡す。
 *  - 戻り値は上のエラーコードに従う。バックエンド固有の値を漏らさない。
 *  - file_* / dir_* のハンドルは void*。ゲストには見せない。
 *  - delay_ms は中断されたら CDK_E_ABORT を返す。
 */
typedef struct cdk_wasm_host {
    void *user;

    uint64_t (*now_ms)(void *user);
    int      (*delay_ms)(void *user, int ms);
    int      (*random)(void *user, void *out, int len);

    void *(*file_open)(void *user, const char *path, int mode);
    int   (*file_read)(void *user, void *h, void *buf, int len);
    int   (*file_write)(void *user, void *h, const void *buf, int len);
    int   (*file_seek)(void *user, void *h, int off, int whence);
    int   (*file_close)(void *user, void *h);
    int   (*file_size)(void *user, const char *path);
    int   (*file_unlink)(void *user, const char *path);
    int   (*file_mkdir)(void *user, const char *path);
    void *(*dir_open)(void *user, const char *path);
    int   (*dir_read)(void *user, void *h, char *out, int cap);
    int   (*dir_close)(void *user, void *h);

    int (*gpio_mode)(void *user, int pin, int mode);
    int (*gpio_set)(void *user, int pin, int level);
    int (*gpio_get)(void *user, int pin);
    int (*adc_read)(void *user, int ch);

    int (*i2c_open)(void *user, int sda, int scl, int hz);
    int (*i2c_write)(void *user, int addr, const void *p, int n);
    int (*i2c_read)(void *user, int addr, void *p, int n);
    int (*i2c_wrrd)(void *user, int addr, const void *w, int wn, void *r, int rn);
    int (*i2c_close)(void *user);

    int (*uart_open)(void *user, int tx, int rx, int baud);   /* 戻りは実ハンドル */
    int (*uart_write)(void *user, int h, const void *p, int n);
    int (*uart_read)(void *user, int h, void *p, int n);
    int (*uart_close)(void *user, int h);

    int (*net_up)(void *user);
    int (*sock_open)(void *user, int type);                   /* 戻りは実 fd */
    int (*sock_connect)(void *user, int sd, const char *host, int port);
    int (*sock_send)(void *user, int sd, const void *p, int n);
    int (*sock_recv)(void *user, int sd, void *p, int n);
    int (*sock_close)(void *user, int sd);
} cdk_wasm_host_t;

#ifdef __cplusplus
}
#endif

#endif /* CDK_WASM_ABI_H */
