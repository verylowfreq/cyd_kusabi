# ホスト側ビルド。components/ の純 C コアをそのまま取り込んで単体テストする。
#
#   cmake -S host -B build-host -G Ninja
#   cmake --build build-host
#   ctest --test-dir build-host --output-on-failure
#
cmake_minimum_required(VERSION 3.16)
project(cdk_host C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
enable_testing()

# Windows の CRT が fopen 等を deprecated 扱いにしてくるのを黙らせる。
add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

set(CDK_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../components)

# components/ に足すたびにここにも足す。順序は依存関係に合わせる。
set(CDK_CORE_MODULES
    cdk_proto
    cdk_text
    cdk_canvas
    cdk_font
    cdk_term
    cdk_keys
    cdk_shell
    cdk_screen
)

set(CDK_CORE_SRCS "")
set(CDK_CORE_INCS "")
foreach(mod ${CDK_CORE_MODULES})
    # CONFIGURE_DEPENDS が無いと、既存モジュールに .c を足したとき
    # ビルドが黙って古いままになる（M1 で cdk_term に複数ファイルを置く）。
    file(GLOB mod_srcs CONFIGURE_DEPENDS ${CDK_CORE_DIR}/${mod}/*.c)
    list(APPEND CDK_CORE_SRCS ${mod_srcs})
    list(APPEND CDK_CORE_INCS ${CDK_CORE_DIR}/${mod}/include)
endforeach()

add_library(cdk_core STATIC ${CDK_CORE_SRCS})
target_include_directories(cdk_core PUBLIC ${CDK_CORE_INCS})

if(MSVC)
    target_compile_options(cdk_core PRIVATE /W4 /WX /wd4996)
else()
    target_compile_options(cdk_core PRIVATE -Wall -Wextra -Werror -Wshadow)
endif()

# --- テスト ---
# フォントブロブはホストではファイルから読む。実機ではフラッシュに焼く。
set(CDK_FONT_BLOB ${CDK_CORE_DIR}/cdk_font/shinonome12.cdkfont)
if(NOT EXISTS ${CDK_FONT_BLOB})
    message(FATAL_ERROR
        "${CDK_FONT_BLOB} がありません。先に生成してください:
"
        "  python tools/mkfont.py <shinonome-0.9.11/bdf> -o ${CDK_FONT_BLOB}")
endif()

add_library(cdk_testmain INTERFACE)
target_include_directories(cdk_testmain INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_compile_definitions(cdk_testmain INTERFACE CDK_FONT_BLOB_PATH="${CDK_FONT_BLOB}")

set(CDK_TESTS
    test_proto
    test_text
    test_canvas
    test_font
    test_term
    test_keys
    test_keys_decode
    test_shell
    test_screen
)

foreach(t ${CDK_TESTS})
    add_executable(${t} tests/${t}.c)
    target_link_libraries(${t} PRIVATE cdk_core cdk_testmain)
    if(MSVC)
        target_compile_options(${t} PRIVATE /W4 /WX /wd4996)
    else()
        target_compile_options(${t} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${t} COMMAND ${t})
endforeach()

# components/ に ESP-IDF 依存が混ざっていないことを機械的に保証する。
# ここが崩れるとホストでのテストができなくなるので、テストとして常時回す。
# ESP-IDF 側の CMakeLists に .c を足し忘れると、ホストは GLOB なので緑のまま
# 実機のリンクだけが落ちる。先に止める。
add_test(NAME sources_listed
         COMMAND ${CMAKE_COMMAND}
                 -DCORE_DIR=${CDK_CORE_DIR}
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_sources_listed.cmake)

add_test(NAME no_idf_in_core
         COMMAND ${CMAKE_COMMAND}
                 -DCORE_DIR=${CDK_CORE_DIR}
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_no_idf.cmake)

# --- 参照レンダラ ---
# 実機のスクリーンショットと突き合わせるための基準画像を吐く。
# `tools/cdk.py shot --verify` がこれを呼ぶ。
add_executable(cdk_render tools/cdk_render.c)
target_link_libraries(cdk_render PRIVATE cdk_core)
target_compile_definitions(cdk_render PRIVATE CDK_FONT_BLOB_PATH="${CDK_FONT_BLOB}")
