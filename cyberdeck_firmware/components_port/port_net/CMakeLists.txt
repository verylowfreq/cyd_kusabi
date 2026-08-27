idf_component_register(SRCS "port_net.c" "port_ssh.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_wifi esp_netif esp_event lwip libssh2 port_cfg)
