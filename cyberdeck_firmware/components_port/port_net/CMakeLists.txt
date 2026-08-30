idf_component_register(SRCS "port_net.c" "port_ssh.c" "port_knownhosts.c" "port_sshkey.c" "port_sock.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_wifi esp_netif esp_event lwip libssh2 mbedtls
                                cdk_sshkey port_cfg port_storage)

# ホスト鍵の「不一致なら断る」枝は、外から普通に叩けない。
# 通したいときだけ細工を有効にする（DESIGN.md §8.2）。
#   idf.py build -DCDK_FAULT_KNOWNHOSTS=1
if(CDK_FAULT_KNOWNHOSTS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CDK_KNOWNHOSTS_FAULT_INJECT=1)
    message(WARNING "CDK_FAULT_KNOWNHOSTS: ホスト鍵を故意に壊すビルドです")
endif()
