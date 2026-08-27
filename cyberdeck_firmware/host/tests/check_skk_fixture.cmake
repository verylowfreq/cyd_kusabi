# テスト用の辞書 mini.skkd が、いまの skk_build.py から作れるものと
# 一致しているかを検査する。
#
# 辞書の形式はビルダ (tools/skk_build.py) とリーダ (cdk_skk_dict.c) の
# 両方に書いてある。片方だけ直すと、コミット済みの mini.skkd では
# テストが通り続けるのに、実物の辞書を作り直した瞬間に壊れる。
# ここで両者のずれを止める。
#
#   cmake -DPY=<python> -DJISYO=<mini.jisyo> -DBIN=<mini.skkd>
#         -DBUILDER=<skk_build.py> -DTMP=<出力先> -P check_skk_fixture.cmake

foreach(v PY JISYO BIN BUILDER TMP)
    if(NOT DEFINED ${v})
        message(FATAL_ERROR "${v} が指定されていません")
    endif()
endforeach()

execute_process(
    COMMAND ${PY} ${BUILDER} ${JISYO} --encoding utf-8 -o ${TMP}
    RESULT_VARIABLE rc
    OUTPUT_QUIET
    ERROR_VARIABLE err)

if(NOT rc EQUAL 0)
    message(FATAL_ERROR "skk_build.py が失敗しました (rc=${rc})\n${err}")
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND} -E compare_files ${TMP} ${BIN}
    RESULT_VARIABLE diff)

if(NOT diff EQUAL 0)
    message(FATAL_ERROR
        "${BIN} が skk_build.py の出力と違います。\n"
        "辞書の形式を変えたなら fixture を作り直してください:\n"
        "  python tools/skk_build.py host/fixtures/skk/mini.jisyo "
        "--encoding utf-8 -o host/fixtures/skk/mini.skkd")
endif()

message(STATUS "mini.skkd は skk_build.py の出力と一致")
