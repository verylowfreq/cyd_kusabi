/* ユーザーアプリ向けの素の TCP / UDP ソケット。
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
 */
#include "cdk_sock.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_log.h"
#include "lwip/sockets.h"

static const char *TAG = "cdk_sock";

/* 刻みの幅。これより長く止まらない。 */
#define STEP_MS 100

/* 受信の上限。何も無ければ 0 を返す。 */
#define RECV_MS 200

int cdk_sock_open(int type)
{
    const int st = (type == CDK_SOCK_TYPE_UDP) ? SOCK_DGRAM : SOCK_STREAM;
    const int sd = socket(AF_INET, st, 0);
    if (sd < 0) {
        ESP_LOGE(TAG, "socket() に失敗 errno=%d", errno);
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
    const int sent = (int)send(sd, p, (size_t)n, 0);
    if (sent < 0) return -1;
    return sent;
}

int cdk_sock_recv(int sd, void *p, int n)
{
    if (sd < 0 || p == NULL || n <= 0) return -1;
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
    return close(sd);
}
