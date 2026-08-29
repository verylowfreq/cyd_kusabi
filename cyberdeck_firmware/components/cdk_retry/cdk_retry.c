#include "cdk_retry.h"

#include <string.h>

void cdk_retry_init(cdk_retry_t *r, uint32_t base_ms, uint32_t cap_ms,
                    uint8_t max_attempts)
{
    if (r == NULL) {
        return;
    }
    memset(r, 0, sizeof(*r));
    /* **待ちは 2^31 ms を超えられない。** 時刻の一周をまたぐ判定を
     * 符号付きの差でやっているので、それより長い間隔は「もう過ぎた」と
     * 区別がつかなくなる。24 日待つ再接続に意味は無いので、ここで頭打ちにする。 */
    const uint32_t LIMIT = 0x7FFFFFFFu;

    r->base_ms = (base_ms > 0u) ? base_ms : 1u;
    if (r->base_ms > LIMIT) {
        r->base_ms = LIMIT;
    }
    r->cap_ms = (cap_ms >= r->base_ms) ? cap_ms : r->base_ms;
    if (r->cap_ms > LIMIT) {
        r->cap_ms = LIMIT;
    }
    r->max_attempts = max_attempts;
}

/* attempt 回目（1 始まり）の待ち時間。base << (attempt-1) を cap で止める。 */
static uint32_t delay_for(const cdk_retry_t *r, uint8_t attempt)
{
    uint32_t d = r->base_ms;
    for (uint8_t i = 1; i < attempt; i++) {
        /* **上限で止めてから倍にする。** 先に倍にすると 32bit を溢れて
         * 待ち時間が短くなり、上限を上げたつもりが逆に連打になる。 */
        if (d >= r->cap_ms) {
            return r->cap_ms;
        }
        d *= 2u;
    }
    return (d > r->cap_ms) ? r->cap_ms : d;
}

void cdk_retry_begin(cdk_retry_t *r, uint32_t now_ms)
{
    if (r == NULL) {
        return;
    }
    r->active = true;
    r->attempt = 0;
    r->next_at_ms = now_ms + delay_for(r, 1);
}

void cdk_retry_stop(cdk_retry_t *r)
{
    if (r != NULL) {
        r->active = false;
    }
}

bool cdk_retry_due(const cdk_retry_t *r, uint32_t now_ms)
{
    if (r == NULL || !r->active) {
        return false;
    }
    /* 符号付きで引くと一周をまたいでも「過ぎたか」を正しく判定できる。 */
    return (int32_t)(now_ms - r->next_at_ms) >= 0;
}

void cdk_retry_failed(cdk_retry_t *r, uint32_t now_ms)
{
    if (r == NULL || !r->active) {
        return;
    }
    if (r->attempt < 255u) {
        r->attempt++;
    }
    if (r->max_attempts != 0u && r->attempt >= r->max_attempts) {
        r->active = false;
        return;
    }
    r->next_at_ms = now_ms + delay_for(r, (uint8_t)(r->attempt + 1u));
}

uint32_t cdk_retry_remaining_ms(const cdk_retry_t *r, uint32_t now_ms)
{
    if (r == NULL || !r->active) {
        return 0;
    }
    const int32_t d = (int32_t)(r->next_at_ms - now_ms);
    return (d > 0) ? (uint32_t)d : 0u;
}

bool cdk_retry_exhausted(const cdk_retry_t *r)
{
    if (r == NULL) {
        return false;
    }
    return !r->active && r->max_attempts != 0u && r->attempt >= r->max_attempts;
}
