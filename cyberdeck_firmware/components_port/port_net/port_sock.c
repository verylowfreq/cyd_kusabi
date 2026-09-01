/* ユーザーアプリ向けの素の TCP / UDP ソケットと、TLS の口。
 *
 * SSH は port_ssh.c が 1 本だけ握っている。こちらはアプリが自由に開ける口で、
 * lwIP の BSD ソケットをそのまま薄く包む。
 *
 * ## 中断できることが要件
 *
 * **接続も受信も、呼び出し側が止めたくなったら止められること。**
 * ユーザーアプリは Ctrl-C で止められる約束になっていて、止まれるのは
 * native の入口だけ。ここでブロックすると、いちばん詰まりやすい API が
 * いちばん止まらない API になる。
 *
 * だから:
 *  - connect は非ブロッキングにして、poll で刻みながら abort を見る
 *  - recv は 200ms で必ず戻る（何も無ければ 0）
 *
 * abort は関数ポインタで受ける。port_net が port_wasm を知らずに済む。
 *
 * ## TLS (DESIGN.md §15.7)
 *
 * **TLS は自前で実装せず ESP-IDF (esp-tls + 証明書バンドル) に任せる。**
 * ゲストから見える API は増やさない。`cdk_sock_open` の型に TLS を足すと、
 * connect / send / recv / close はそのままの名前で TLS 越しに動く。
 *
 * 中身は esp_tls_t なので fd では表せない。そこで `TLS_BASE + i` という
 * 「fd のふりをする整数」を返し、この中だけで実体に引き直す。上の層
 * (port_wasm_backend → cdk_wasm) は中身を見ないので何も変えなくていい。
 */
#include "cdk_sock.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "cdk_sock";

/* 刻みの幅。これより長く止まらない。 */
#define STEP_MS 100

/* 受信の上限。何も無ければ 0 を返す。 */
#define RECV_MS 200

/* ---- TLS のハンドル表 ------------------------------------------------
 *
 * **同時 2 本まで。** ここは机上の数ではなく実機で決めた。
 *
 * mbedtls のセッションは in 16KB + out 4KB を持つ。既定の
 * CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC のままだとこれが内部 DRAM から取られ、
 * この基板は空きが 75KB あっても連続では 31,744 バイトしか取れないので
 * 2 本目で破綻した（DESIGN.md §15.6）。そのときログに出たのは
 *   E esp-aes: Failed to allocate memory
 *   E esp-tls-mbedtls: mbedtls_ssl_handshake returned -0x0001
 * で、**壊れるのは 2 本目だけではなかった。** 内部 DRAM が枯れると、既に
 * 繋がっている 1 本目の read / write も esp-aes の確保に失敗して落ちる。
 * 4 本試したときは再起動した。
 *
 * 効いたのは `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`（sdkconfig.defaults）。
 * esp-aes の DMA 記述子は MALLOC_CAP_DMA なので内部 DRAM からしか取れず、
 * そこを空けてやる必要がある。mbedtls 本体を PSRAM へ追い出すと、2 本とも
 * ハンドシェイクから送受信まで通る。SSH を挟んでも通ることを確かめた。
 *
 * 上限を増やすときは**必ず実機で、SSH を挟んで繰り返し**試すこと。
 * 一度は内部 DRAM のままでも 2 本通ってしまったことがある。
 * **たまたま通る数を上限にしてはいけない。**
 * （4 本も一度は通ったが、2 本ほど繰り返し確かめていないので上限は 2。）
 *
 * **`TLS_BASE` が実 fd と衝突しないのは lwIP の fd が小さいから。**
 * lwIP の fd は LWIP_SOCKET_OFFSET(=0) から CONFIG_LWIP_MAX_SOCKETS(既定 16)
 * までしかない。将来ここが 0x7000 を超えるほど大きくなると黙って壊れるので、
 * cdk_sock_open で実 fd が TLS_BASE 未満であることを実行時に確かめている。
 */
#define TLS_MAX  2
#define TLS_BASE 0x7000

static struct {
    esp_tls_t *tls;
    bool       used;
    bool       insecure; /* true なら証明書を検証しない */
} s_tls[TLS_MAX];

static bool is_tls(int sd)
{
    return sd >= TLS_BASE && sd < TLS_BASE + TLS_MAX;
}

/* is_tls() を通った sd だけ渡すこと。 */
static int tls_idx(int sd)
{
    return sd - TLS_BASE;
}

/* ハンドシェイクが済んだ後、下のソケットを素の TCP と同じ設定に戻す。
 *
 * **これを忘れると recv の「200ms 待つ」という約束が崩れる。** 接続中は
 * 刻んで abort を見るために非ブロッキングにしてあるので、そのままだと
 * esp_tls_conn_read が即 WANT_READ で戻り、こちらは 0 を返し、
 * ゲストが 240MHz で空回りする。 */
static void tls_settle_socket(esp_tls_t *tls)
{
    int fd = -1;
    if (esp_tls_get_conn_sockfd(tls, &fd) != ESP_OK || fd < 0) {
        ESP_LOGW(TAG, "TLS の下のソケットが取れません");
        return;
    }

    const int flags = fcntl(fd, F_GETFL, 0);
    (void)fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    struct timeval tv = {.tv_sec = 0, .tv_usec = RECV_MS * 1000};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* 対話的な用途が多いので Nagle は切る。素の TCP と揃える。 */
    int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

static int tls_open(bool insecure)
{
    int slot = -1;
    for (int i = 0; i < TLS_MAX; i++)
        if (!s_tls[i].used) { slot = i; break; }
    if (slot < 0) {
        ESP_LOGW(TAG, "TLS は同時 %d 本までです", TLS_MAX);
        return -1;
    }

    esp_tls_t *t = esp_tls_init();
    if (t == NULL) {
        ESP_LOGE(TAG, "esp_tls_init に失敗（内部 DRAM 不足の疑い）");
        return -1;
    }

    s_tls[slot].tls = t;
    s_tls[slot].used = true;
    s_tls[slot].insecure = insecure;
    return TLS_BASE + slot;
}

static int tls_connect(int sd, const char *host, int port, int timeout_ms,
                       cdk_sock_abort_fn abort_fn)
{
    const int i = tls_idx(sd);
    if (!s_tls[i].used) return -1;

    /* **cfg はループの外で 1 回だけ組む。** esp_tls_conn_new_async は
     * 呼び直すたびに同じ引数を渡す約束で、状態は tls 側が持っている。 */
    esp_tls_cfg_t cfg = {
        .timeout_ms = timeout_ms,
        .non_block  = true,
    };
    if (s_tls[i].insecure) {
        /* crt_bundle_attach は NULL のまま。CA を一切指定しないと
         * CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY の既定に落ちて検証しない。
         * skip_common_name はホスト名の照合（と SNI）を止めるだけで、
         * これだけでは連鎖の検証は止まらない。 */
        cfg.skip_common_name = true;
    } else {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    const int hlen = (int)strlen(host);
    int       waited = 0;
    for (;;) {
        if (abort_fn != NULL && abort_fn()) {
            ESP_LOGI(TAG, "接続を中止しました");
            return CDK_SOCK_ABORTED;
        }

        const int r = esp_tls_conn_new_async(host, hlen, port, &cfg, s_tls[i].tls);
        if (r == 1) {
            tls_settle_socket(s_tls[i].tls);
            return 0;
        }
        if (r < 0) {
            ESP_LOGW(TAG, "TLS で繋がりません %s:%d", host, port);
            return -1;
        }

        if (waited >= timeout_ms) {
            ESP_LOGW(TAG, "TLS の接続がタイムアウトしました (%d ms)", timeout_ms);
            return CDK_SOCK_TIMEOUT;
        }

        /* **自前で刻む。** 素の TCP は select のタイムアウトが刻みを
         * 作っていたが、non_block の esp_tls_conn_new_async は進行中でも
         * 即 0 で戻るので、ここで待たないと全力で回り続ける。 */
        vTaskDelay(pdMS_TO_TICKS(STEP_MS));
        waited += STEP_MS;
    }
}

/* ---- 素の TCP / UDP と TLS の共通の入口 -------------------------------- */

int cdk_sock_open(int type)
{
    if (type == CDK_SOCK_TYPE_TLS)          return tls_open(false);
    if (type == CDK_SOCK_TYPE_TLS_INSECURE) return tls_open(true);

    const int st = (type == CDK_SOCK_TYPE_UDP) ? SOCK_DGRAM : SOCK_STREAM;
    const int sd = socket(AF_INET, st, 0);
    if (sd < 0) {
        ESP_LOGE(TAG, "socket() に失敗 errno=%d", errno);
        return -1;
    }

    /* 実 fd が TLS のふりをする値に届いてしまうと、下の is_tls() が
     * 素のソケットを TLS と誤認する。lwIP の設定を変えない限り起きないが、
     * 黙って壊れるより断る。 */
    if (sd >= TLS_BASE) {
        ESP_LOGE(TAG, "fd=%d が TLS_BASE を超えました。TLS_BASE を上げること", sd);
        close(sd);
        return -1;
    }

    /* 受信は必ず有限時間で戻す。ここを既定のままにすると、
     * 相手が黙ったときにアプリごと止まって Ctrl-C も効かない。 */
    struct timeval tv = {.tv_sec = 0, .tv_usec = RECV_MS * 1000};
    (void)setsockopt(sd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return sd;
}

int cdk_sock_connect(int sd, const char *host, int port, int timeout_ms,
                     cdk_sock_abort_fn abort_fn)
{
    if (sd < 0 || host == NULL || port <= 0 || port > 65535) return -1;

    if (is_tls(sd)) return tls_connect(sd, host, port, timeout_ms, abort_fn);

    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%d", port);

    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    /* 名前解決は刻めない（getaddrinfo は途中で戻せない）。lwIP の DNS には
     * 自前のタイムアウトがあるので、ここは有限時間で必ず返ってくる。 */
    struct addrinfo *res = NULL;
    const int        gai = getaddrinfo(host, portstr, &hints, &res);
    if (gai != 0 || res == NULL) {
        ESP_LOGW(TAG, "名前解決に失敗 %s (gai=%d)", host, gai);
        return -1;
    }

    /* 非ブロッキングにしてから繋ぐ。そうしないと刻んで abort を見られない。 */
    const int flags = fcntl(sd, F_GETFL, 0);
    (void)fcntl(sd, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(sd, res->ai_addr, res->ai_addrlen);
    if (rc != 0 && errno != EINPROGRESS) {
        ESP_LOGW(TAG, "connect() に失敗 errno=%d", errno);
        freeaddrinfo(res);
        (void)fcntl(sd, F_SETFL, flags);
        return -1;
    }
    freeaddrinfo(res);

    int waited = 0;
    for (;;) {
        if (abort_fn != NULL && abort_fn()) {
            ESP_LOGI(TAG, "接続を中止しました");
            (void)fcntl(sd, F_SETFL, flags);
            return CDK_SOCK_ABORTED;
        }
        if (waited >= timeout_ms) {
            ESP_LOGW(TAG, "接続がタイムアウトしました (%d ms)", timeout_ms);
            (void)fcntl(sd, F_SETFL, flags);
            return CDK_SOCK_TIMEOUT;
        }

        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(sd, &wf);
        struct timeval tv = {.tv_sec = 0, .tv_usec = STEP_MS * 1000};

        const int n = select(sd + 1, NULL, &wf, NULL, &tv);
        if (n < 0) {
            (void)fcntl(sd, F_SETFL, flags);
            return -1;
        }
        if (n > 0) {
            /* 書けるようになった＝結果が出た。成否は SO_ERROR で見る。
             * select が返っただけでは失敗も成功も区別できない。 */
            int       soerr = 0;
            socklen_t len = sizeof(soerr);
            (void)getsockopt(sd, SOL_SOCKET, SO_ERROR, &soerr, &len);
            (void)fcntl(sd, F_SETFL, flags);
            if (soerr != 0) {
                ESP_LOGW(TAG, "接続できません errno=%d", soerr);
                return -1;
            }
            /* 対話的な用途が多いので Nagle は切る。 */
            int one = 1;
            (void)setsockopt(sd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            return 0;
        }
        waited += STEP_MS;
    }
}

int cdk_sock_send(int sd, const void *p, int n)
{
    if (sd < 0 || p == NULL || n <= 0) return -1;

    if (is_tls(sd)) {
        const int i = tls_idx(sd);
        if (!s_tls[i].used) return -1;
        const int sent = (int)esp_tls_conn_write(s_tls[i].tls, p, (size_t)n);
        if (sent >= 0) return sent;
        if (sent == ESP_TLS_ERR_SSL_WANT_READ || sent == ESP_TLS_ERR_SSL_WANT_WRITE) {
            return 0; /* まだ送れていない。呼び直せばよい */
        }
        return -1;
    }

    const int sent = (int)send(sd, p, (size_t)n, 0);
    if (sent < 0) return -1;
    return sent;
}

int cdk_sock_recv(int sd, void *p, int n)
{
    if (sd < 0 || p == NULL || n <= 0) return -1;

    if (is_tls(sd)) {
        const int i = tls_idx(sd);
        if (!s_tls[i].used) return -1;
        /* esp_tls_conn_read は相手が閉じたとき（close_notify を含む）に 0 を
         * 返す。素の recv と同じ意味なので、そのまま CDK_SOCK_CLOSED に写す。
         * 「まだ来ていない」は 0 ではなく WANT_READ で来るので取り違えない。 */
        const int got = (int)esp_tls_conn_read(s_tls[i].tls, p, (size_t)n);
        if (got > 0) return got;
        if (got == 0) return CDK_SOCK_CLOSED; /* 相手が閉じた */
        if (got == ESP_TLS_ERR_SSL_WANT_READ || got == ESP_TLS_ERR_SSL_WANT_WRITE) {
            return 0; /* いまは何も無い。呼び直せばよい */
        }
        return -1;
    }

    const int got = (int)recv(sd, p, (size_t)n, 0);
    if (got > 0) return got;
    if (got == 0) return CDK_SOCK_CLOSED; /* 相手が閉じた */
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
        return 0; /* いまは何も無い。呼び直せばよい */
    }
    return -1;
}

int cdk_sock_close(int sd)
{
    if (sd < 0) return -1;

    if (is_tls(sd)) {
        const int i = tls_idx(sd);
        if (!s_tls[i].used) return -1;
        /* 繋がる前に閉じられても esp_tls_conn_destroy でよい
         * （esp_tls_init しかしていない状態も片付けてくれる）。 */
        const int r = esp_tls_conn_destroy(s_tls[i].tls);
        s_tls[i].tls = NULL;
        s_tls[i].used = false;
        s_tls[i].insecure = false;
        return r;
    }

    return close(sd);
}
