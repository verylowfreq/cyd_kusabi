/* known_hosts によるホスト鍵の検証。
 *
 * M2 では指紋をログに出すだけだった。SD が使えるようになったので
 * `/sdcard/ssh/known_hosts` に覚えて突き合わせる。
 *
 * 方針は TOFU（初回は覚える）。ただし
 *
 *   - 初回に覚えるときは指紋を画面に出す
 *   - **記録と食い違ったら必ず接続を断る**
 *
 * 食い違いを警告だけにして繋いでしまうと、中間者攻撃を検出する意味がなくなる。
 * SD が無い / 書けない場合は「覚えられないが繋ぐ」に落とす。
 * 端末として使えないより、検証が弱いほうがましという判断（DESIGN.md §8.2）。
 */
#include "cdk_knownhosts.h"

#include "cdk_storage.h"
#include "esp_log.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "cdk_knownhosts";

#define KH_DIR  CDK_SD_MOUNT_POINT "/ssh"
#define KH_PATH KH_DIR "/known_hosts"

const char *cdk_knownhosts_path(void)
{
    return KH_PATH;
}

int cdk_knownhosts_forget(void)
{
    if (!cdk_sd_mounted()) {
        return CDK_KNOWNHOSTS_FORGET_NO_SD;
    }
    cdk_sd_lock();
    errno = 0;
    const int rc = unlink(KH_PATH);
    const int err = errno;
    cdk_sd_unlock();

    if (rc != 0) {
        /* 「もう無い」と「消せない」を分ける。同じ扱いにすると、
         * 2 回目の forget が失敗したように見えて驚く。 */
        return (err == ENOENT) ? CDK_KNOWNHOSTS_FORGET_ABSENT
                               : CDK_KNOWNHOSTS_FORGET_ERROR;
    }
    ESP_LOGW(TAG, "%s を消しました。次の接続は TOFU になります", KH_PATH);
    return CDK_KNOWNHOSTS_FORGET_OK;
}

cdk_hostkey_result_t cdk_knownhosts_check(LIBSSH2_SESSION *session, const char *host,
                                          int port)
{
    if (!cdk_sd_mounted()) {
        ESP_LOGW(TAG, "SD が無いのでホスト鍵を覚えられません");
        return CDK_HOSTKEY_UNVERIFIED;
    }

    size_t   keylen = 0;
    int      keytype = 0;
    const char *key = libssh2_session_hostkey(session, &keylen, &keytype);
    if (key == NULL) {
        return CDK_HOSTKEY_ERROR;
    }

    /* libssh2 の鍵種別を known_hosts のビットに直す。
     * ここを取り違えると、同じ鍵を毎回「初めて見た」と判定してしまう。 */
    int type_bits = 0;
    switch (keytype) {
    case LIBSSH2_HOSTKEY_TYPE_RSA:
        type_bits = LIBSSH2_KNOWNHOST_KEY_SSHRSA;
        break;
    /* 曲線ごとに別のビット。まとめて ECDSA_256 にすると、
     * nistp384 の鍵を ecdsa-sha2-nistp256 という嘘の名前で書き出してしまう。 */
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:
        type_bits = LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
        break;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:
        type_bits = LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
        break;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:
        type_bits = LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
        break;
    case LIBSSH2_HOSTKEY_TYPE_ED25519:
        type_bits = LIBSSH2_KNOWNHOST_KEY_ED25519;
        break;
    default:
        ESP_LOGW(TAG, "知らない鍵種別 %d。検証できません", keytype);
        return CDK_HOSTKEY_UNVERIFIED;
    }

    /* 照合に使う鍵。既定では本物そのもの。 */
    const char *check_key = key;

#ifdef CDK_KNOWNHOSTS_FAULT_INJECT
    /* 不一致の枝を実機で通すための細工。既定では無効。
     * ここは「記録と違ったら必ず断る」という約束そのものなので、
     * 一度も実行しないまま正しいと信じるわけにはいかない。
     *   idf.py build -DCDK_FAULT_KNOWNHOSTS=1
     *
     * 壊すのは**照合に使う鍵だけ**で、記録には本物を書く。
     * 両方壊すと、記録も同じように壊れて一致してしまい、
     * 「細工したのに繋がる」という紛らわしい結果になる。
     * 照合だけ壊せば、1 回目は記録して繋がり、2 回目以降は必ず断る。 */
    static char injected[1024];
    if (keylen > 0 && keylen <= sizeof(injected)) {
        memcpy(injected, key, keylen);
        injected[keylen - 1] = (char)(injected[keylen - 1] ^ 0xFF);
        check_key = injected;
        ESP_LOGW(TAG, "細工: 照合用のホスト鍵の末尾を反転しました");
    }
#endif

    LIBSSH2_KNOWNHOSTS *kh = libssh2_knownhost_init(session);
    if (kh == NULL) {
        return CDK_HOSTKEY_ERROR;
    }

    cdk_sd_lock();
    mkdir(KH_DIR, 0777); /* 既にあってもよい */
    const int nread = libssh2_knownhost_readfile(kh, KH_PATH,
                                                 LIBSSH2_KNOWNHOST_FILE_OPENSSH);
    cdk_sd_unlock();
    if (nread < 0) {
        ESP_LOGI(TAG, "%s はまだありません (rc=%d)", KH_PATH, nread);
    }

    struct libssh2_knownhost *found = NULL;
    const int check = libssh2_knownhost_checkp(
        kh, host, port, check_key, keylen,
        LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | type_bits,
        &found);

    cdk_hostkey_result_t result;
    switch (check) {
    case LIBSSH2_KNOWNHOST_CHECK_MATCH:
        result = CDK_HOSTKEY_KNOWN;
        break;

    case LIBSSH2_KNOWNHOST_CHECK_MISMATCH:
        /* 断る。ここを警告だけにすると検証している意味がない。 */
        ESP_LOGE(TAG, "ホスト鍵が記録と違います。接続を中止します");
        result = CDK_HOSTKEY_MISMATCH;
        break;

    case LIBSSH2_KNOWNHOST_CHECK_NOTFOUND: {
        const int rc = libssh2_knownhost_addc(
            kh, host, NULL, key, keylen, NULL, 0,
            LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | type_bits,
            NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "覚えられません rc=%d", rc);
            result = CDK_HOSTKEY_UNVERIFIED;
            break;
        }
        cdk_sd_lock();
        const int wr = libssh2_knownhost_writefile(kh, KH_PATH,
                                                   LIBSSH2_KNOWNHOST_FILE_OPENSSH);
        cdk_sd_unlock();
        if (wr != 0) {
            ESP_LOGW(TAG, "%s に書けません rc=%d", KH_PATH, wr);
            result = CDK_HOSTKEY_UNVERIFIED;
        } else {
            ESP_LOGI(TAG, "%s を %s に覚えました", host, KH_PATH);
            result = CDK_HOSTKEY_ADDED;
        }
        break;
    }

    default:
        ESP_LOGW(TAG, "known_hosts の検査に失敗 rc=%d", check);
        result = CDK_HOSTKEY_ERROR;
        break;
    }

    libssh2_knownhost_free(kh);
    return result;
}
