# ゲストの線形メモリの境界検査が抜けていないかを機械的に見る。
#
# port_wasm_natives.c の native 関数ひとつごとに、m3ApiGetArgMem の数と
# m3ApiCheckMem の数が一致していること。
#
# 「近くに m3ApiCheckMem が 1 個あればよい」では駄目で、バッファを 2 本取る
# i2c_wrrd や sock_connect や con_size で片方だけ検査していても通ってしまう。
# ここが抜けるとゲストがサンドボックスの外を読み書きできる。人間のレビューに
# 任せる種類の間違いではないので、テストとして常時回す。
#
#   cmake -DNATIVES=<path to port_wasm_natives.c> -P check_wasm_natives.cmake

if(NOT DEFINED NATIVES)
    message(FATAL_ERROR "NATIVES を渡してください")
endif()
if(NOT EXISTS "${NATIVES}")
    message(FATAL_ERROR "${NATIVES} がありません")
endif()

file(READ "${NATIVES}" src)

# CMake のリストは ; 区切りなので、C のソースをそのまま MATCHALL に掛けると
# 文の切れ目でばらばらになる。数えるのに ; は要らないので先に潰しておく。
string(REPLACE ";" "%SEMI%" src "${src}")

# 関数の切れ目で割る。先頭の断片はファイル冒頭のコメントなので捨てる
# （説明文の中にマクロ名が出てくる）。
string(REPLACE "static m3ApiRawFunction(" "\n@@FN@@" src "${src}")
string(REGEX MATCHALL "@@FN@@[^@]*" chunks "${src}")

list(LENGTH chunks nfn)
if(nfn LESS 30)
    message(FATAL_ERROR
        "native 関数が ${nfn} 個しか見つかりません。"
        "この検査がファイルの形に追随できていない可能性があります")
endif()

set(bad "")
foreach(chunk ${chunks})
    # 関数名は "@@FN@@n_con_write)" のように続く
    string(REGEX MATCH "^@@FN@@([A-Za-z0-9_]+)" _m "${chunk}")
    set(name "${CMAKE_MATCH_1}")

    string(REGEX MATCHALL "m3ApiGetArgMem" gets "${chunk}")
    string(REGEX MATCHALL "m3ApiCheckMem" checks "${chunk}")
    list(LENGTH gets ngets)
    list(LENGTH checks nchecks)

    if(NOT ngets EQUAL nchecks)
        list(APPEND bad
             "  ${name}: m3ApiGetArgMem x${ngets} に対し m3ApiCheckMem x${nchecks}")
    endif()
endforeach()

if(bad)
    string(REPLACE ";" "\n" bad "${bad}")
    message(FATAL_ERROR
        "境界検査の数が合っていません。ポインタ引数ごとに m3ApiCheckMem を書いてください:\n"
        "${bad}")
endif()

message(STATUS "native ${nfn} 個の境界検査は対になっています")
