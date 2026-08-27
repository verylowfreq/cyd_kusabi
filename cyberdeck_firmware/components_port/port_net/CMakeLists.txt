idf_component_register(SRCS "port_net.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_wifi esp_netif esp_event nvs_flash libssh2)
