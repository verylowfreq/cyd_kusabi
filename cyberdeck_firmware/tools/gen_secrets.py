#!/usr/bin/env python3
"""gen_secrets.py — リポジトリ直下の .env から main/secrets.h を生成する。

  python tools/gen_secrets.py

.env も secrets.h も gitignore 済み。値がリポジトリに入らないようにするため、
生成物は必ず無視対象に入れておくこと（このスクリプトも起動時に確認する）。

M2 のスパイト用の暫定手段。SSH が通ったら NVS + `cdk.py config` に移し、
ファームウェアにビルド時に焼き込むのをやめる（DESIGN.md §8.1）。
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

PROJECT_DIR = Path(__file__).resolve().parent.parent
REPO_ROOT = PROJECT_DIR.parent
ENV_FILE = REPO_ROOT / ".env"
OUT_FILE = PROJECT_DIR / "main" / "secrets.h"

# .env のキー -> secrets.h のマクロ名
KEYS = {
    "TEST_WIFI_SSID": "CDK_WIFI_SSID",
    "TEST_WIFI_PSK": "CDK_WIFI_PSK",  
    "TEST_SSHSERVER_HOST": "CDK_SSH_HOST",
    "TEST_SSHSERVER_USERNAME": "CDK_SSH_USER",
    "TEST_SSHSERVER_PASSWORD": "CDK_SSH_PASS",
}


def c_string(value: str) -> str:
    """C の文字列リテラルとして安全に囲む。"""
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    escaped = escaped.replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t")
    return f'"{escaped}"'


def main() -> int:
    if not ENV_FILE.exists():
        print(f"{ENV_FILE} がありません。", file=sys.stderr)
        return 1

    env: dict[str, str] = {}
    for line in ENV_FILE.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$", line)
        if m:
            value = m.group(2).strip()
            if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
                value = value[1:-1]
            env[m.group(1)] = value

    missing = [k for k in KEYS if k not in env]
    if missing:
        print(f".env に足りないキー: {', '.join(missing)}", file=sys.stderr)
        return 1

    # 生成物が無視対象に入っているか確かめる。ここを外すと値がコミットされる。
    rel = OUT_FILE.relative_to(REPO_ROOT).as_posix()
    r = subprocess.run(["git", "check-ignore", "-q", rel], cwd=REPO_ROOT)
    if r.returncode != 0:
        print(f"{rel} が gitignore されていません。生成を中止します。", file=sys.stderr)
        return 1

    lines = [
        "/* .env から tools/gen_secrets.py が生成。手で編集しないこと。",
        " * gitignore 済み。値をリポジトリに入れないこと。",
        " * M2 の暫定手段。SSH が通ったら NVS へ移す（DESIGN.md §8.1）。 */",
        "#pragma once",
        "",
    ]
    for env_key, macro in KEYS.items():
        lines.append(f"#define {macro} {c_string(env[env_key])}")
    lines.append("")

    OUT_FILE.write_text("\n".join(lines), encoding="utf-8")
    print(f"{OUT_FILE.relative_to(REPO_ROOT)} を生成しました "
          f"({len(KEYS)} 個のマクロ、値は伏せます)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
