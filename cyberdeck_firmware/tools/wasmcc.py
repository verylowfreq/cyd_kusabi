#!/usr/bin/env python3
"""ユーザーアプリ (.c) を .wasm にする。

clang の wasm32 バックエンドをそのまま使う。**WASI SDK は要らない。**
自由形式 (-nostdlib) で組んで、libc の代わりに wasm-sdk/src/cdk_app.c を足す。

  python tools/wasmcc.py wasm-apps/hello.c
  python tools/wasmcc.py wasm-apps/hello.c -o out/hello.wasm
  python tools/wasmcc.py --all                # wasm-apps/*.c を全部

出力の既定は wasm-apps/build/<名前>.wasm。
"""

import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SDK_INC = os.path.join(ROOT, "wasm-sdk", "include")
SDK_SRC = os.path.join(ROOT, "wasm-sdk", "src", "cdk_app.c")
APPS = os.path.join(ROOT, "wasm-apps")
BUILD = os.path.join(APPS, "build")

# 実機の受け入れ上限。ここを超えるものは wasm run が弾くので、
# 作るときに気づけるようにしておく。cdk_wasm_run.h の CDK_WASM_MODULE_MAX と対。
MODULE_MAX = 256 * 1024

# 線形メモリの上限。ゲストが grow できる限界で、実機の PSRAM を食う量に効く。
INITIAL_MEMORY = 64 * 1024
MAX_MEMORY = 1024 * 1024
GUEST_STACK = 8 * 1024


def find_clang():
    for cand in ("clang", r"C:\Program Files\LLVM\bin\clang.exe"):
        p = shutil.which(cand) if os.sep not in cand else (cand if os.path.exists(cand) else None)
        if p:
            return p
    sys.exit("clang が見つかりません。PATH を通すか LLVM を入れてください。")


def compile_one(clang, src, out, verbose=False):
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    cmd = [
        clang,
        "--target=wasm32",
        "-Os",
        "-nostdlib",
        "-ffreestanding",
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I", SDK_INC,
        # --no-entry: _start は無い。cdk_main だけを出す。
        "-Wl,--no-entry",
        "-Wl,--export=cdk_main",
        # インポート（cdk モジュール）は未定義のままリンクさせる。
        "-Wl,--allow-undefined",
        f"-Wl,--initial-memory={INITIAL_MEMORY}",
        f"-Wl,--max-memory={MAX_MEMORY}",
        f"-Wl,-z,stack-size={GUEST_STACK}",
        "-Wl,--strip-all",
        "-o", out,
        src,
        SDK_SRC,
    ]
    if verbose:
        print(" ".join(cmd))
    r = subprocess.run(cmd)
    if r.returncode != 0:
        return False

    size = os.path.getsize(out)
    if size > MODULE_MAX:
        print(f"{out}: {size} バイト。実機の上限 {MODULE_MAX} を超えています", file=sys.stderr)
        return False
    print(f"{os.path.relpath(out, ROOT)}  {size} バイト")
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src", nargs="*", help="ソース (.c)")
    ap.add_argument("-o", "--out", help="出力先。src が 1 個のときだけ")
    ap.add_argument("--all", action="store_true", help="wasm-apps/*.c を全部")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    srcs = list(args.src)
    if args.all:
        srcs += sorted(os.path.join(APPS, f) for f in os.listdir(APPS) if f.endswith(".c"))
    if not srcs:
        ap.error("ソースを指定するか --all を付けてください")
    if args.out and len(srcs) != 1:
        ap.error("-o はソースが 1 個のときだけ使えます")

    clang = find_clang()
    ok = True
    for src in srcs:
        out = args.out or os.path.join(BUILD, os.path.splitext(os.path.basename(src))[0] + ".wasm")
        if not compile_one(clang, src, out, args.verbose):
            ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
