#!/usr/bin/env python3
"""Rust のユーザーアプリを `.wasm` にして、C 版と同じ置き場に並べる。

  python tools/wasmrs.py --all          # ワークスペース全部
  python tools/wasmrs.py hello echo     # 名前を指定して

ターゲットとリンク指定は `wasm-sdk/rust/.cargo/config.toml` にあるので、
`cargo build --release` だけでも同じものが出る。この道具がやるのは
そのあとの 2 つ:

- `wasm-apps/build/rs_<名前>.wasm` へ写す。**`rs_` を付けるのは C 版と
  名前がぶつからないため**で、実機の `wasm ls` に並べて見比べられる。
- `wasminfo.check()` で、ファームが受け取れる形かを確かめる
  （大きさ・線形メモリの宣言・`cdk_main` の有無・インポート元）。

実機へは C 版と同じく:

  python tools/cdk.py push wasm-apps/build/rs_hello.wasm apps/rs_hello.wasm
"""

import argparse
import os
import shutil
import subprocess
import sys

import wasminfo
from wasmcc import BUILD, ROOT

RUST = os.path.join(ROOT, "wasm-sdk", "rust")
TARGET = os.path.join(RUST, "target", "wasm32-unknown-unknown", "release")

# SDK そのもの。アプリではないので配らない。
NOT_AN_APP = {"cdk-app"}


def cargo():
    exe = shutil.which("cargo")
    if not exe:
        sys.exit("cargo が見つかりません。rustup を入れて "
                 "`rustup target add wasm32-unknown-unknown` してください。")
    return exe


def app_names():
    """wasm-sdk/rust/apps/ にあるクレート名。"""
    apps = os.path.join(RUST, "apps")
    if not os.path.isdir(apps):
        return []
    return sorted(d for d in os.listdir(apps)
                  if os.path.isdir(os.path.join(apps, d, "src")))


def build(names, verbose=False):
    cmd = [cargo(), "build", "--release"]
    for n in names:
        cmd += ["--package", n]
    if verbose:
        print(" ".join(cmd))
    # cargo は wasm-sdk/rust から回す（.cargo/config.toml を効かせるため）。
    return subprocess.run(cmd, cwd=RUST).returncode == 0


def stage(name):
    """target から wasm-apps/build/rs_<名前>.wasm へ写して検査する。"""
    # クレート名のハイフンは cargo が下線に直す。
    src = os.path.join(TARGET, name.replace("-", "_") + ".wasm")
    if not os.path.exists(src):
        print(f"{name}: {src} がありません", file=sys.stderr)
        return False

    os.makedirs(BUILD, exist_ok=True)
    dst = os.path.join(BUILD, f"rs_{name}.wasm")
    shutil.copyfile(src, dst)

    size = os.path.getsize(dst)
    if not wasminfo.check(dst):
        return False
    print(f"{os.path.relpath(dst, ROOT)}  {size} バイト")
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("names", nargs="*", help="クレート名（apps/ の下）")
    ap.add_argument("--all", action="store_true", help="apps/ を全部")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    names = list(args.names)
    if args.all:
        names += app_names()
    if not names:
        ap.error("クレート名を指定するか --all を付けてください")

    bad = [n for n in names if n in NOT_AN_APP]
    if bad:
        ap.error(f"{bad} はアプリではありません（SDK 本体）")

    names = sorted(set(names))
    if not build(names, args.verbose):
        return 1

    ok = True
    for n in names:
        if not stage(n):
            ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    sys.exit(main())
