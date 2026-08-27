idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES cdk_canvas cdk_font cdk_keys cdk_proto cdk_screen
                                cdk_term port_display port_link port_net
                       EMBED_FILES
                           "${CMAKE_CURRENT_LIST_DIR}/../components/cdk_font/shinonome12.cdkfont"
                           "${CMAKE_CURRENT_LIST_DIR}/../host/fixtures/vt/banner.vt")
