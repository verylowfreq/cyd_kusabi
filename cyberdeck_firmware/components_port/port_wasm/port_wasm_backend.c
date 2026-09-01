/* cdk_wasm_host_t の実機側。ABI の呼び出しを実際のハードへ落とす。
 *
 * ここだけが ESP-IDF を知っている。cdk_wasm はこの表しか叩かない。
 *
 * SD は **1 呼び出しごとに cdk_sd_lock / cdk_sd_unlock で挟む。**
 * CoreS3 では液晶と SD が SPI を共有していて、しかも G35 の向きを
 * 持ち主ごとに切り替える必要がある。挟まないと画面が壊れるか SD が化ける。
 * 再帰ミューテックスなので、別タスク（wasm タスク）から取っても安全。
 */
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cdk_net.h"
#include "cdk_periph.h"
#include "cdk_sock.h"
#include "cdk_storage.h"
#include "cdk_wasm.h"
#include "cdk_wasm_run.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ---- sys ------------------------------------------------------------- */

static uint64_t h_now_ms(void *u)
{
    (void)u;
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static int h_delay(void *u, int ms)
{
    (void)u;
    /* **細かく刻んで、毎回 Ctrl-C を見る。** 一息に vTaskDelay すると
     * sys_delay(60000) を呼ばれたときに 1 分止められない。 */
    const int step = 20;
    for (int left = ms; left > 0; left -= step) {
        if (cdk_wasm_stop_requested()) return CDK_E_ABORT;
        const int d = (left < step) ? left : step;
        vTaskDelay(pdMS_TO_TICKS(d));
    }
    return cdk_wasm_stop_requested() ? CDK_E_ABORT : CDK_E_OK;
}

static int h_random(void *u, void *out, int len)
{
    (void)u;
    esp_fill_random(out, (size_t)len);
    return len;
}

/* ---- ファイル --------------------------------------------------------
 *
 * 実体は FILE* / DIR*。ゲストには表の添字しか渡らない（cdk_wasm 側）。
 */

static void *h_file_open(void *u, const char *path, int mode)
{
    (void)u;
    const char *m = (mode == CDK_OPEN_WRITE) ? "wb"
                  : (mode == CDK_OPEN_APPEND) ? "ab"
                                              : "rb";
    if (!cdk_sd_mounted()) return NULL;
    cdk_sd_lock();
    FILE *f = fopen(path, m);
    cdk_sd_unlock();
    return f;
}

static int h_file_read(void *u, void *h, void *buf, int len)
{
    (void)u;
    cdk_sd_lock();
    const size_t n = fread(buf, 1, (size_t)len, (FILE *)h);
    const int    bad = ferror((FILE *)h);
    cdk_sd_unlock();
    if (n == 0 && bad) return CDK_E_IO;
    return (int)n;
}

static int h_file_write(void *u, void *h, const void *buf, int len)
{
    (void)u;
    cdk_sd_lock();
    const size_t n = fwrite(buf, 1, (size_t)len, (FILE *)h);
    cdk_sd_unlock();
    if (n == 0 && len > 0) return CDK_E_IO;
    return (int)n;
}

static int h_file_seek(void *u, void *h, int off, int whence)
{
    (void)u;
    const int w = (whence == CDK_SEEK_CUR) ? SEEK_CUR
                : (whence == CDK_SEEK_END) ? SEEK_END
                                           : SEEK_SET;
    cdk_sd_lock();
    const int r = fseek((FILE *)h, (long)off, w);
    const long pos = ftell((FILE *)h);
    cdk_sd_unlock();
    if (r != 0 || pos < 0) return CDK_E_IO;
    return (int)pos;
}

static int h_file_close(void *u, void *h)
{
    (void)u;
    cdk_sd_lock();
    const int r = fclose((FILE *)h);
    cdk_sd_unlock();
    return (r == 0) ? CDK_E_OK : CDK_E_IO;
}

static int h_file_size(void *u, const char *path)
{
    (void)u;
    struct stat st;
    if (!cdk_sd_mounted()) return CDK_E_IO;
    cdk_sd_lock();
    const int r = stat(path, &st);
    cdk_sd_unlock();
    if (r != 0) return CDK_E_NOENT;
    return (int)st.st_size;
}

static int h_file_unlink(void *u, const char *path)
{
    (void)u;
    if (!cdk_sd_mounted()) return CDK_E_IO;
    cdk_sd_lock();
    const int r = unlink(path);
    cdk_sd_unlock();
    return (r == 0) ? CDK_E_OK : CDK_E_NOENT;
}

static int h_file_mkdir(void *u, const char *path)
{
    (void)u;
    if (!cdk_sd_mounted()) return CDK_E_IO;
    cdk_sd_lock();
    const int r = mkdir(path, 0775);
    cdk_sd_unlock();
    return (r == 0) ? CDK_E_OK : CDK_E_IO;
}

static void *h_dir_open(void *u, const char *path)
{
    (void)u;
    if (!cdk_sd_mounted()) return NULL;
    cdk_sd_lock();
    DIR *d = opendir(path);
    cdk_sd_unlock();
    return d;
}

static int h_dir_read(void *u, void *h, char *out, int cap)
{
    (void)u;
    cdk_sd_lock();
    const struct dirent *e = readdir((DIR *)h);
    cdk_sd_unlock();
    if (!e) return 0; /* 終わり */

    const int n = (int)strlen(e->d_name);
    if (n + 1 > cap) return CDK_E_NOMEM;
    memcpy(out, e->d_name, (size_t)n + 1);
    return n;
}

static int h_dir_close(void *u, void *h)
{
    (void)u;
    cdk_sd_lock();
    (void)closedir((DIR *)h);
    cdk_sd_unlock();
    return CDK_E_OK;
}

/* ---- GPIO / ADC ------------------------------------------------------
 *
 * 予約ピンの表は port_periph が持っている。**そこを通す**ことが大事で、
 * ここで別の判定を書くと 2 か所に分かれて片方だけ古くなる。
 */

static int map_err(esp_err_t e)
{
    switch (e) {
    case ESP_OK:                 return CDK_E_OK;
    case ESP_ERR_INVALID_ARG:    return CDK_E_PERM;  /* 予約ピン・範囲外 */
    case ESP_ERR_INVALID_STATE:  return CDK_E_PERM;
    case ESP_ERR_NO_MEM:         return CDK_E_NOMEM;
    case ESP_ERR_TIMEOUT:        return CDK_E_TIME;
    case ESP_ERR_NOT_FOUND:      return CDK_E_NOENT;
    default:                     return CDK_E_IO;
    }
}

static int h_gpio_mode(void *u, int pin, int mode)
{
    (void)u;
    static const char *const NAMES[] = {"in", "in_pu", "in_pd", "out"};
    return map_err(cdk_gpio_mode(pin, NAMES[mode]));
}

static int h_gpio_set(void *u, int pin, int level)
{
    (void)u;
    return map_err(cdk_gpio_set(pin, level));
}

static int h_gpio_get(void *u, int pin)
{
    (void)u;
    int level = 0;
    const esp_err_t e = cdk_gpio_get(pin, &level);
    return (e == ESP_OK) ? level : map_err(e);
}

static int h_adc_read(void *u, int ch)
{
    (void)u;
    int raw = 0, mv = 0;
    const esp_err_t e = cdk_adc_read(ch, &raw, &mv);
    if (e != ESP_OK) return map_err(e);
    /* 校正が使えないときは mv が -1 になる。生値を返すより、
     * 使えないと言った方がアプリ側で分岐できる。 */
    return (mv >= 0) ? mv : CDK_E_IO;
}

/* ---- I2C ------------------------------------------------------------- */

static int h_i2c_open(void *u, int sda, int scl, int hz)
{
    (void)u;
    return map_err(cdk_i2c_open(sda, scl, (uint32_t)hz));
}

static int h_i2c_write(void *u, int addr, const void *p, int n)
{
    (void)u;
    return map_err(cdk_i2c_write(addr, p, (size_t)n));
}

static int h_i2c_read(void *u, int addr, void *p, int n)
{
    (void)u;
    return map_err(cdk_i2c_read(addr, p, (size_t)n));
}

static int h_i2c_wrrd(void *u, int addr, const void *w, int wn, void *r, int rn)
{
    (void)u;
    return map_err(cdk_i2c_write_read(addr, w, (size_t)wn, r, (size_t)rn));
}

static int h_i2c_close(void *u)
{
    (void)u;
    return map_err(cdk_i2c_close());
}

/* ---- UART ------------------------------------------------------------
 *
 * **1 本しかない。** cdk_wasm 側の上限も 1 にしてあるが、断れる側は
 * こちらなので、ここでも二重に開けないことを見る。
 */

static int h_uart_open(void *u, int tx, int rx, int baud)
{
    (void)u;
    if (cdk_uart_is_open()) return CDK_E_NOMEM;
    const esp_err_t e = cdk_uart_open(tx, rx, baud);
    if (e != ESP_OK) return map_err(e);
    return 0; /* 実ハンドルは 1 つだけ。cdk_wasm が添字に置き換える */
}

static int h_uart_write(void *u, int h, const void *p, int n)
{
    (void)u; (void)h;
    const int r = cdk_uart_write(p, n);
    return (r < 0) ? CDK_E_IO : r;
}

static int h_uart_read(void *u, int h, void *p, int n)
{
    (void)u; (void)h;
    const int r = cdk_uart_read(p, n);
    return (r < 0) ? CDK_E_IO : r;
}

static int h_uart_close(void *u, int h)
{
    (void)u; (void)h;
    return map_err(cdk_uart_close());
}

/* ---- ネットワーク -----------------------------------------------------
 *
 * 接続待ちの間も Ctrl-C が効くように、abort を関数ポインタで渡す。
 * port_net 側は port_wasm を知らない。
 */

static int h_net_up(void *u)
{
    (void)u;
    return cdk_wifi_is_connected() ? 1 : 0;
}

static int h_sock_open(void *u, int type)
{
    (void)u;
    if (!cdk_wifi_is_connected()) return CDK_E_IO;

    int t;
    switch (type) {
    case CDK_SOCK_UDP:           t = CDK_SOCK_TYPE_UDP; break;
    case CDK_SOCK_TLS:           t = CDK_SOCK_TYPE_TLS; break;
    case CDK_SOCK_TLS_INSECURE:  t = CDK_SOCK_TYPE_TLS_INSECURE; break;
    default:                     t = CDK_SOCK_TYPE_TCP; break;
    }

    const int sd = cdk_sock_open(t);
    if (sd >= 0) return sd;
    /* TLS は本数を使い切ると開けない。ソケットが尽きたのと同じ意味なので
     * CDK_E_NOMEM で返す。それ以外の失敗と区別できないが、ゲストから見て
     * どちらも「いま開けない」で扱いは変わらない。 */
    return (t == CDK_SOCK_TYPE_TLS || t == CDK_SOCK_TYPE_TLS_INSECURE) ? CDK_E_NOMEM
                                                                       : CDK_E_IO;
}

static int h_sock_connect(void *u, int sd, const char *host, int port)
{
    (void)u;
    const int r = cdk_sock_connect(sd, host, port, 10000, cdk_wasm_stop_requested);
    switch (r) {
    case 0:                return CDK_E_OK;
    case CDK_SOCK_TIMEOUT: return CDK_E_TIME;
    case CDK_SOCK_ABORTED: return CDK_E_ABORT;
    default:               return CDK_E_IO;
    }
}

static int h_sock_send(void *u, int sd, const void *p, int n)
{
    (void)u;
    const int r = cdk_sock_send(sd, p, n);
    return (r < 0) ? CDK_E_IO : r;
}

static int h_sock_recv(void *u, int sd, void *p, int n)
{
    (void)u;
    const int r = cdk_sock_recv(sd, p, n);
    if (r >= 0) return r;                      /* 0 = いまは何も無い */
    if (r == CDK_SOCK_CLOSED) return CDK_E_IO; /* 相手が閉じた */
    return CDK_E_IO;
}

static int h_sock_close(void *u, int sd)
{
    (void)u;
    return (cdk_sock_close(sd) == 0) ? CDK_E_OK : CDK_E_IO;
}

/* ---- 表 -------------------------------------------------------------- */

static const cdk_wasm_host_t k_backend = {
    .user = NULL,

    .now_ms   = h_now_ms,
    .delay_ms = h_delay,
    .random   = h_random,

    .file_open   = h_file_open,
    .file_read   = h_file_read,
    .file_write  = h_file_write,
    .file_seek   = h_file_seek,
    .file_close  = h_file_close,
    .file_size   = h_file_size,
    .file_unlink = h_file_unlink,
    .file_mkdir  = h_file_mkdir,
    .dir_open    = h_dir_open,
    .dir_read    = h_dir_read,
    .dir_close   = h_dir_close,

    .gpio_mode = h_gpio_mode,
    .gpio_set  = h_gpio_set,
    .gpio_get  = h_gpio_get,
    .adc_read  = h_adc_read,

    .i2c_open  = h_i2c_open,
    .i2c_write = h_i2c_write,
    .i2c_read  = h_i2c_read,
    .i2c_wrrd  = h_i2c_wrrd,
    .i2c_close = h_i2c_close,

    .uart_open  = h_uart_open,
    .uart_write = h_uart_write,
    .uart_read  = h_uart_read,
    .uart_close = h_uart_close,

    .net_up       = h_net_up,
    .sock_open    = h_sock_open,
    .sock_connect = h_sock_connect,
    .sock_send    = h_sock_send,
    .sock_recv    = h_sock_recv,
    .sock_close   = h_sock_close,
};

const cdk_wasm_host_t *cdk_wasm_backend(void) { return &k_backend; }
