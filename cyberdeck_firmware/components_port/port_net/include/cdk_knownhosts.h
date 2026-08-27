/* known_hosts によるホスト鍵の検証。実装は port_knownhosts.c。 */
#pragma once

#include "libssh2.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CDK_HOSTKEY_KNOWN = 0,   /* 記録と一致 */
    CDK_HOSTKEY_ADDED,       /* 初めて見たので覚えた (TOFU) */
    CDK_HOSTKEY_MISMATCH,    /* 記録と違う。接続してはいけない */
    CDK_HOSTKEY_UNVERIFIED,  /* SD が無い等で検証できなかった */
    CDK_HOSTKEY_ERROR
} cdk_hostkey_result_t;

cdk_hostkey_result_t cdk_knownhosts_check(LIBSSH2_SESSION *session, const char *host,
                                          int port);

const char *cdk_knownhosts_path(void);

/* 記録を消す。鍵を入れ替えたサーバーに繋ぎ直すときに要る。
 * これが無いと、正当な再鍵でも「不一致 → 接続不能」で詰む。 */
enum {
    CDK_KNOWNHOSTS_FORGET_OK     =  0,
    CDK_KNOWNHOSTS_FORGET_ABSENT =  1, /* もともと無い。失敗ではない */
    CDK_KNOWNHOSTS_FORGET_NO_SD  = -1,
    CDK_KNOWNHOSTS_FORGET_ERROR  = -2,
};

int cdk_knownhosts_forget(void);

#ifdef __cplusplus
}
#endif
