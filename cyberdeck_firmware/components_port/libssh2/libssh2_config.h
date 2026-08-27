/* libssh2 の ESP-IDF 向け設定。
 *
 * libssh2_setup.h が HAVE_CONFIG_H のときにこれを include する。
 * 上流の autotools / CMake が吐くものを手書きで置き換えたもの。
 *
 * 暗号バックエンドは mbedTLS。ESP-IDF が同梱していて、
 * ESP32-S3 のハードウェアアクセラレータも効く。
 */
#ifndef LIBSSH2_CONFIG_H
#define LIBSSH2_CONFIG_H

/* ---- 暗号バックエンド ---- */
#define LIBSSH2_MBEDTLS

/* ---- newlib / lwIP にあるもの ---- */
#define HAVE_UNISTD_H
#define HAVE_INTTYPES_H
#define HAVE_STDLIB_H
#define HAVE_SYS_TIME_H
#define HAVE_SYS_SELECT_H
#define HAVE_SYS_SOCKET_H
#define HAVE_ARPA_INET_H
#define HAVE_NETINET_IN_H
#define HAVE_GETTIMEOFDAY
#define HAVE_SELECT
#define HAVE_SNPRINTF
#define HAVE_STRTOLL
#define HAVE_LONGLONG
/* lwIP / newlib に poll.h は無い。select を使わせる。 */
/* #define HAVE_POLL */

/* lwIP のソケットは POSIX 互換。close(2) で閉じられる。 */
#define HAVE_UNISTD_H

/* ---- 使わない機能を落としてフラッシュと RAM を節約する ----
 * この端末が要るのは「対話シェル + PTY」だけ。 */
#define LIBSSH2_NO_DSA          /* DSA 鍵は現代の sshd で無効 */
#define LIBSSH2_NO_MD5          /* 弱いハッシュ */
#define LIBSSH2_NO_HMAC_RIPEMD
#define LIBSSH2_NO_BLOWFISH
#define LIBSSH2_NO_RC4
#define LIBSSH2_NO_CAST
#define LIBSSH2_NO_3DES

/* 圧縮は使わない。zlib を引き込まない。 */
#undef LIBSSH2_HAVE_ZLIB

/* ESP-IDF には <sys/param.h> があるが、libssh2 は使わない */

#endif /* LIBSSH2_CONFIG_H */
