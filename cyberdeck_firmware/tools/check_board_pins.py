#!/usr/bin/env python3
"""cdk_board.h の v1 ピン定義と HARDWARE_DESIGN.md のピン表を突き合わせる。

ピン番号は 2 か所に書いてある。

  - HARDWARE_DESIGN.md  人が読む表。基板を設計するときに見る
  - cdk_board.h         ファームが使う定数

片方だけ直すと、**基板は正しいのにファームだけ別のピンを叩く**という、
実機でしか出ないうえに原因が分かりにくい壊れ方をする。ここで止める。

このスクリプトが持っているのは「どの定数がどの用途か」という対応だけで、
ピン番号そのものは持たない。番号を 3 か所に増やしたら意味がない。

    python tools/check_board_pins.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOC = ROOT.parent / "HARDWARE_DESIGN.md"
HDR = ROOT / "components_port" / "port_board" / "include" / "cdk_board.h"

# 定数 → ピン表の「用途」欄に必ず入っていてほしい文字列
EXPECT = {
    "CDK_BOARD_LCD_BL":       "Backlight",
    "CDK_BOARD_LCD_DC":       "LCD D/C",
    "CDK_BOARD_LCD_CS":       "LCD CS",
    "CDK_BOARD_LCD_MOSI":     "LCD MOSI",
    "CDK_BOARD_LCD_SCLK":     "LCD SCLK",
    "CDK_BOARD_LCD_MISO":     "LCD MISO",
    "CDK_BOARD_LCD_RST":      "LCD RESET",
    "CDK_BOARD_SD_MISO":      "SD MISO",
    "CDK_BOARD_SD_MOSI":      "SD MOSI",
    "CDK_BOARD_SD_CS":        "SD CS",
    "CDK_BOARD_SD_SCLK":      "SD SCLK",
    "CDK_BOARD_I2C_SDA":      "SDA",
    "CDK_BOARD_I2C_SCL":      "SCL",
    "CDK_BOARD_USB_DM":       "D−",
    "CDK_BOARD_USB_DP":       "D+",
    "CDK_BOARD_USB_VBUS_EN":  "VBUS",
    "CDK_BOARD_BATT_ADC_GPIO": "電池電圧センス",
    "CDK_BOARD_LINK_UART_RX": "UART1 RX",
    "CDK_BOARD_LINK_UART_TX": "UART1 TX",
    "CDK_BOARD_LOG_UART_TX":  "UART0 TX",
    "CDK_BOARD_LOG_UART_RX":  "UART0 RX",
    "CDK_BOARD_EXT_UART_RX":  "UART2 RX",
    "CDK_BOARD_EXT_UART_TX":  "UART2 TX",
    "CDK_BOARD_EXT_UART_RTS": "UART2 RTS",
    "CDK_BOARD_EXT_UART_CTS": "UART2 CTS",
    "CDK_BOARD_GROVE_ADC0":   "ADC外部入力0",
    "CDK_BOARD_GROVE_ADC1":   "ADC外部入力1",
}


def read_header_pins() -> dict[str, int]:
    """cdk_board.h の v1 側 (#elif ... #else) から #define を拾う。"""
    text = HDR.read_text(encoding="utf-8")
    start = text.find("defined(CONFIG_CDK_BOARD_V1)")
    if start < 0:
        raise SystemExit("cdk_board.h に v1 の節がありません")
    end = text.find("\n#else", start)
    if end < 0:
        end = len(text)
    body = text[start:end]

    pins: dict[str, int] = {}
    for name, value in re.findall(r"^#define\s+(CDK_BOARD_\w+)\s+(\d+)\s*(?:/\*.*)?$",
                                  body, re.MULTILINE):
        pins[name] = int(value)
    return pins


def read_doc_table() -> dict[int, str]:
    """HARDWARE_DESIGN.md のピン表を {GPIO: 用途} にする。"""
    rows: dict[int, str] = {}
    for line in DOC.read_text(encoding="utf-8").splitlines():
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip("|").split("|")]
        if len(cells) < 2:
            continue
        m = re.fullmatch(r"(\d+)", cells[0])
        if m is None:
            continue
        rows[int(m.group(1))] = cells[1]
    return rows


def main() -> int:
    pins = read_header_pins()
    doc = read_doc_table()
    problems: list[str] = []

    if not doc:
        problems.append(f"{DOC.name} からピン表を読めませんでした")

    for name, want in EXPECT.items():
        if name not in pins:
            problems.append(f"{name} が cdk_board.h の v1 節にありません")
            continue
        gpio = pins[name]
        if gpio not in doc:
            problems.append(
                f"{name} = GPIO{gpio} だが、{DOC.name} のピン表に GPIO{gpio} の行が無い")
            continue
        if want not in doc[gpio]:
            problems.append(
                f"{name} = GPIO{gpio} だが、{DOC.name} の GPIO{gpio} は "
                f"「{doc[gpio]}」で、「{want}」が入っていない")

    # 2 つの機能が同じピンに割り当たっていないか。
    seen: dict[int, str] = {}
    for name in EXPECT:
        if name not in pins:
            continue
        gpio = pins[name]
        if gpio in seen:
            problems.append(f"GPIO{gpio} に {seen[gpio]} と {name} の両方が割り当たっている")
        seen[gpio] = name

    if problems:
        print("[board] ピン定義が食い違っています:", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        print(f"\n  {DOC} と\n  {HDR}\n  の両方を直してください。", file=sys.stderr)
        return 1

    print(f"[board] v1 のピン {len(EXPECT)} 件が HARDWARE_DESIGN.md と一致")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
