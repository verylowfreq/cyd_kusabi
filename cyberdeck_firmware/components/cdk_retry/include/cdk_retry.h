/* cdk_retry — 指数バックオフの小さな状態機械
 *
 * ESP-IDF に依存しない純 C。時刻は呼び出し側が渡す。
 *
 * SSH が切れたときの再接続に使う。**待ち時間を持たずに即座に繋ぎ直すと、
 * 相手が落ちている間ずっと全力で叩き続けることになる。**
 * 1 秒 → 2 → 4 → 8 → …と延ばし、上限で頭打ちにする。
 */
#ifndef CDK_RETRY_H
#define CDK_RETRY_H

#include <stdbool.h>
#include <stddef.h> /* インライン関数で NULL を見るので要る */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     active;
    uint8_t  attempt;      /* これまでに試した回数 */
    uint8_t  max_attempts; /* 0 なら無制限 */
    uint32_t base_ms;
    uint32_t cap_ms;
    uint32_t next_at_ms;
} cdk_retry_t;

void cdk_retry_init(cdk_retry_t *r, uint32_t base_ms, uint32_t cap_ms,
                    uint8_t max_attempts);

/* 再接続を始める。最初の 1 回は base_ms 待ってから。
 * **すぐには試さない。** 切れた直後は相手もこちらも整っていないことが多い。 */
void cdk_retry_begin(cdk_retry_t *r, uint32_t now_ms);

void cdk_retry_stop(cdk_retry_t *r);

static inline bool cdk_retry_active(const cdk_retry_t *r)
{
    return (r != NULL) && r->active;
}

/* 次の試行の時刻が来たか。**符号付きの差で比べるので時刻が一周しても動く。** */
bool cdk_retry_due(const cdk_retry_t *r, uint32_t now_ms);

/* 1 回試して駄目だった。次を予約する。上限に達したら active が false になる。 */
void cdk_retry_failed(cdk_retry_t *r, uint32_t now_ms);

/* 次の試行までの残り (ms)。画面に出す用。過ぎていれば 0。 */
uint32_t cdk_retry_remaining_ms(const cdk_retry_t *r, uint32_t now_ms);

/* 何回目を試すところか（1 始まり）。 */
static inline uint8_t cdk_retry_next_attempt(const cdk_retry_t *r)
{
    return (uint8_t)((r != NULL) ? r->attempt + 1u : 0u);
}

/* 上限まで試して諦めたか。 */
bool cdk_retry_exhausted(const cdk_retry_t *r);

#ifdef __cplusplus
}
#endif

#endif /* CDK_RETRY_H */
