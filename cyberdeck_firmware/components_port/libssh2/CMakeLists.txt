# libssh2 1.11.1 を ESP-IDF コンポーネントとして包む。
# 上流のビルドシステムは使わず、src/Makefile.inc の CSOURCES をそのまま並べる。
# 設定は libssh2_config.h（HAVE_CONFIG_H 経由で libssh2_setup.h が読む）。

set(LIBSSH2_SRCS
    agent.c
    bcrypt_pbkdf.c
    channel.c
    comp.c
    chacha.c
    cipher-chachapoly.c
    crypt.c
    crypto.c
    global.c
    hostkey.c
    keepalive.c
    kex.c
    knownhost.c
    mac.c
    misc.c
    packet.c
    pem.c
    poly1305.c
    publickey.c
    scp.c
    session.c
    sftp.c
    transport.c
    userauth.c
    userauth_kbd_packet.c
    version.c
)
list(TRANSFORM LIBSSH2_SRCS PREPEND "upstream/src/")

idf_component_register(SRCS ${LIBSSH2_SRCS}
                       INCLUDE_DIRS "upstream/include"
                       PRIV_INCLUDE_DIRS "." "upstream/src"
                       REQUIRES mbedtls lwip)

target_compile_definitions(${COMPONENT_LIB} PRIVATE HAVE_CONFIG_H)

# 上流のコードは自分たちの警告設定でビルドされている。
# こちらの -Werror を持ち込まない。
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-type-limits
)
