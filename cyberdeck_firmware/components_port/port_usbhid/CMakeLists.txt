idf_component_register(SRCS "port_usbhid.c"
                       INCLUDE_DIRS "include"
                       REQUIRES usb driver port_board cdk_keys cdk_hid)
