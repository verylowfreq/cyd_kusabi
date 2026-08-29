/* 実機で SSH の鍵を作る。
 *
 * **鍵を持ってくる手段が他に無い。** SD へファイルを送り込む口は無いし、
 * カードを抜いて PC で書くのは展示中にはできない。実機で作れれば、
 * 出てきた公開鍵をサーバーの authorized_keys に貼るだけで済む。
 *
 * 作るのは **ECDSA P-256**。mbedTLS バックエンドの libssh2 は
 * ed25519 に対応していない (`LIBSSH2_ED25519 = 0`)。RSA でもよいが、
 * 実機での鍵生成が桁違いに遅い。
 */
#include "cdk_net.h"
#include "cdk_sshkey.h"

#include "esp_check.h"
#include "esp_log.h"

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecp.h"
#include "mbedtls/entropy.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "cdk_sshkey";

/* PEM は 256bit の EC でも 300 バイト強。余裕を見て 1KB。 */
#define PEM_CAP 1024u

static esp_err_t write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "%s を開けません", path);
        return ESP_FAIL;
    }
    const size_t n = fwrite(data, 1, len, f);
    fclose(f);
    if (n != len) {
        ESP_LOGE(TAG, "%s の書き込みが途中で切れました (%u/%u)", path, (unsigned)n,
                 (unsigned)len);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* 鍵から公開鍵の 1 行と指紋を作る。 */
static esp_err_t derive_pub(mbedtls_pk_context *pk, const char *comment, char *line,
                            size_t line_cap, char *fp, size_t fp_cap)
{
    if (mbedtls_pk_get_type(pk) != MBEDTLS_PK_ECKEY) {
        ESP_LOGE(TAG, "EC の鍵ではありません");
        return ESP_ERR_NOT_SUPPORTED;
    }
    mbedtls_ecp_keypair *ec = mbedtls_pk_ec(*pk);
    if (ec->MBEDTLS_PRIVATE(grp).id != MBEDTLS_ECP_DP_SECP256R1) {
        ESP_LOGE(TAG, "P-256 以外の曲線には未対応です");
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t q[CDK_SSHKEY_P256_Q_LEN];
    size_t  qlen = 0;
    if (mbedtls_ecp_point_write_binary(&ec->MBEDTLS_PRIVATE(grp),
                                       &ec->MBEDTLS_PRIVATE(Q),
                                       MBEDTLS_ECP_PF_UNCOMPRESSED, &qlen, q,
                                       sizeof(q)) != 0) {
        return ESP_FAIL;
    }

    if (cdk_sshkey_line_p256(q, qlen, comment, line, line_cap) == 0) {
        return ESP_FAIL;
    }

    /* 指紋は**ブロブ**の SHA-256（行全体でも base64 でもない）。
     * ここを間違えると ssh-keygen -l と一致しない。 */
    uint8_t blob[160];
    const size_t blen = cdk_sshkey_blob_p256(q, qlen, blob, sizeof(blob));
    if (blen == 0) {
        return ESP_FAIL;
    }
    uint8_t digest[32];
    if (mbedtls_sha256(blob, blen, digest, 0) != 0) {
        return ESP_FAIL;
    }
    if (cdk_sshkey_fingerprint(digest, sizeof(digest), fp, fp_cap) == 0) {
        return ESP_FAIL;
    }

    /* **公開鍵はログに出す。** 公開鍵は公開してよいものだし、ここに出れば
     * `cdk.py log` で拾って `ssh-keygen -l -f` と突き合わせられる
     * （画面の小さい字を読むより確実）。
     * **秘密鍵は絶対に出さない。** 一度ログに出ると取り消せない。 */
    ESP_LOGI(TAG, "pubkey %s", line);
    ESP_LOGI(TAG, "fingerprint %s", fp);
    return ESP_OK;
}

esp_err_t cdk_sshkey_generate(const char *path, const char *comment, char *line,
                              size_t line_cap, char *fp, size_t fp_cap)
{
    if (path == NULL || line == NULL || fp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    mbedtls_pk_context       pk;
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context drbg;
    esp_err_t                ret = ESP_FAIL;
    unsigned char           *pem = NULL;

    mbedtls_pk_init(&pk);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);

    static const char SEED[] = "cyberdeck-sshkey";
    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                              (const unsigned char *)SEED, sizeof(SEED) - 1u) != 0) {
        ESP_LOGE(TAG, "乱数の初期化に失敗しました");
        goto done;
    }

    if (mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) {
        goto done;
    }
    ESP_LOGI(TAG, "ECDSA P-256 の鍵を作っています...");
    if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk),
                            mbedtls_ctr_drbg_random, &drbg) != 0) {
        ESP_LOGE(TAG, "鍵生成に失敗しました");
        goto done;
    }

    pem = malloc(PEM_CAP);
    if (pem == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto done;
    }
    memset(pem, 0, PEM_CAP);
    if (mbedtls_pk_write_key_pem(&pk, pem, PEM_CAP) != 0) {
        ESP_LOGE(TAG, "PEM に書き出せません");
        goto done;
    }

    if (derive_pub(&pk, comment, line, line_cap, fp, fp_cap) != ESP_OK) {
        goto done;
    }

    /* 秘密鍵を先に書く。公開鍵だけ残って秘密鍵が無い状態を作らない。 */
    ret = write_file(path, pem, strlen((char *)pem));
    if (ret != ESP_OK) {
        goto done;
    }

    /* `.pub` は認証には使わない（mbedTLS が秘密鍵から導出する）。
     * 人がサーバーへ貼るために置いておく。 */
    char pubpath[128];
    snprintf(pubpath, sizeof(pubpath), "%s.pub", path);
    char withnl[512];
    const int n = snprintf(withnl, sizeof(withnl), "%s\n", line);
    if (n > 0) {
        (void)write_file(pubpath, withnl, (size_t)n);
    }

done:
    if (pem != NULL) {
        /* 秘密鍵をヒープに残さない */
        memset(pem, 0, PEM_CAP);
        free(pem);
    }
    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return ret;
}

esp_err_t cdk_sshkey_info(const char *path, char *line, size_t line_cap, char *fp,
                          size_t fp_cap)
{
    if (path == NULL || line == NULL || fp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    unsigned char *pem = malloc(PEM_CAP);
    if (pem == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    memset(pem, 0, PEM_CAP);
    const size_t n = fread(pem, 1, PEM_CAP - 1u, f);
    fclose(f);
    pem[n] = '\0';

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    esp_err_t ret = ESP_FAIL;

    /* mbedtls_pk_parse_key は長さに終端の分を含める約束。 */
    if (mbedtls_pk_parse_key(&pk, pem, n + 1u, NULL, 0, NULL, NULL) != 0) {
        ESP_LOGW(TAG, "%s を鍵として読めません（PEM か、対応する種類か確認）", path);
        ret = ESP_ERR_INVALID_ARG;
        goto done;
    }
    ret = derive_pub(&pk, NULL, line, line_cap, fp, fp_cap);

    /* **ディスク上の .pub も読んで突き合わせる。**
     * 認証で実際にサーバーへ出すのはこのファイルのほう（mbedTLS の
     * 導出は RSA 専用なので `.pub` を渡している）。秘密鍵と食い違って
     * いると、署名と提示鍵が別物になってサーバーは必ず拒否する。
     * そのとき返るのは rc=-18 で、**「鍵が未登録」と区別がつかない**。 */
    if (ret == ESP_OK) {
        char pubpath[160];
        snprintf(pubpath, sizeof(pubpath), "%s.pub", path);
        FILE *pf = fopen(pubpath, "rb");
        if (pf == NULL) {
            ESP_LOGW(TAG, "%s がありません（ECDSA ではこれが無いと認証できません）",
                     pubpath);
        } else {
            char onfile[512];
            const size_t got = fread(onfile, 1, sizeof(onfile) - 1u, pf);
            fclose(pf);
            onfile[got] = '\0';
            /* 末尾の改行を落としてから比べる */
            size_t e = got;
            while (e > 0 && (onfile[e - 1] == '\n' || onfile[e - 1] == '\r')) {
                onfile[--e] = '\0';
            }
            ESP_LOGI(TAG, "onfile %s", onfile);
            /* 行頭が一致すればよい（コメントは違ってよい） */
            const char *sp = strrchr(line, ' ');
            const size_t cmp_len = (sp != NULL) ? (size_t)(sp - line) : strlen(line);
            if (strncmp(onfile, line, cmp_len) != 0) {
                ESP_LOGE(TAG,
                         "**%s の中身が秘密鍵と一致しません。**"
                         "`ssh keygen -f` で作り直してください", pubpath);
            }
        }
    }

done:
    memset(pem, 0, PEM_CAP);
    free(pem);
    mbedtls_pk_free(&pk);
    return ret;
}
