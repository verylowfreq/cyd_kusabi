# components/ の純 C コアに ESP-IDF 依存が混ざっていないことを検査する。
#
# ここが崩れるとホスト側でのテストができなくなり、
# 「パソコン上で検証できるところはパソコン上で検証する」という前提が壊れる。
#
#   cmake -DCORE_DIR=<path> -P check_no_idf.cmake

if(NOT DEFINED CORE_DIR)
    message(FATAL_ERROR "CORE_DIR が指定されていません")
endif()

file(GLOB_RECURSE core_files ${CORE_DIR}/*.c ${CORE_DIR}/*.h)

set(banned_includes
    "freertos/"
    "esp_log.h"
    "esp_err.h"
    "esp_system.h"
    "esp_heap_caps.h"
    "esp_timer.h"
    "esp_lcd"
    "driver/"
    "nvs_flash.h"
    "sdkconfig.h"
)

# 実際の呼び出し/宣言だけを見る。コメントで名前に言及しただけで
# 落ちないよう、後続に '(' か識別子が続くことを要求する。
set(banned_calls
    "ESP_LOGI"
    "ESP_LOGW"
    "ESP_LOGE"
    "ESP_LOGD"
    "ESP_LOGV"
    "ESP_ERROR_CHECK"
    "ESP_RETURN_ON_ERROR"
    "xTaskCreate"
    "vTaskDelay"
    "heap_caps_malloc"
    "heap_caps_calloc"
)
set(banned_types
    "esp_err_t"
    "TaskHandle_t"
    "SemaphoreHandle_t"
)

set(violations "")

foreach(f ${core_files})
    file(READ ${f} contents)
    file(RELATIVE_PATH rel ${CORE_DIR} ${f})

    foreach(inc ${banned_includes})
        if(contents MATCHES "#[ \t]*include[ \t]*[<\"][^>\"]*${inc}")
            list(APPEND violations "${rel}: ESP-IDF ヘッダ '${inc}' を include している")
        endif()
    endforeach()

    foreach(sym ${banned_calls})
        if(contents MATCHES "${sym}[ \t]*\\(")
            list(APPEND violations "${rel}: ESP-IDF 由来の '${sym}(' を呼んでいる")
        endif()
    endforeach()

    foreach(sym ${banned_types})
        if(contents MATCHES "${sym}[ \t]+[A-Za-z_*]")
            list(APPEND violations "${rel}: ESP-IDF 由来の型 '${sym}' を使っている")
        endif()
    endforeach()
endforeach()

list(LENGTH violations n)
if(n GREATER 0)
    foreach(v ${violations})
        message(SEND_ERROR "  ${v}")
    endforeach()
    message(FATAL_ERROR
        "components/ は ESP-IDF に依存してはいけない。"
        "ESP-IDF を使う処理は components_port/ に置くこと。")
endif()

message(STATUS "components/ に ESP-IDF 依存なし (${n} violations)")
