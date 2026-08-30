#!/usr/bin/env python3
"""`.wasm` の中身を覗く。ファームが受け取れる形かを出す前に確かめる。

見るのは 3 つ:

- **大きさ** — ファームが読み込む上限 (`CDK_WASM_MODULE_MAX` = 256KB)。
- **線形メモリの宣言** — ファームは 1MB (16 ページ) を超える確保を断る。
  ここが「最大 = 無制限」のまま出ていると、**ロードは通るのに走らせてから
  memory.grow で落ちる**という分かりにくい壊れ方をする。
  Rust の cdylib は `--max-memory` を渡さないとそうなるので、必ず見る。
- **インポートとエクスポート** — `cdk_main` が出ているか、
  インポートが全部 `cdk` モジュールから来ているか。
  綴りを間違えたインポートはロード時ではなく**リンク時に黙って無視され**、
  呼んだ瞬間に「未知のインポート」で落ちる。

  python tools/wasminfo.py wasm-apps/build/hello.wasm
"""

import argparse
import io
import sys

# ファーム側の枠。数を 2 か所に持たないよう wasmcc から借りる。
from wasmcc import MODULE_MAX

# d_m3MaxLinearMemoryPages と対。ここを変えるときは
# components_port/wasm3/CMakeLists.txt と host/CMakeLists.txt も一緒に。
MAX_MEMORY_PAGES = 16

IMPORT_MODULE = "cdk"
ENTRY = "cdk_main"


def _leb(b, i):
    """符号なし LEB128 を 1 つ読む。"""
    r = 0
    s = 0
    while True:
        c = b[i]
        i += 1
        r |= (c & 0x7F) << s
        if not c & 0x80:
            return r, i
        s += 7


def inspect(path):
    """`.wasm` を読んで {size, imports, exports, mem} を返す。"""
    b = io.open(path, "rb").read()
    if b[:4] != b"\0asm":
        raise ValueError(f"{path} は wasm ではありません")

    out = {"size": len(b), "imports": [], "exports": [], "mem": None}
    i = 8
    while i < len(b):
        sid = b[i]
        i += 1
        size, i = _leb(b, i)
        end = i + size
        j = i

        if sid == 2:  # import
            n, j = _leb(b, j)
            for _ in range(n):
                ln, j = _leb(b, j)
                mod = b[j:j + ln].decode("utf-8", "replace")
                j += ln
                ln, j = _leb(b, j)
                nm = b[j:j + ln].decode("utf-8", "replace")
                j += ln
                kind = b[j]
                j += 1
                if kind == 0:            # 関数: 型番号
                    _, j = _leb(b, j)
                elif kind == 1:          # テーブル: 要素型 + limits
                    j += 1
                    fl = b[j]; j += 1
                    _, j = _leb(b, j)
                    if fl & 1:
                        _, j = _leb(b, j)
                elif kind == 2:          # メモリ: limits
                    fl = b[j]; j += 1
                    _, j = _leb(b, j)
                    if fl & 1:
                        _, j = _leb(b, j)
                elif kind == 3:          # グローバル: 値型 + 可変
                    j += 2
                out["imports"].append((mod, nm))

        elif sid == 5:  # memory
            n, j = _leb(b, j)
            for _ in range(n):
                fl = b[j]; j += 1
                mn, j = _leb(b, j)
                mx = None
                if fl & 1:
                    mx, j = _leb(b, j)
                out["mem"] = (mn, mx)

        elif sid == 7:  # export
            n, j = _leb(b, j)
            for _ in range(n):
                ln, j = _leb(b, j)
                nm = b[j:j + ln].decode("utf-8", "replace")
                j += ln
                j += 1               # 種別
                _, j = _leb(b, j)    # 番号
                out["exports"].append(nm)

        i = end
    return out


def check(path, verbose=False):
    """ファームに渡せる形かを見る。駄目なら理由を並べて False。"""
    info = inspect(path)
    bad = []

    if info["size"] > MODULE_MAX:
        bad.append(f"大きすぎます: {info['size']} バイト（上限 {MODULE_MAX}）")

    if ENTRY not in info["exports"]:
        bad.append(f"{ENTRY} をエクスポートしていません")

    mem = info["mem"]
    if mem is None:
        # メモリを持たないモジュール。(ptr,len) を渡す口が一切無いことになる。
        bad.append("線形メモリがありません")
    else:
        mn, mx = mem
        if mn > MAX_MEMORY_PAGES:
            bad.append(f"初期メモリが {mn} ページ（上限 {MAX_MEMORY_PAGES}）")
        if mx is None:
            bad.append(
                "線形メモリの最大が宣言されていません。"
                f"リンク時に --max-memory={MAX_MEMORY_PAGES * 65536} を渡してください"
                "（走らせてから memory.grow で落ちます）"
            )
        elif mx > MAX_MEMORY_PAGES:
            bad.append(f"最大メモリが {mx} ページ（上限 {MAX_MEMORY_PAGES}）")

    others = sorted({m for m, _ in info["imports"]} - {IMPORT_MODULE})
    if others:
        bad.append(f"{IMPORT_MODULE} 以外からインポートしています: {others}")

    if verbose or bad:
        mn, mx = mem if mem else (0, None)
        print(f"  ファイル   : {info['size']} バイト")
        print(f"  メモリ     : 初期 {mn} / 最大 {mx if mx is not None else '無制限'} ページ")
        print(f"  エクスポート: {', '.join(info['exports'])}")
        names = sorted(n for _, n in info["imports"])
        print(f"  インポート : {len(names)} 個  {', '.join(names)}")
    for msg in bad:
        print(f"  ✗ {msg}", file=sys.stderr)

    return not bad


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+")
    ap.add_argument("-q", "--quiet", action="store_true", help="問題があるときだけ出す")
    args = ap.parse_args()

    ok = True
    for f in args.files:
        print(f)
        if not check(f, verbose=not args.quiet):
            ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
    sys.exit(main())
