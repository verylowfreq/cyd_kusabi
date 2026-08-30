/* ホスト PC から microSD へファイルを書く（`cdk.py push`）。
 *
 * これが無いと、ユーザーアプリを更新するのに SD を抜いて PC に挿すしかなく、
 * エージェントが自分で書いて自分で試すループが回らない。
 *
 * 書ける場所は cdk_wasm と同じサンドボックス（/sdcard の下だけ）。
 * 判定は cdk_wasm_path_resolve に任せる。**規則を 2 つ持たない。**
 */
#include "app.h"

#include "cdk_link.h"
#include "cdk_proto.h"
#include "cdk_storage.h"
#include "cdk_wasm.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "push";

/* 受信中の 1 本ぶん。同時に 1 本だけ。 */
static FILE    *s_fp;
static char     s_path[CDK_WASM_PATH_MAX];
static uint32_t s_want;    /* BEGIN で言われた全体の長さ */
static uint32_t s_got;     /* 実際に書いたバイト数 */
static uint32_t s_crc;     /* 中身の CRC32（継続計算） */
static bool     s_failed;  /* 途中で失敗した。END まで黙って捨てる */

static void fail(const char *msg)
{
    s_failed = true;
    if (s_fp) {
        cdk_sd_lock();
        fclose(s_fp);
        cdk_sd_unlock();
        s_fp = NULL;
    }
    ESP_LOGW(TAG, "%s", msg);
    (void)cdk_link_send(CDK_MSG_ERROR, msg, strlen(msg));
}

static void reset_state(void)
{
    if (s_fp) {
        cdk_sd_lock();
        fclose(s_fp);
        cdk_sd_unlock();
        s_fp = NULL;
    }
    s_want = s_got = 0;
    s_crc = 0;
    s_failed = false;
    s_path[0] = '\0';
}

void cdk_push_begin(const uint8_t *payload, uint32_t len)
{
    reset_state();

    if (len < 5) { fail("push: BEGIN が短すぎます"); return; }

    memcpy(&s_want, payload, 4);
    const char    *path = (const char *)payload + 4;
    const uint32_t plen = len - 4;

    if (cdk_wasm_path_resolve(path, (int)plen, s_path, (int)sizeof s_path) != CDK_E_OK) {
        fail("push: microSD の外へは書けません");
        return;
    }
    if (!cdk_sd_mounted()) { fail("push: SD がマウントされていません"); return; }

    cdk_sd_lock();
    s_fp = fopen(s_path, "wb");
    cdk_sd_unlock();
    if (!s_fp) {
        /* 途中のディレクトリが無いのがいちばん多い。理由を具体的に返す。 */
        char msg[CDK_WASM_PATH_MAX + 128];
        snprintf(msg, sizeof msg, "push: %s を開けません（ディレクトリはありますか）",
                 s_path);
        fail(msg);
        return;
    }
    ESP_LOGI(TAG, "%s へ %u バイト受け取ります", s_path, (unsigned)s_want);
}

void cdk_push_data(const uint8_t *payload, uint32_t len)
{
    if (s_failed) return;
    if (!s_fp) { fail("push: BEGIN より先に DATA が来ました"); return; }
    if (len == 0) return;

    /* 言われた長さを超えたら止める。ディスクを埋めさせない。 */
    if (s_got + len > s_want) { fail("push: 言われた長さを超えました"); return; }

    cdk_sd_lock();
    const size_t n = fwrite(payload, 1, len, s_fp);
    cdk_sd_unlock();
    if (n != len) { fail("push: 書き込みに失敗しました（容量は足りていますか）"); return; }

    s_crc = cdk_crc32(s_crc, payload, len);
    s_got += (uint32_t)n;
}

void cdk_push_end(const uint8_t *payload, uint32_t len)
{
    if (s_failed) { reset_state(); return; }
    if (!s_fp) { fail("push: BEGIN より先に END が来ました"); reset_state(); return; }

    cdk_sd_lock();
    const int closed = fclose(s_fp);
    cdk_sd_unlock();
    s_fp = NULL;

    if (closed != 0) { fail("push: 閉じられませんでした"); reset_state(); return; }
    if (s_got != s_want) {
        fail("push: 長さが合いません（フレームが落ちています）");
        reset_state();
        return;
    }
    if (len >= 4) {
        uint32_t want_crc = 0;
        memcpy(&want_crc, payload, 4);
        if (want_crc != s_crc) {
            fail("push: CRC が合いません（中身が壊れています）");
            reset_state();
            return;
        }
    }

    ESP_LOGI(TAG, "%s に %u バイト書きました", s_path, (unsigned)s_got);
    const uint32_t wrote = s_got;
    (void)cdk_link_send(CDK_MSG_FILE_OK, &wrote, sizeof(wrote));
    reset_state();
}
