idf_component_register(SRCS "port_display.c"
                       INCLUDE_DIRS "include"
                       REQUIRES cdk_canvas esp_lcd driver port_board port_spibus)
