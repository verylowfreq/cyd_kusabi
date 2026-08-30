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
    cdk_hid
    cdk_status
    cdk_sshkey
    cdk_retry
    cdk_shell
    cdk_screen
    cdk_skk
    cdk_edit
    cdk_wasm
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
set(CDK_SKK_DICT ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/skk/mini.skkd)
target_compile_definitions(cdk_testmain INTERFACE
    CDK_FONT_BLOB_PATH="${CDK_FONT_BLOB}"
    CDK_SKK_DICT_PATH="${CDK_SKK_DICT}")

set(CDK_TESTS
    test_proto
    test_text
    test_canvas
    test_font
    test_term
    test_keys
    test_keys_decode
    test_hid
    test_status
    test_sshkey
    test_retry
    test_shell
    test_screen
    test_skk_romkana
    test_skk_dict
    test_skk
    test_skk_user
    test_edit
    test_wasm_abi
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

# ゲストの線形メモリの境界検査が抜けていないか。
# 人間のレビューに任せる種類の間違いではないので、テストとして常時回す。
add_test(NAME wasm_natives_checked
         COMMAND ${CMAKE_COMMAND}
                 -DNATIVES=${CMAKE_CURRENT_SOURCE_DIR}/../components_port/port_wasm/port_wasm_natives.c
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_wasm_natives.cmake)

add_test(NAME no_idf_in_core
         COMMAND ${CMAKE_COMMAND}
                 -DCORE_DIR=${CDK_CORE_DIR}
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_no_idf.cmake)

# 辞書の形式はビルダとリーダの両方に書いてある。片方だけ直すと、
# コミット済みの fixture では通り続けて実物の辞書だけが壊れる。
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    # ピン番号は HARDWARE_DESIGN.md と cdk_board.h の 2 か所にある。
    # 片方だけ直すと、基板は正しいのにファームが別のピンを叩く。
    add_test(NAME board_pins
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/../tools/check_board_pins.py)

    # 起動バナーが桁数に収まっているか。#7（枠線の桁ずれ）は人間が画面を
    # 見て気づいたもので、スクリーンショットと基準画像は一致していた。
    # 同じコードが同じ間違いを描く以上、数えるならここで数えるしかない。
    add_test(NAME banner_width
             COMMAND ${Python3_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/../tools/check_banner_width.py)

    add_test(NAME skk_fixture_reproducible
             COMMAND ${CMAKE_COMMAND}
                     -DPY=${Python3_EXECUTABLE}
                     -DJISYO=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/skk/mini.jisyo
                     -DBIN=${CDK_SKK_DICT}
                     -DBUILDER=${CMAKE_CURRENT_SOURCE_DIR}/../tools/skk_build.py
                     -DTMP=${CMAKE_CURRENT_BINARY_DIR}/mini_regen.skkd
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_skk_fixture.cmake)
else()
    message(STATUS "Python が見つからないので skk_fixture_reproducible は作りません")
endif()

# --- 参照レンダラ ---
# 実機のスクリーンショットと突き合わせるための基準画像を吐く。
# `tools/cdk.py shot --verify` がこれを呼ぶ。
add_executable(cdk_render tools/cdk_render.c)
target_link_libraries(cdk_render PRIVATE cdk_core)
target_compile_definitions(cdk_render PRIVATE CDK_FONT_BLOB_PATH="${CDK_FONT_BLOB}")

# --- 辞書の探索ツール ---
# 実物の SKK-JISYO.L を実機と同じコードで引く。fixture の 10 件では
# 索引の並べ直しの誤りが表に出ないので、17 万件で別実装と突き合わせる。
add_executable(skk_probe tools/skk_probe.c)
target_link_libraries(skk_probe PRIVATE cdk_core)

# --- wasm3 とユーザーアプリの実行 ---
#
# 上流 wasm3 は純 C99 なので、実機と同じソースをホストでも組める。
# **components_port/ をホストビルドが取り込むのはここだけ。** 例外にする理由は、
# ゲストの境界検査（port_wasm_natives.c の m3ApiCheckMem）と VM の出し入れ
# （port_wasm_vm.c）を、実機と同じコードのまま ctest で回したいから。
# この 2 ファイルは ESP-IDF に依存しないように書いてある。
# FreeRTOS のタスクと PSRAM を触る port_wasm.c / port_wasm_backend.c は入れない。
set(WASM3_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../components_port/wasm3/upstream/source)
file(GLOB WASM3_SRCS CONFIGURE_DEPENDS ${WASM3_DIR}/*.c ${WASM3_DIR}/extensions/*.c)
add_library(wasm3 STATIC ${WASM3_SRCS})
target_include_directories(wasm3 PUBLIC ${WASM3_DIR})
# 実機と同じ上限。ここが違うと「ホストでは通るのに実機で落ちる」が出る。
target_compile_definitions(wasm3 PUBLIC d_m3MaxNativeStack=20480)
if(MSVC)
    target_compile_options(wasm3 PRIVATE /W0)
else()
    target_compile_options(wasm3 PRIVATE -w)
endif()

set(PORT_WASM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../components_port/port_wasm)
add_executable(test_wasm_run
               tests/test_wasm_run.c
               ${PORT_WASM_DIR}/port_wasm_vm.c
               ${PORT_WASM_DIR}/port_wasm_natives.c)
target_include_directories(test_wasm_run PRIVATE ${PORT_WASM_DIR}/include)
target_link_libraries(test_wasm_run PRIVATE cdk_core cdk_testmain wasm3)
target_compile_definitions(test_wasm_run PRIVATE
    CDK_WASM_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/wasm")
if(MSVC)
    target_compile_options(test_wasm_run PRIVATE /W4 /WX /wd4996)
else()
    target_compile_options(test_wasm_run PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_wasm_run COMMAND test_wasm_run)
