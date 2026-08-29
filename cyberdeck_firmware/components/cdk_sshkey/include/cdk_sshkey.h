/* cdk_sshkey — OpenSSH 形式の公開鍵行を組み立てる
 *
 * ESP-IDF に依存しない純 C。鍵そのものを作るのは port 層（mbedTLS）で、
 * ここは**並べ方と符号化だけ**を持つ。
 *
 *   ecdsa-sha2-nistp256 AAAAE2VjZHNh... comment
 *
 * base64 の中身は SSH の「文字列」を並べたもの。文字列は 4 バイトの
 * ビッグエンディアン長 + 中身:
 *
 *   string  "ecdsa-sha2-nistp256"
 *   string  "nistp256"
 *   string  Q      65 バイト (0x04 || X(32) || Y(32))
 *
 * **mbedTLS バックエンドの libssh2 は ed25519 に対応していない**
 * (`LIBSSH2_ED25519 = 0`)。`ssh-keygen` の既定が ed25519 なので、
 * 何も言わないと必ず踏む。使えるのは RSA と ECDSA だけ。
 */
#ifndef CDK_SSHKEY_H
#define CDK_SSHKEY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* P-256 の非圧縮点。0x04 || X(32) || Y(32) */
#define CDK_SSHKEY_P256_Q_LEN 65u

/* ecdsa-sha2-nistp256 の公開鍵ブロブを組む。書いたバイト数を返す。
 * 足りなければ 0。 */
size_t cdk_sshkey_blob_p256(const uint8_t *q, size_t q_len, uint8_t *out, size_t cap);

/* base64（改行なし、= で詰める）。書いたバイト数を返す（終端を除く）。 */
size_t cdk_b64_encode(const uint8_t *in, size_t n, char *out, size_t cap);

/* authorized_keys に貼れる 1 行を組む。comment は NULL 可。
 * 末尾に改行は付けない。 */
size_t cdk_sshkey_line_p256(const uint8_t *q, size_t q_len, const char *comment,
                            char *out, size_t cap);

/* `SHA256:...` 形式の指紋。digest は SHA-256 の 32 バイト。
 * OpenSSH と同じく **base64 の詰め文字 `=` は落とす**。 */
size_t cdk_sshkey_fingerprint(const uint8_t *digest, size_t digest_len, char *out,
                              size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* CDK_SSHKEY_H */
