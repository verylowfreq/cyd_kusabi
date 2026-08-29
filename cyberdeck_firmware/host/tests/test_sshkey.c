#include "cdk_sshkey.h"
#include "t.h"

/* ---- ssh-keygen が作った本物の鍵を基準にする -----------------------
 *
 * 自分で組んだ期待値と自分の実装を比べても、同じ間違いをしていれば通る
 * （#7 で踏んだ形）。**別実装の出力を期待値にする。**
 *
 *   ssh-keygen -t ecdsa -b 256 -N "" -C cyberdeck-test -f tk
 *
 * 秘密鍵は捨ててあり、これは公開鍵の中身だけ。 */
static const uint8_t Q[65] = {
    0x04, 0xcd, 0x2c, 0xeb, 0x98, 0x84, 0x98, 0x87, 0xa5, 0x42, 0xad, 0xe1, 0x74,
    0x08, 0xcf, 0xc1, 0x02, 0xdf, 0x9a, 0x9b, 0x71, 0xfe, 0xe0, 0x3f, 0x10, 0xda,
    0x73, 0xa1, 0x10, 0x3e, 0xe9, 0x29, 0x2d, 0xbf, 0xf4, 0x8a, 0x59, 0x7f, 0x7a,
    0xf8, 0x42, 0x14, 0x35, 0xc3, 0x31, 0x16, 0xa3, 0x93, 0xf6, 0xbe, 0xdb, 0x1f,
    0x24, 0xa0, 0xcd, 0x09, 0x46, 0x0d, 0x9c, 0x13, 0x8a, 0xe6, 0xaa, 0xac, 0x89,
};

static const char WANT[] =
    "ecdsa-sha2-nistp256 "
    "AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBM0s65iEmIelQq3hdAjPwQ"
    "Lfmptx/uA/ENpzoRA+6Sktv/SKWX96+EIUNcMxFqOT9r7bHySgzQlGDZwTiuaqrIk= "
    "cyberdeck-test";

static char BUF[512];

int main(void)
{
    T_CASE("base64");
    {
        /* RFC 4648 のベクタ */
        T_CHECK(cdk_b64_encode((const uint8_t *)"", 0, BUF, sizeof(BUF)) == 0);
        T_EQ_INT((int)strlen(BUF), 0);
        cdk_b64_encode((const uint8_t *)"f", 1, BUF, sizeof(BUF));
        T_CHECK(strcmp(BUF, "Zg==") == 0);
        cdk_b64_encode((const uint8_t *)"fo", 2, BUF, sizeof(BUF));
        T_CHECK(strcmp(BUF, "Zm8=") == 0);
        cdk_b64_encode((const uint8_t *)"foo", 3, BUF, sizeof(BUF));
        T_CHECK(strcmp(BUF, "Zm9v") == 0);
        cdk_b64_encode((const uint8_t *)"foobar", 6, BUF, sizeof(BUF));
        T_CHECK(strcmp(BUF, "Zm9vYmFy") == 0);

        /* 入りきらないときは書かない（途中まで書いて返さない） */
        char tiny[4];
        T_EQ_INT((int)cdk_b64_encode((const uint8_t *)"foobar", 6, tiny, sizeof(tiny)),
                 0);
    }

    T_CASE("公開鍵の行が ssh-keygen と一致する");
    {
        /* **これが本番。** 別実装が出した 1 行と字面で一致すること。 */
        const size_t n = cdk_sshkey_line_p256(Q, sizeof(Q), "cyberdeck-test", BUF,
                                              sizeof(BUF));
        T_CHECK(n > 0);
        T_CHECK(strcmp(BUF, WANT) == 0);
        T_EQ_INT((int)n, (int)strlen(WANT));
    }

    T_CASE("コメント無し");
    {
        cdk_sshkey_line_p256(Q, sizeof(Q), NULL, BUF, sizeof(BUF));
        /* 型と base64 まではまったく同じ */
        T_CHECK(strncmp(BUF, WANT, strlen(BUF)) == 0);
        T_CHECK(strchr(BUF, ' ') != NULL);
        T_CHECK(strstr(BUF, "cyberdeck-test") == NULL);
    }

    T_CASE("ブロブの構造");
    {
        uint8_t blob[160];
        const size_t n = cdk_sshkey_blob_p256(Q, sizeof(Q), blob, sizeof(blob));
        /* 4+19 + 4+8 + 4+65 = 104 */
        T_EQ_INT((int)n, 104);
        /* 先頭は長さ 19 の "ecdsa-sha2-nistp256" */
        T_EQ_INT(blob[0], 0);
        T_EQ_INT(blob[1], 0);
        T_EQ_INT(blob[2], 0);
        T_EQ_INT(blob[3], 19);
        T_CHECK(memcmp(blob + 4, "ecdsa-sha2-nistp256", 19) == 0);
        T_CHECK(memcmp(blob + 4 + 19 + 4, "nistp256", 8) == 0);
    }

    T_CASE("圧縮点は受け付けない");
    {
        /* 0x02 / 0x03 は圧縮形式。黙って通すと、サーバー側で
         * 「鍵は合っているのに弾かれる」ことになる。 */
        uint8_t bad[65];
        memcpy(bad, Q, sizeof(bad));
        bad[0] = 0x02;
        T_EQ_INT((int)cdk_sshkey_blob_p256(bad, sizeof(bad), (uint8_t *)BUF,
                                           sizeof(BUF)),
                 0);
        T_EQ_INT((int)cdk_sshkey_line_p256(bad, sizeof(bad), NULL, BUF, sizeof(BUF)),
                 0);
    }

    T_CASE("長さが違う点は受け付けない");
    {
        T_EQ_INT((int)cdk_sshkey_blob_p256(Q, 33, (uint8_t *)BUF, sizeof(BUF)), 0);
        T_EQ_INT((int)cdk_sshkey_blob_p256(Q, 64, (uint8_t *)BUF, sizeof(BUF)), 0);
        T_EQ_INT((int)cdk_sshkey_blob_p256(NULL, 65, (uint8_t *)BUF, sizeof(BUF)), 0);
    }

    T_CASE("指紋が ssh-keygen -l と一致する");
    {
        /* 上と同じ鍵の SHA-256。ssh-keygen -l の出力:
         *   256 SHA256:LvzM1hm8l82sQHqwdV2FbffGrWFjSexF+5xTa0SLlk8 ... (ECDSA)
         * digest はホスト側で別に計算したもの（実機では mbedTLS が出す）。 */
        static const uint8_t D[32] = {
            0x2e, 0xfc, 0xcc, 0xd6, 0x19, 0xbc, 0x97, 0xcd, 0xac, 0x40, 0x7a,
            0xb0, 0x75, 0x5d, 0x85, 0x6d, 0xf7, 0xc6, 0xad, 0x61, 0x63, 0x49,
            0xec, 0x45, 0xfb, 0x9c, 0x53, 0x6b, 0x44, 0x8b, 0x96, 0x4f,
        };
        const size_t n = cdk_sshkey_fingerprint(D, sizeof(D), BUF, sizeof(BUF));
        T_CHECK(n > 0);
        T_CHECK(strcmp(BUF, "SHA256:LvzM1hm8l82sQHqwdV2FbffGrWFjSexF+5xTa0SLlk8") == 0);
        /* 詰め文字は落とす */
        T_CHECK(strchr(BUF, '=') == NULL);
    }

    T_CASE("指紋の引数チェック");
    {
        static const uint8_t D[32] = {0};
        T_EQ_INT((int)cdk_sshkey_fingerprint(D, 31, BUF, sizeof(BUF)), 0);
        T_EQ_INT((int)cdk_sshkey_fingerprint(NULL, 32, BUF, sizeof(BUF)), 0);
        char tiny[4];
        T_EQ_INT((int)cdk_sshkey_fingerprint(D, 32, tiny, sizeof(tiny)), 0);
    }

    return t_report("test_sshkey");
}
