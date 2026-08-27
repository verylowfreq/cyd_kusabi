#!/usr/bin/env python3
"""dev-stats-hook.py — Claude Code の PostToolUse フックから呼ばれて開発統計を数える。

DEV_STATS.md の機械的に数えられる項目（ビルド・書き込み・テスト・カメラ撮影）を
自動でカウントする。人間が数えると必ずずれるし、AI が後から会話ログを
読み返して再構成しても概算にしかならないため。

判断が要る項目（不具合数、人間の介入回数など）は数えない。
それらは DEV_STATS.md に手で書く。

呼ばれ方:
    python dev-stats-hook.py          # PostToolUse（成功）
    python dev-stats-hook.py fail     # PostToolUseFailure

標準入力にフックの JSON が来る。カウンタは .dev-stats.json に貯める。

**このスクリプトは何があっても exit 0 で終わること。**
フックが失敗すると開発の邪魔になるだけで、統計は本質ではない。
"""

from __future__ import annotations

import json
import re
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent
COUNTER_FILE = ROOT / ".dev-stats.json"

# コマンド文字列 → カウンタ名。当たったものは**すべて**数える。
# 1 回の呼び出しで build と flash を続けて実行することが多く、
# 最初の 1 つだけ数えると取りこぼす。
CLASSIFIERS: list[tuple[str, str]] = [
    # 書き込みはビルドより先に判定する（cdk.py flash は内部で esptool を呼ぶ）
    ("flash", r"cdk\.py\s+flash|idf\.py\s+.*\bflash\b|write_flash"),
    ("camera", r"capture-camera\.py|cdk\.py\s+cam\b"),
    ("test_run", r"\bctest\b"),
    ("build_device", r"idf\.py\s+.*\bbuild\b|cdk\.py\s+build\b"),
    ("build_host", r"cmake\s+--build"),
    ("boot", r"cdk\.py\s+boot\b"),
    ("selftest", r"cdk\.py\s+selftest\b"),
    ("shot", r"cdk\.py\s+shot\b"),
]

# 出力から結果を読み取る。exit code は grep などのパイプで潰れるので当てにしない。
TEST_PASS = re.compile(r"100% tests passed")
TEST_FAIL = re.compile(r"tests? (?:failed|FAILED)|The following tests FAILED")
BUILD_OK = re.compile(r"Project build complete|Linking C executable|ninja: no work to do")
BUILD_NG = re.compile(r"^FAILED:|fatal error|error:", re.MULTILINE)


def load() -> dict:
    try:
        return json.loads(COUNTER_FILE.read_text(encoding="utf-8"))
    except Exception:
        return {}


def save(data: dict) -> None:
    data["updated"] = datetime.now(timezone.utc).astimezone().isoformat(timespec="seconds")
    tmp = COUNTER_FILE.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(data, ensure_ascii=False, indent=2, sort_keys=True),
                   encoding="utf-8")
    tmp.replace(COUNTER_FILE)


def bump(data: dict, key: str, n: int = 1) -> None:
    data[key] = int(data.get(key, 0)) + n


def main() -> int:
    try:
        raw = sys.stdin.read()
        if not raw.strip():
            return 0
        payload = json.loads(raw)
    except Exception:
        return 0

    try:
        command = str(payload.get("tool_input", {}).get("command", ""))
        if not command:
            return 0

        kinds = [name for name, pattern in CLASSIFIERS if re.search(pattern, command)]
        if not kinds:
            return 0

        # 出力は tool_response 側。まるごと文字列にして探す。
        try:
            out = json.dumps(payload.get("tool_response", ""), ensure_ascii=False)
        except Exception:
            out = ""

        data = load()
        for kind in kinds:
            bump(data, kind)

        if "test_run" in kinds:
            if TEST_PASS.search(out):
                bump(data, "test_pass")
            elif TEST_FAIL.search(out):
                bump(data, "test_fail")
        if "build_device" in kinds or "build_host" in kinds:
            if BUILD_NG.search(out):
                bump(data, "build_fail")
            elif BUILD_OK.search(out):
                bump(data, "build_ok")

        save(data)
    except Exception:
        # 統計のために開発を止めない
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
