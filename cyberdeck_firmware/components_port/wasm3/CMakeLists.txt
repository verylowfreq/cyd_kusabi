# wasm3 を ESP-IDF コンポーネントとして包む。
# 上流の source/CMakeLists.txt は使わず、そこの sources をそのまま並べる。
# WASI 系（m3_api_wasi / uvwasi / meta_wasi）と m3_api_libc、tracer は入れない。
# ユーザーアプリの ABI は cdk_wasm が持っており、WASI は名乗らない（DESIGN.md §15）。
#
# 上流: https://github.com/wasm3/wasm3  MIT
# 取り込んだコミットは upstream/COMMIT.txt。

set(WASM3_SRCS
    m3_bind.c
    m3_code.c
    m3_compile.c
    m3_core.c
    m3_env.c
    m3_exec.c
    m3_function.c
    m3_info.c
    m3_module.c
    m3_parse.c
    m3_validate.c
    extensions/m3_extensions.c
)
list(TRANSFORM WASM3_SRCS PREPEND "upstream/source/")

idf_component_register(SRCS ${WASM3_SRCS}
                       INCLUDE_DIRS "upstream/source")

# インタプリタはネイティブスタックを再帰で使う。上限を FreeRTOS タスクの
# スタック (CDK_WASM_TASK_STACK) より小さく取り、暴走した再帰がスタックを
# 壊す前に wasm3 側でトラップさせる。既定の 8MB のままだと素通りする。
# インタプリタはネイティブスタックを再帰で使う。上限を FreeRTOS タスクの
# スタック (CDK_WASM_TASK_STACK) より小さく取り、暴走した再帰がスタックを
# 壊す前に wasm3 側でトラップさせる。既定の 8MB のままだと素通りする。
#
# 線形メモリは 1MB (16 ページ) で打ち止める。**ここを入れるまで、
# 「1MB まで」は C SDK のリンク指定だけで、ファームは何も制限していなかった**
# （wasm3 の既定は 4GB）。検査は ResizeMemory の中にあるので、
# インスタンス化のときだけでなく memory.grow にも効く。
# tools/wasminfo.py の MAX_MEMORY_PAGES と対。
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    d_m3MaxNativeStack=49152
    d_m3MaxLinearMemoryPages=16)

# 上流のコードは自分たちの警告設定でビルドされている。
# こちらの -Werror を持ち込まない。
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-type-limits
)
