idf_component_register(SRCS "main.c" "cdk_commands.c" "cdk_fscmds.c" "cdk_skkcmds.c" "cdk_editcmds.c"
                       INCLUDE_DIRS "."
                       REQUIRES cdk_canvas cdk_edit cdk_font cdk_keys cdk_proto cdk_screen
                                cdk_shell cdk_skk cdk_term esp_app_format esp_timer
                                port_cfg port_display port_link port_net port_periph
                                port_skk port_spibus port_storage
                       EMBED_FILES
                           "${CMAKE_CURRENT_LIST_DIR}/../components/cdk_font/shinonome12.cdkfont"
                           "${CMAKE_CURRENT_LIST_DIR}/../host/fixtures/vt/banner.vt")
