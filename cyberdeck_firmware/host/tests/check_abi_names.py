#!/usr/bin/env python3
"""ゲスト ABI のインポート名が 3 か所で一致しているかを見る。

名前の一覧は 3 か所にある:

1. `components_port/port_wasm/port_wasm_natives.c` の `k_natives[]` … **正**
2. `wasm-sdk/include/cdk_app.h` の `CDK_IMPORT(...)` … C の SDK
3. `wasm-sdk/rust/cdk-app/src/sys.rs` の `unsafe extern` ブロック … Rust の SDK

**片方だけに足すと、その言語で書いたアプリだけが動かなくなる。**
しかも症状は「呼んだ瞬間に落ちる」で、アプリが悪いのかファームが悪いのかが
実機の画面からは分からない。ピン番号を 2 か所に持っている問題
（`check_board_pins.py`）と同じ形なので、同じように機械で見る。

## 空振りで通らないようにしてある

`wasm_natives_checked` は最初、区切りの取り方を誤って「0 個 対 0 個」で
全部通っていた。同じ形の失敗がここでも起きうる（正規表現がソースの
書き方に合わなくなると 0 件になり、0 と 0 は一致してしまう）。
なので **突き合わせる前に、3 か所それぞれで最低数を満たしているか**を見る。
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))  # cyberdeck_firmware/

NATIVES = os.path.join(ROOT, "components_port", "port_wasm", "port_wasm_natives.c")
C_HEADER = os.path.join(ROOT, "wasm-sdk", "include", "cdk_app.h")
RUST_SYS = os.path.join(ROOT, "wasm-sdk", "rust", "cdk-app", "src", "sys.rs")

# 正規表現がソースの書き方に追随できなくなったときに気づくための下限。
# ABI を増やすときはここも上げる（減らすことは想定していない）。
MIN_NAMES = 45


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def from_natives(src):
    """k_natives[] の {"name", "sig", &n_name} を拾う。"""
    return set(re.findall(r'\{\s*"(\w+)"\s*,\s*"[^"]*"\s*,\s*&\w+\s*\}', src))


def from_c_header(src):
    """CDK_IMPORT(name) を拾う。定義そのもの (#define) は除く。"""
    names = set(re.findall(r'CDK_IMPORT\((\w+)\)', src))
    names.discard("n")  # #define CDK_IMPORT(n) ... の引数名
    return names


def from_rust(src):
    """unsafe extern ブロックの中の pub fn だけを拾う。

    ブロックの外にも定数などがあるので、まず範囲を切り出す。
    """
    m = re.search(r'unsafe\s+extern\s+"C"\s*\{', src)
    if not m:
        return set()
    # 対応する閉じ括弧まで数える（中に { } を含む宣言は無いが、素直に数える）
    i = m.end() - 1
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                block = src[i:j]
                break
    else:
        return set()
    return set(re.findall(r'pub\s+fn\s+(\w+)\s*\(', block))


def main():
    sources = [
        ("ファームの表 (k_natives)", NATIVES, from_natives),
        ("C の SDK (cdk_app.h)", C_HEADER, from_c_header),
        ("Rust の SDK (sys.rs)", RUST_SYS, from_rust),
    ]

    found = {}
    bad = False
    for label, path, extract in sources:
        if not os.path.exists(path):
            print(f"{path} がありません", file=sys.stderr)
            return 1
        names = extract(read(path))
        found[label] = names

        # ここが空振り防止。0 対 0 の一致を通さない。
        if len(names) < MIN_NAMES:
            print(f"{label}: {len(names)} 個しか読めませんでした"
                  f"（最低 {MIN_NAMES}）。この検査がソースの書き方に"
                  f"追随できていない可能性があります: {path}", file=sys.stderr)
            bad = True

    if bad:
        return 1

    truth_label, truth = sources[0][0], found[sources[0][0]]
    for label, _, _ in sources[1:]:
        names = found[label]
        missing = sorted(truth - names)
        extra = sorted(names - truth)
        if missing:
            print(f"{label} に足りません: {', '.join(missing)}", file=sys.stderr)
            bad = True
        if extra:
            print(f"{label} に余計なものがあります: {', '.join(extra)}", file=sys.stderr)
            bad = True

    if bad:
        print(f"（正は {truth_label}: {len(truth)} 個）", file=sys.stderr)
        return 1

    print(f"ABI のインポート名 {len(truth)} 個が 3 か所で一致しています")
    return 0


if __name__ == "__main__":
    sys.exit(main())
