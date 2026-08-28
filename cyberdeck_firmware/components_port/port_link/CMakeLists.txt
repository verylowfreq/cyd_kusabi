idf_component_register(SRCS "port_link.c"
                       INCLUDE_DIRS "include"
                       REQUIRES cdk_proto esp_driver_usb_serial_jtag esp_driver_uart
                                port_board)
