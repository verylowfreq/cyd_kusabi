/* SKK 辞書の読み出し口。詳細は cdk_skkdict_src.h。 */
#include "cdk_skkdict_src.h"

#include "cdk_storage.h"
#include "esp_log.h"
#include "esp_partition.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "cdk_skkdict";

static const esp_partition_t *s_part;
static FILE                  *s_file;
static cdk_skk_src_t          s_src;
static char                   s_desc[96];

/* ---- フラッシュ ---- */

static int read_flash(void *ctx, uint32_t off, void *buf, uint32_t len)
{
    const esp_partition_t *p = (const esp_partition_t *)ctx;
    if (p == NULL || off + len > p->size) {
        return -1;
    }
    return (esp_partition_read(p, off, buf, len) == ESP_OK) ? 0 : -1;
}

/* ---- SD 上のファイル ---- */

static int read_sd(void *ctx, uint32_t off, void *buf, uint32_t len)
{
    FILE *f = (FILE *)ctx;
    if (f == NULL) {
        return -1;
    }
    /* SD は LCD と SPI を共有する。読んでいる間はバスを押さえる。 */
    cdk_sd_lock();
    int rc = -1;
    if (fseek(f, (long)off, SEEK_SET) == 0 && fread(buf, 1, len, f) == len) {
        rc = 0;
    }
    cdk_sd_unlock();
    return rc;
}

/* ------------------------------------------------------------------ */

void cdk_skkdict_close(void)
{
    if (s_file != NULL) {
        cdk_sd_lock();
        fclose(s_file);
        cdk_sd_unlock();
        s_file = NULL;
    }
    s_part = NULL;
    s_src = CDK_SKK_SRC_NONE;
    s_desc[0] = '\0';
}

esp_err_t cdk_skkdict_open(cdk_skk_dict_t *d)
{
    cdk_skkdict_close();

    /* SD を先に見る。差し替えができるほうを優先する。 */
    if (cdk_sd_mounted()) {
        cdk_sd_lock();
        FILE *f = fopen(CDK_SKK_SD_PATH, "rb");
        cdk_sd_unlock();
        if (f != NULL) {
            if (cdk_skk_dict_open(d, read_sd, f)) {
                s_file = f;
                s_src = CDK_SKK_SRC_SD;
                snprintf(s_desc, sizeof(s_desc), "%s 送りあり%lu 送りなし%lu",
                         CDK_SKK_SD_PATH, (unsigned long)d->ari_count,
                         (unsigned long)d->nasi_count);
                ESP_LOGI(TAG, "%s", s_desc);
                return ESP_OK;
            }
            ESP_LOGW(TAG, "%s は SKK 辞書として読めません", CDK_SKK_SD_PATH);
            cdk_sd_lock();
            fclose(f);
            cdk_sd_unlock();
        }
    }

    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "skkdict");
    if (p == NULL) {
        ESP_LOGW(TAG, "skkdict パーティションがありません");
        return ESP_ERR_NOT_FOUND;
    }
    if (!cdk_skk_dict_open(d, read_flash, (void *)p)) {
        /* 焼いていないパーティションは 0xFF で埋まっている。
         * 「壊れている」ではなく「まだ入れていない」と出す。 */
        ESP_LOGW(TAG, "skkdict に辞書が入っていません "
                      "(tools/cdk.py dict で焼く)");
        return ESP_ERR_NOT_FOUND;
    }
    s_part = p;
    s_src = CDK_SKK_SRC_FLASH;
    snprintf(s_desc, sizeof(s_desc), "flash:skkdict 送りあり%lu 送りなし%lu",
             (unsigned long)d->ari_count, (unsigned long)d->nasi_count);
    ESP_LOGI(TAG, "%s", s_desc);
    return ESP_OK;
}

cdk_skk_src_t cdk_skkdict_source(void)
{
    return s_src;
}

/* ------------------------------------------------------------------ */
/* 個人辞書                                                            */
/* ------------------------------------------------------------------ */

static int s_user_written;

/* 学習のたびに 1 行追記する。全体を書き直さないのは、書いている途中で
 * 電源が落ちたときに辞書ごと失うのを避けるため。
 *
 * ここは**描画の外**から呼ばれる（route_key → 確定 → ここ → その後で描画）。
 * 描画中に SD を触ると LCD と SPI を奪い合う。 */
static void user_persist(void *ctx, const char *line, size_t len)
{
    (void)ctx;
    if (!cdk_sd_mounted()) {
        return;
    }
    cdk_sd_lock();
    FILE *f = fopen(CDK_SKK_USER_PATH, "ab");
    if (f != NULL) {
        if (fwrite(line, 1, len, f) == len) {
            s_user_written++;
        }
        fclose(f);
    }
    cdk_sd_unlock();
}

int cdk_skkuser_attach(cdk_skk_user_t *u)
{
    s_user_written = 0;
    if (!cdk_sd_mounted()) {
        ESP_LOGW(TAG, "SD が無いので学習を残せません");
        return 0;
    }

    cdk_sd_lock();
    mkdir("/sdcard/skk", 0777); /* 既にあってもよい */
    FILE *f = fopen(CDK_SKK_USER_PATH, "rb");
    cdk_sd_unlock();

    int loaded = 0;
    if (f != NULL) {
        char line[CDK_SKK_USER_LINE_MAX];
        cdk_sd_lock();
        while (fgets(line, sizeof(line), f) != NULL) {
            if (cdk_skk_user_feed_line(u, line)) {
                loaded++;
            }
        }
        fclose(f);
        cdk_sd_unlock();
        ESP_LOGI(TAG, "%s から %d 件", CDK_SKK_USER_PATH, loaded);
    }

    cdk_skk_user_set_persist(u, user_persist, NULL);
    return loaded;
}

int cdk_skkuser_written(void)
{
    return s_user_written;
}

const char *cdk_skkdict_describe(void)
{
    return (s_desc[0] != '\0') ? s_desc : "なし";
}
