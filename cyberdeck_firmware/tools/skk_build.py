#!/usr/bin/env python3
"""SKK 辞書 (SKK-JISYO.*) を実機が引けるバイナリに変換する。

    python tools/skk_build.py SKK-JISYO.L -o skkdict.bin

実機で数MBのテキストをパースするのは現実的でないので、PC 側で
「索引 + レコード」に組み替える。デバイス側は索引を二分探索して
レコードを読むだけで、RAM 使用量は辞書の大きさに依存しない。

送り仮名あり / なしは**別の索引**にする。同じ索引に混ぜると
「おくr」のような送りあり見出しと普通の見出しが並んでしまい、
送り仮名変換が静かに壊れる。

形式 (little endian):

    ヘッダ 32 バイト
      0  "SKKD"
      4  u16 version = 1
      6  u16 flags        bit0: 注釈を含む
      8  u32 ari_count
     12  u32 nasi_count
     16  u32 index_off    送りあり索引の先頭。直後に送りなし索引が続く
     20  u32 record_off
     24  u32 record_len
     28  u32 reserved

    索引: u32 × (ari_count + nasi_count)
          レコード領域の先頭からの相対オフセット。見出し語の
          UTF-8 バイト列の昇順（memcmp 順）に並ぶ

    レコード: "見出し語\\0候補列\\0" の並び
              候補列は SKK と同じ "/候補1/候補2;注釈/" の形

見出し語を UTF-8 バイト順に並べ直すのは、デバイス側が memcmp だけで
比較できるようにするため。元の辞書は EUC-JP 順に並んでいるので、
そのまま使うと二分探索が成立しない。
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

MAGIC = b"SKKD"
VERSION = 1
HEADER_SIZE = 32

FLAG_ANNOTATIONS = 1 << 0

ARI_MARKER = ";; okuri-ari entries."
NASI_MARKER = ";; okuri-nasi entries."

# デバイス側の受け皿の大きさ。これを超える行は捨てて警告を出す。
# 黙って切ると、候補列の途中で切れた壊れたエントリができる。
MAX_KEY = 128
MAX_CAND = 4096


def strip_annotations(cands: str) -> str:
    """"/A;注釈/B/" → "/A/B/"。"""
    parts = [p for p in cands.split("/") if p != ""]
    out = [p.split(";", 1)[0] for p in parts]
    return "/" + "/".join(out) + "/"


def parse(path: Path, encoding: str, keep_annotations: bool):
    """(送りあり, 送りなし) の [(key, cands)] を返す。"""
    ari: list[tuple[str, str]] = []
    nasi: list[tuple[str, str]] = []
    target = nasi  # マーカーが無い辞書は送りなし扱いにする
    dropped = 0

    with path.open("rb") as f:
        raw = f.read()
    text = raw.decode(encoding, errors="replace")

    for line in text.split("\n"):
        line = line.rstrip("\r")
        if not line:
            continue
        if line.startswith(";"):
            if line.startswith(ARI_MARKER):
                target = ari
            elif line.startswith(NASI_MARKER):
                target = nasi
            continue

        sp = line.find(" ")
        if sp <= 0:
            continue
        key = line[:sp]
        cands = line[sp + 1:].strip()
        if not cands.startswith("/"):
            continue
        if not keep_annotations:
            cands = strip_annotations(cands)

        kb = key.encode("utf-8")
        cb = cands.encode("utf-8")
        if len(kb) > MAX_KEY or len(cb) > MAX_CAND:
            dropped += 1
            continue
        target.append((key, cands))

    return ari, nasi, dropped


def has_annotations(ari, nasi) -> bool:
    """候補列に注釈が 1 つでも残っているか。

    ヘッダのフラグは「注釈を残す設定でビルドしたか」ではなく
    **実際に入っているか**を表す。SKK-JISYO.L.unannotated のように
    最初から注釈の無い辞書を、--no-annotations を付けずにビルドしたとき、
    「注釈あり」と名乗ってしまうのを防ぐ。
    """
    for entries in (ari, nasi):
        for _key, cands in entries:
            if ";" in cands:
                return True
    return False


def build(ari, nasi, keep_annotations: bool) -> bytes:
    # UTF-8 バイト順に並べ直す。デバイス側は memcmp で比べるため。
    ari = sorted(ari, key=lambda kv: kv[0].encode("utf-8"))
    nasi = sorted(nasi, key=lambda kv: kv[0].encode("utf-8"))

    records = bytearray()
    ari_idx: list[int] = []
    nasi_idx: list[int] = []

    for idx, entries in ((ari_idx, ari), (nasi_idx, nasi)):
        for key, cands in entries:
            idx.append(len(records))
            records += key.encode("utf-8") + b"\x00"
            records += cands.encode("utf-8") + b"\x00"

    index_off = HEADER_SIZE
    index_len = 4 * (len(ari_idx) + len(nasi_idx))
    record_off = index_off + index_len

    flags = FLAG_ANNOTATIONS if (keep_annotations and has_annotations(ari, nasi)) else 0
    header = struct.pack(
        "<4sHHIIIIII",
        MAGIC, VERSION, flags,
        len(ari_idx), len(nasi_idx),
        index_off, record_off, len(records), 0,
    )
    assert len(header) == HEADER_SIZE, len(header)

    out = bytearray(header)
    for off in ari_idx:
        out += struct.pack("<I", off)
    for off in nasi_idx:
        out += struct.pack("<I", off)
    out += records
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description="SKK 辞書をバイナリ化する")
    ap.add_argument("input", type=Path, help="SKK-JISYO.L など")
    ap.add_argument("-o", "--out", type=Path, required=True)
    ap.add_argument("--encoding", default="euc_jp",
                    help="入力の文字コード (既定: euc_jp。UTF-8 版なら utf-8)")
    ap.add_argument("--no-annotations", action="store_true",
                    help="注釈 (;以降) を落として小さくする")
    ap.add_argument("--limit", type=int, default=0,
                    help="各索引の件数の上限。テスト用の小さな辞書を作るとき用")
    args = ap.parse_args()

    if not args.input.exists():
        print(f"[skk] {args.input} がありません", file=sys.stderr)
        return 1

    keep = not args.no_annotations
    ari, nasi, dropped = parse(args.input, args.encoding, keep)
    if args.limit > 0:
        ari = ari[: args.limit]
        nasi = nasi[: args.limit]

    annotated = keep and has_annotations(ari, nasi)
    blob = build(ari, nasi, keep)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(blob)

    print(f"[skk] {args.out}")
    print(f"[skk]   送りあり {len(ari):,} 件 / 送りなし {len(nasi):,} 件")
    print(f"[skk]   索引 {4 * (len(ari) + len(nasi)):,} バイト / "
          f"全体 {len(blob):,} バイト")
    print(f"[skk]   注釈: {'あり' if annotated else 'なし'}")
    if dropped:
        print(f"[skk]   長すぎて捨てた行: {dropped}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
