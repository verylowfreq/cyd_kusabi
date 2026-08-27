# components/ の各コンポーネントについて、ディレクトリにある .c が
# すべて CMakeLists.txt の SRCS に並んでいるかを検査する。
#
# ホスト側は file(GLOB) で拾うので、足し忘れてもテストは緑のまま通る。
# 実機のリンク時に初めて undefined reference で気づくことになるため、
# ここで先に止める。
#
#   cmake -DCORE_DIR=<path> -P check_sources_listed.cmake

if(NOT DEFINED CORE_DIR)
    message(FATAL_ERROR "CORE_DIR が指定されていません")
endif()

file(GLOB component_dirs ${CORE_DIR}/*)
set(problems "")

foreach(dir ${component_dirs})
    if(NOT IS_DIRECTORY ${dir})
        continue()
    endif()
    if(NOT EXISTS ${dir}/CMakeLists.txt)
        continue()
    endif()

    get_filename_component(name ${dir} NAME)
    file(READ ${dir}/CMakeLists.txt cmakelists)
    file(GLOB srcs ${dir}/*.c)

    foreach(src ${srcs})
        get_filename_component(base ${src} NAME)
        if(NOT cmakelists MATCHES "${base}")
            list(APPEND problems "${name}/${base} が ${name}/CMakeLists.txt の SRCS にありません")
        endif()
    endforeach()
endforeach()

list(LENGTH problems n)
if(n GREATER 0)
    foreach(p ${problems})
        message(SEND_ERROR "  ${p}")
    endforeach()
    message(FATAL_ERROR
        "ESP-IDF 側のビルドから漏れています。"
        "ホスト側は GLOB なので気づけず、実機のリンクで初めて落ちます。")
endif()

message(STATUS "全 .c が CMakeLists に並んでいます")
