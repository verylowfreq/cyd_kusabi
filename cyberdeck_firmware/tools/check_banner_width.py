#!/usr/bin/env python3
"""起動バナーが画面の桁数に収まっているか確かめる。

不具合 #7（枠線の桁数が中身と合っていない）と同じ壊れ方を防ぐ。
あれは人間が画面を見て気づいたもので、**スクリーンショットとホストの
基準画像はバイト単位で一致していた**（同じコードが同じ間違いを描くので
一致してしまう）。だから機械で数えるならここで数えるしかない。

    python tools/check_banner_width.py

`banner.vt`   は 80 桁（等倍）
`banner40.vt` は 40 桁（2 倍。480/12 = 40）

枠線の行は**すべて同じ幅**でなければならない。1 本だけ長さが違うのが
#7 の症状だった。
"""

from __future__ import annotations

import re
import sys
import unicodedata
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIXTURES = ROOT / "host" / "fixtures" / "vt"

# ファイル → 収まるべき桁数
LIMITS = {
    "banner.vt": 80,
    "banner40.vt": 40,
}

ESC_RE = re.compile("\x1b\\[[0-9;?]*[A-Za-z]")


def width(s: str) -> int:
    """表示桁数。East Asian Width の W と F を 2 桁として数える。

    cdk_text の実装と同じ規則。ここがずれると数えても意味が無い。
    """
    n = 0
    for ch in s:
        n += 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1
    return n


def main() -> int:
    problems: list[str] = []

    for name, limit in LIMITS.items():
        path = FIXTURES / name
        if not path.exists():
            problems.append(f"{name} がありません")
            continue

        text = path.read_text(encoding="utf-8", newline="")
        frame_widths: dict[int, list[int]] = {}

        for i, raw in enumerate(text.split("\r\n"), start=1):
            vis = ESC_RE.sub("", raw)
            w = width(vis)
            if w > limit:
                problems.append(
                    f"{name}:{i} が {w} 桁で、{limit} 桁に収まりません: {vis!r}")
            # 枠線らしい行（罫線か + と - だけ）の幅を集める
            body = vis.strip()
            if body and all(c in "+-|┏┓┗┛━┃│─" for c in body):
                frame_widths.setdefault(w, []).append(i)

        # 枠の行は全部同じ幅であること（#7 の再発防止）
        if len(frame_widths) > 1:
            detail = ", ".join(f"{w}桁(行 {rows})" for w, rows in
                               sorted(frame_widths.items()))
            problems.append(f"{name} の枠線の幅が揃っていません: {detail}")

    if problems:
        print("[banner] バナーの桁数が合っていません:", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 1

    print(f"[banner] {', '.join(f'{k} <= {v}桁' for k, v in LIMITS.items())} を確認")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
