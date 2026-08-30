#!/usr/bin/env python3
"""host/fixtures/wasm/*.wasm を作り直す。

fixture の .wasm は **コミットしてある**。ctest に clang を要求しないため
（実機を持たない環境でもホストテストが 1 秒で回ることを守る）。

出力は clang のバージョンで変わるので、SKK 辞書のような
「再生成してバイト一致」の ctest は付けていない。ABI を変えたときや
SDK を直したときに、ここを手で回して差分をコミットする。

  python host/fixtures/wasm/build.py
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))  # cyberdeck_firmware/
sys.path.insert(0, os.path.join(ROOT, "tools"))

import wasmcc  # noqa: E402


def main():
    clang = wasmcc.find_clang()
    src_dir = os.path.join(HERE, "src")
    ok = True

    for f in sorted(os.listdir(src_dir)):
        if not f.endswith(".c"):
            continue
        src = os.path.join(src_dir, f)
        out = os.path.join(HERE, os.path.splitext(f)[0] + ".wasm")

        if f == "f_noentry.c":
            # cdk_main を出さないモジュール。--export=cdk_main を付けると
            # リンクで落ちるので、ここだけ素の clang を呼ぶ。
            cmd = [clang, "--target=wasm32", "-Os", "-nostdlib", "-ffreestanding",
                   "-std=c11", "-I", wasmcc.SDK_INC,
                   "-Wl,--no-entry", "-Wl,--export=not_main",
                   "-Wl,--allow-undefined", "-Wl,--strip-all",
                   "-o", out, src, wasmcc.SDK_SRC]
            if subprocess.run(cmd).returncode != 0:
                ok = False
            else:
                print(f"{os.path.basename(out)}  {os.path.getsize(out)} バイト")
            continue

        if not wasmcc.compile_one(clang, src, out):
            ok = False

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
