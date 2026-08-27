#!/usr/bin/env python3
"""cdk.py — Cyberdeck の開発ループ用 CLI。

DESIGN.md §3 の「エージェントが自律的に回せる開発ループ」の実体。
どのコマンドも必ず有限時間で終了する。idf.py monitor のように
終わらないものは使わない。

チャネル (DESIGN.md §3.4):
  CTRL  USB-Serial-JTAG (既定 COM22)  cdk_proto のバイナリフレーム専用
  LOG   FT232 経由の UART1 (既定 COM75)  ESP_LOG のテキスト専用

使い方:
  python tools/cdk.py boot                    リセットして起動ログを取る
  python tools/cdk.py log --timeout 5         ログを 5 秒ぶん取る
  python tools/cdk.py ping                    制御チャネルの疎通確認
  python tools/cdk.py shot out.png --verify   画面を取得しホストの基準画像と比較
  python tools/cdk.py type "ls -la\\n"         テキストを入力として流し込む
  python tools/cdk.py key ctrl+c              キーイベントを送る
  python tools/cdk.py cam out.jpg            実機のパネルをカメラで撮る
  python tools/cdk.py flash                   ビルド済みバイナリを書き込む
  python tools/cdk.py selftest                boot + ping + shot --verify

pyserial が要る。ESP-IDF の python 環境にはもともと入っている:
  C:\\DATA-0\\Espressif\\python_env\\idf5.5_py3.14_env\\Scripts\\python.exe
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path

try:
    import serial  # type: ignore
except ImportError:
    sys.exit(
        "pyserial が見つかりません。ESP-IDF の python を使ってください:\n"
        "  C:\\DATA-0\\Espressif\\python_env\\idf5.5_py3.14_env\\Scripts\\python.exe "
        "tools/cdk.py ..."
    )

# デバイスのログは UTF-8。Windows のコンソール既定は CP932 なので明示する。
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

PROJECT_DIR = Path(__file__).resolve().parent.parent
BUILD_DIR = PROJECT_DIR / "build"
HOST_BUILD_DIR = PROJECT_DIR / "build-host"
TOOLS_DIR = PROJECT_DIR / "tools"
IDF_PATH = Path(os.environ.get("IDF_PATH", r"C:\DATA-0\Espressif\esp-idf-v5.5.5"))

DEFAULT_CTRL = os.environ.get("CDK_CTRL", "COM22")
DEFAULT_LOG = os.environ.get("CDK_LOG", "COM75")
LOG_BAUD = 115200

READY_SENTINEL = "#CDK-READY"

# ---- cdk_proto (components/cdk_proto/include/cdk_proto.h と一致させること) ----

MAGIC = bytes([0x1B, ord("C"), ord("D"), ord("K")])
HEADER_SIZE = 9
TRAILER_SIZE = 4
MAX_PAYLOAD = 4096

MSG_PING = 0x01
MSG_PONG = 0x02
MSG_READY = 0x03
MSG_LOG = 0x04
MSG_KEY = 0x10
MSG_TEXT = 0x11
MSG_VT = 0x12
MSG_SHOT_REQ = 0x20
MSG_SHOT_INFO = 0x21
MSG_SHOT_DATA = 0x22
MSG_SHOT_END = 0x23
MSG_ERROR = 0x7F

MSG_NAMES = {v: k for k, v in list(globals().items()) if k.startswith("MSG_")}

MOD_CTRL, MOD_SHIFT, MOD_ALT, MOD_GUI = 0x01, 0x02, 0x04, 0x08

PIXFMT_RGB565_LE = 0
PIXFMT_RGB565_BE = 1


def encode(msg_type: int, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload が大きすぎます: {len(payload)}")
    head = MAGIC + bytes([msg_type]) + struct.pack("<I", len(payload))
    body = head + payload
    return body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)


class FrameParser:
    """再同期する受信パーサ。ファーム側の scan() と同じ規則で動く。

    magic が見つかっても長さを盲信しない。CRC が合わなければ 1 バイト捨てて
    magic を探し直す。これがないとログやパニック出力が割り込んだ瞬間に
    永久に復帰できなくなる。
    """

    def __init__(self) -> None:
        self.buf = bytearray()
        self.dropped = 0
        self.bad_crc = 0

    def feed(self, data: bytes) -> list[tuple[int, bytes]]:
        self.buf += data
        out: list[tuple[int, bytes]] = []
        i = 0
        n = len(self.buf)
        while n - i >= 4:
            if self.buf[i : i + 4] != MAGIC:
                i += 1
                self.dropped += 1
                continue
            if n - i < HEADER_SIZE:
                break
            length = struct.unpack_from("<I", self.buf, i + 5)[0]
            if length > MAX_PAYLOAD:
                i += 1
                self.dropped += 1
                continue
            total = length + HEADER_SIZE + TRAILER_SIZE
            if n - i < total:
                break
            want = struct.unpack_from("<I", self.buf, i + HEADER_SIZE + length)[0]
            have = zlib.crc32(self.buf[i : i + HEADER_SIZE + length]) & 0xFFFFFFFF
            if want != have:
                i += 1
                self.dropped += 1
                self.bad_crc += 1
                continue
            out.append(
                (self.buf[i + 4], bytes(self.buf[i + HEADER_SIZE : i + HEADER_SIZE + length]))
            )
            i += total
        del self.buf[:i]
        return out


# ---------------------------------------------------------------- シリアル


def open_port(name: str, baud: int = LOG_BAUD, timeout: float = 0.1) -> serial.Serial:
    """DTR/RTS を上げずに開く。開いた瞬間にリセットが掛かるのを避ける。"""
    s = serial.Serial()
    s.port = name
    s.baudrate = baud
    s.timeout = timeout
    s.dtr = False
    s.rts = False
    try:
        s.open()
    except serial.SerialException as e:
        raise SystemExit(f"{name} を開けません: {e}")
    return s


def hard_reset(port: serial.Serial) -> None:
    """USB-Serial-JTAG 用のハードリセット (esptool の HardReset(uses_usb=True) 相当)。"""

    def set_rts(state: bool) -> None:
        port.setRTS(state)
        # Windows の usbser.sys 対策。DTR にダミーの変化を与えて
        # set-control-line-state を確実に送らせる。
        port.setDTR(port.dtr)

    set_rts(True)  # EN -> LOW
    time.sleep(0.2)
    set_rts(False)
    time.sleep(0.2)


def read_until(
    port: serial.Serial,
    timeout: float,
    sentinel: str | None = None,
    echo: bool = True,
) -> str:
    """timeout 秒 か sentinel が出るまで読む。必ず戻る。"""
    deadline = time.monotonic() + timeout
    text = ""
    pending = b""
    while time.monotonic() < deadline:
        chunk = port.read(4096)
        if not chunk:
            continue
        pending += chunk
        # UTF-8 が途中で切れても落ちないようにする
        try:
            s = pending.decode("utf-8")
            pending = b""
        except UnicodeDecodeError:
            s = pending.decode("utf-8", errors="ignore")
            pending = b""
        text += s
        if echo:
            sys.stdout.write(s)
            sys.stdout.flush()
        if sentinel and sentinel in text:
            break
    return text


# ------------------------------------------------------------------ 制御


class Ctrl:
    def __init__(self, portname: str) -> None:
        self.port = open_port(portname, baud=115200, timeout=0.05)
        self.parser = FrameParser()
        self.queue: list[tuple[int, bytes]] = []

    def close(self) -> None:
        self.port.close()

    def send(self, msg_type: int, payload: bytes = b"") -> None:
        self.port.write(encode(msg_type, payload))
        self.port.flush()

    def recv(self, want: int | None = None, timeout: float = 3.0):
        """フレームを 1 つ受け取る。want を指定するとその型が来るまで読み飛ばす。"""
        deadline = time.monotonic() + timeout
        while True:
            for i, (t, p) in enumerate(self.queue):
                if want is None or t == want:
                    del self.queue[i]
                    return t, p
            if time.monotonic() >= deadline:
                return None
            data = self.port.read(4096)
            if data:
                self.queue += self.parser.feed(data)

    def drain(self, seconds: float = 0.2) -> None:
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            data = self.port.read(4096)
            if data:
                self.queue += self.parser.feed(data)


# -------------------------------------------------------------------- PNG


def rgb565le_to_rgb888(raw: bytes, width: int, height: int) -> bytes:
    out = bytearray(width * height * 3)
    for i in range(width * height):
        v = raw[i * 2] | (raw[i * 2 + 1] << 8)
        r = (v >> 11) & 0x1F
        g = (v >> 5) & 0x3F
        b = v & 0x1F
        # 5/6 bit を 8 bit に広げる。上位ビットを下位に複製する定番の方法。
        out[i * 3 + 0] = (r << 3) | (r >> 2)
        out[i * 3 + 1] = (g << 2) | (g >> 4)
        out[i * 3 + 2] = (b << 3) | (b >> 2)
    return bytes(out)


def write_png(path: Path, rgb: bytes, width: int, height: int) -> None:
    def chunk(tag: bytes, data: bytes) -> bytes:
        return (
            struct.pack(">I", len(data))
            + tag
            + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        )

    raw = bytearray()
    stride = width * 3
    for y in range(height):
        raw.append(0)  # filter type: None
        raw += rgb[y * stride : (y + 1) * stride]

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    path.write_bytes(png)


# --------------------------------------------------------------- コマンド


def cmd_log(args) -> int:
    port = open_port(args.log, LOG_BAUD, timeout=0.1)
    try:
        text = read_until(port, args.timeout, args.until, echo=True)
    finally:
        port.close()
    if args.until and args.until not in text:
        print(f"\n[cdk] '{args.until}' が {args.timeout} 秒以内に出ませんでした", file=sys.stderr)
        return 1
    return 0


def cmd_boot(args) -> int:
    log = open_port(args.log, LOG_BAUD, timeout=0.1)
    ctrl = open_port(args.ctrl, 115200, timeout=0.05)
    try:
        log.reset_input_buffer()
        hard_reset(ctrl)
        text = read_until(log, args.timeout, READY_SENTINEL, echo=True)
    finally:
        ctrl.close()
        log.close()

    if READY_SENTINEL not in text:
        print(
            f"\n[cdk] {READY_SENTINEL} が {args.timeout} 秒以内に出ませんでした",
            file=sys.stderr,
        )
        return 1
    print(f"\n[cdk] 起動完了")
    return 0


def cmd_ping(args) -> int:
    ctrl = Ctrl(args.ctrl)
    try:
        payload = b"cdk-ping-0123456789"
        ctrl.send(MSG_PING, payload)
        got = ctrl.recv(MSG_PONG, timeout=args.timeout)
    finally:
        ctrl.close()

    if got is None:
        print(f"[cdk] PONG が返りません ({args.ctrl})", file=sys.stderr)
        return 1
    if got[1] != payload:
        print(f"[cdk] PONG の中身が違います: {got[1]!r}", file=sys.stderr)
        return 1
    print(f"[cdk] ping OK ({args.ctrl})")
    return 0


def cmd_shot(args) -> int:
    ctrl = Ctrl(args.ctrl)
    try:
        ctrl.drain(0.1)
        ctrl.send(MSG_SHOT_REQ)

        info = ctrl.recv(MSG_SHOT_INFO, timeout=args.timeout)
        if info is None:
            print("[cdk] SHOT_INFO が返りません", file=sys.stderr)
            return 1
        width, height, fmt, _r0, _r1, _r2, byte_len = struct.unpack("<HHBBBBI", info[1])
        print(f"[cdk] {width}x{height} fmt={fmt} {byte_len} バイト")

        buf = bytearray(byte_len)
        seen = 0
        deadline = time.monotonic() + args.timeout
        while seen < byte_len and time.monotonic() < deadline:
            got = ctrl.recv(timeout=1.0)
            if got is None:
                continue
            t, p = got
            if t == MSG_SHOT_DATA:
                off = struct.unpack_from("<I", p, 0)[0]
                body = p[4:]
                if off + len(body) > byte_len:
                    print(f"[cdk] 範囲外のチャンク off={off} len={len(body)}", file=sys.stderr)
                    return 1
                buf[off : off + len(body)] = body
                seen += len(body)
            elif t == MSG_SHOT_END:
                break
    finally:
        ctrl.close()

    if seen < byte_len:
        print(f"[cdk] 受信が足りません {seen}/{byte_len}", file=sys.stderr)
        return 1

    raw = bytes(buf)
    if fmt == PIXFMT_RGB565_BE:
        raw = bytes(b for pair in zip(raw[1::2], raw[0::2]) for b in pair)

    out = Path(args.out)
    write_png(out, rgb565le_to_rgb888(raw, width, height), width, height)
    print(f"[cdk] {out} に保存しました")

    if args.raw:
        Path(args.raw).write_bytes(raw)

    if args.verify or args.verify_vt:
        return verify_against_reference(raw, width, height, out, args.verify_vt,
                                        getattr(args, "preedit", None))
    return 0


def verify_against_reference(raw: bytes, width: int, height: int, png_path: Path,
                             vt_file: str | None = None,
                             preedit_file: str | None = None) -> int:
    """ホスト側で同じ内容を描いてバイト単位で比較する。

    実機と PC で同じ純 C コードを動かしているので、一致しないなら
    転送経路かキャンバスの扱いのどちらかが壊れている。

    vt_file を渡すと、そのエスケープシーケンスを端末に流した結果と比べる。
    ターミナル・フォント・描画まで含めた経路全体の検証になる。
    """
    exe = HOST_BUILD_DIR / "cdk_render.exe"
    if not exe.exists():
        exe = HOST_BUILD_DIR / "cdk_render"
    if not exe.exists():
        print(
            "[cdk] 基準レンダラがありません。先にホスト側をビルドしてください:\n"
            "  cmake -S host -B build-host -G Ninja -DCMAKE_C_COMPILER=clang\n"
            "  cmake --build build-host",
            file=sys.stderr,
        )
        return 1

    if vt_file:
        ref_path = HOST_BUILD_DIR / f"ref_vt_{width}x{height}.raw"
        cmd = [str(exe), "vt", str(vt_file), str(width), str(height), str(ref_path)]
        if preedit_file:
            # 未確定文字列は引数ではなくファイルで渡す。Windows の argv は
            # ANSI コードページなので、日本語がここで壊れる。
            cmd.append(str(preedit_file))
    else:
        ref_path = HOST_BUILD_DIR / f"ref_{width}x{height}.raw"
        cmd = [str(exe), "testpattern", str(width), str(height), str(ref_path)]
    subprocess.run(cmd, check=True)
    ref = ref_path.read_bytes()

    if len(ref) != len(raw):
        print(f"[cdk] サイズ不一致 実機={len(raw)} 基準={len(ref)}", file=sys.stderr)
        return 1
    if ref == raw:
        what = f"vt:{Path(vt_file).name}" if vt_file else "testpattern"
        print(f"[cdk] 検証 OK: 基準画像と完全一致 ({width}x{height}, {what})")
        return 0

    diff = sum(1 for a, b in zip(ref, raw) if a != b)
    first = next(i for i, (a, b) in enumerate(zip(ref, raw)) if a != b)
    px = first // 2
    print(
        f"[cdk] 検証 NG: {diff} バイト違う。最初の差分は "
        f"バイト {first} (画素 {px} = x{px % width},y{px // width})",
        file=sys.stderr,
    )
    ref_png = png_path.with_name(png_path.stem + "_ref.png")
    write_png(ref_png, rgb565le_to_rgb888(ref, width, height), width, height)
    print(f"[cdk] 基準画像を {ref_png} に出しました", file=sys.stderr)
    return 1


KEYNAMES = {
    # HID usage id
    "enter": 0x28, "esc": 0x29, "escape": 0x29, "bs": 0x2A, "backspace": 0x2A,
    "tab": 0x2B, "space": 0x2C, "right": 0x4F, "left": 0x50, "down": 0x51,
    "up": 0x52, "home": 0x4A, "end": 0x4D, "pgup": 0x4B, "pgdn": 0x4E,
    "del": 0x4C, "delete": 0x4C, "ins": 0x49, "insert": 0x49,
    **{f"f{i}": 0x3A + i - 1 for i in range(1, 13)},
}


def parse_key(spec: str) -> bytes:
    mods = 0
    parts = spec.lower().split("+")
    name = parts[-1]
    for m in parts[:-1]:
        mods |= {"ctrl": MOD_CTRL, "shift": MOD_SHIFT, "alt": MOD_ALT, "gui": MOD_GUI}[m]

    if name in KEYNAMES:
        keycode, codepoint = KEYNAMES[name], 0
    elif len(name) == 1:
        keycode, codepoint = 0, ord(name)
    else:
        raise SystemExit(f"キー名が分かりません: {name}")
    return struct.pack("<HBBI", keycode, mods, 0, codepoint)


def cmd_key(args) -> int:
    ctrl = Ctrl(args.ctrl)
    try:
        for spec in args.keys:
            ctrl.send(MSG_KEY, parse_key(spec))
            time.sleep(0.01)
    finally:
        ctrl.close()
    print(f"[cdk] {len(args.keys)} 個のキーを送りました")
    return 0


def cmd_type(args) -> int:
    text = args.text.encode("utf-8")
    ctrl = Ctrl(args.ctrl)
    try:
        for i in range(0, len(text), MAX_PAYLOAD):
            ctrl.send(MSG_TEXT, text[i : i + MAX_PAYLOAD])
            time.sleep(0.01)
    finally:
        ctrl.close()
    print(f"[cdk] {len(text)} バイト送りました")
    return 0


def cmd_vt(args) -> int:
    """バイト列を端末へ直接流して画面を確定的な状態にする。

    type は「入力」なのでシェルや SSH に届く。画面をホストの基準画像と
    比べたいときは、端末を初期化してから同じバイト列を流す必要がある。
    起動直後の画面には WiFi の IP や SD のカード名が出るので、
    そのままでは基準画像と一致しない（M2 以降 selftest が通っていなかった）。
    """
    data = Path(args.file).read_bytes()
    ctrl = Ctrl(args.ctrl)
    try:
        # 先頭 1 バイトはフラグ。bit0 を立てたフレームだけが端末を初期化する。
        # 分割送信で毎回初期化されると前半が消える。
        chunk = MAX_PAYLOAD - 1
        first = True
        for i in range(0, max(len(data), 1), chunk):
            ctrl.send(MSG_VT, bytes([1 if first else 0]) + data[i : i + chunk])
            first = False
            time.sleep(0.01)
    finally:
        ctrl.close()
    print(f"[cdk] {args.file} を端末へ流しました ({len(data)} バイト)")
    return 0


def cmd_flash(args) -> int:
    """ビルド済みのバイナリを esptool で書き込む。IDF 環境の有効化は要らない。"""
    args_file = BUILD_DIR / "flash_args"
    if not args_file.exists():
        print(f"[cdk] {args_file} がありません。先に idf.py build を実行してください。",
              file=sys.stderr)
        return 1

    cmd = [
        sys.executable, "-m", "esptool",
        "--chip", "esp32s3",
        "--port", args.ctrl,
        "--baud", str(args.baud),
        "--before", "default_reset",
        "--after", "hard_reset",
        "write_flash", "@flash_args",
    ]
    print("[cdk] " + " ".join(cmd))
    r = subprocess.run(cmd, cwd=BUILD_DIR)
    return r.returncode


def partition_offset(name: str) -> tuple[int, int] | None:
    """ビルド済みのパーティション表から (offset, size) を読む。

    オフセットを決め打ちにしない。partitions.csv を編集した瞬間に
    「焼けたのに読めない」という分かりにくい壊れ方をするため。
    """
    csv = BUILD_DIR / "partition_table" / "partition-table.bin"
    if not csv.exists():
        return None
    blob = csv.read_bytes()
    for i in range(0, len(blob), 32):
        e = blob[i:i + 32]
        if len(e) < 32 or e[0:2] != b"\xaa\x50":
            continue
        label = e[12:28].split(b"\x00")[0].decode("ascii", "replace")
        if label == name:
            off, size = struct.unpack_from("<II", e, 4)
            return off, size
    return None


def cmd_dict(args) -> int:
    """SKK 辞書をバイナリ化して skkdict パーティションへ焼く。"""
    out = Path(args.out) if args.out else (BUILD_DIR / "skkdict.bin")

    if args.input:
        build = [sys.executable, str(TOOLS_DIR / "skk_build.py"), str(args.input),
                 "-o", str(out), "--encoding", args.encoding]
        if args.no_annotations:
            build.append("--no-annotations")
        print("[cdk] " + " ".join(build))
        if subprocess.run(build).returncode != 0:
            return 1

    if not out.exists():
        print(f"[cdk] {out} がありません。入力辞書を指定してください。", file=sys.stderr)
        return 1

    part = partition_offset("skkdict")
    if part is None:
        print("[cdk] skkdict パーティションが見つかりません。先に idf.py build を。",
              file=sys.stderr)
        return 1
    off, size = part
    blob_len = out.stat().st_size
    print(f"[cdk] skkdict @0x{off:06X} {size:,} バイト / 辞書 {blob_len:,} バイト")
    if blob_len > size:
        print(f"[cdk] 辞書がパーティションに入りません（{blob_len - size:,} バイト超過）。"
              "partitions.csv を広げるか --no-annotations を使ってください。",
              file=sys.stderr)
        return 1
    if args.build_only:
        return 0

    cmd = [
        sys.executable, "-m", "esptool",
        "--chip", "esp32s3",
        "--port", args.ctrl,
        "--baud", str(args.baud),
        "--before", "default_reset",
        "--after", "hard_reset",
        "write_flash", hex(off), str(out),
    ]
    print("[cdk] " + " ".join(cmd))
    return subprocess.run(cmd).returncode


BOARDS = ("cores3", "v1")


def cmd_build(args) -> int:
    """idf.py build を非対話で回す。

    基板ごとに sdkconfig.defaults.<board> を重ねる。PSRAM のモードが
    違うので、混ぜると起動時に abort する（CoreS3 に octal を指定すると必ず）。
    基板を変えたら sdkconfig を作り直さないと前の設定が残るため、
    --board を明示したときは消してから作る。
    """
    export = IDF_PATH / "export.bat"
    if not export.exists():
        print(f"[cdk] {export} がありません", file=sys.stderr)
        return 1

    board = getattr(args, "board", None) or "cores3"
    if board not in BOARDS:
        print(f"[cdk] 知らない基板: {board}（{' / '.join(BOARDS)}）", file=sys.stderr)
        return 1
    per_board = PROJECT_DIR / f"sdkconfig.defaults.{board}"
    if not per_board.exists():
        print(f"[cdk] {per_board} がありません", file=sys.stderr)
        return 1

    # sdkconfig は「既存の値が defaults に勝つ」ので、基板を変えるときは
    # 消さないと前の基板の PSRAM 設定が residue として残る。
    sdkconfig = PROJECT_DIR / "sdkconfig"
    if getattr(args, "board", None) and sdkconfig.exists():
        marker = PROJECT_DIR / ".cdk_board"
        prev = marker.read_text(encoding="utf-8").strip() if marker.exists() else ""
        if prev != board:
            print(f"[cdk] 基板が {prev or '不明'} → {board} に変わったので sdkconfig を作り直します")
            sdkconfig.unlink()
    (PROJECT_DIR / ".cdk_board").write_text(board, encoding="utf-8")

    defaults = f"sdkconfig.defaults;sdkconfig.defaults.{board}"
    print(f"[cdk] 基板: {board}")
    # cmd.exe に list で渡すとクォートが壊れる。文字列 + shell=True で渡し、
    # 先頭を 'call' にして export.bat から確実に戻ってくるようにする。
    cmd = f'call "{export}" >NUL && idf.py -DSDKCONFIG_DEFAULTS="{defaults}" build'
    r = subprocess.run(cmd, cwd=PROJECT_DIR, shell=True)
    return r.returncode


CAMERA_SCRIPT = PROJECT_DIR.parent / "capture-camera.py"


def cmd_cam(args) -> int:
    """実機のパネルをカメラで撮る。

    スクリーンショット (shot) はキャンバスのダンプなので、パネルへの転送が
    壊れていても正しい絵が返ってくる。向き・バイト順・部分帯転送・
    バックライトといった「キャンバスから先」を見るにはこちらが要る。
    """
    if not CAMERA_SCRIPT.exists():
        print(f"[cdk] {CAMERA_SCRIPT} がありません", file=sys.stderr)
        return 1
    cmd = [sys.executable, str(CAMERA_SCRIPT), str(Path(args.out).resolve())]
    if args.index is not None:
        cmd.append(str(args.index))
    r = subprocess.run(cmd, cwd=CAMERA_SCRIPT.parent, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        return r.returncode
    print(f"[cdk] {args.out} に保存しました")
    return 0


def cmd_selftest(args) -> int:
    """M0 の完了条件をひととおり確認する。"""
    print("== boot ==")
    if cmd_boot(args) != 0:
        return 1
    time.sleep(0.3)
    print("== ping ==")
    if cmd_ping(args) != 0:
        return 1
    print("== vt banner ==")
    # 起動直後の画面には WiFi の IP や SD のカード名が出るので、
    # そのままでは基準画像と一致しない。端末を作り直してから比べる。
    args.file = str(PROJECT_DIR / "host" / "fixtures" / "vt" / "banner.vt")
    if cmd_vt(args) != 0:
        return 1
    time.sleep(0.3)

    print("== shot --verify-vt banner ==")
    args.out = str(PROJECT_DIR / "shot.png")
    args.verify = False
    args.verify_vt = str(PROJECT_DIR / "host" / "fixtures" / "vt" / "banner.vt")
    args.preedit = None
    args.raw = None
    return cmd_shot(args)


def unescape(text: str) -> str:
    r"""type コマンドの文字列に書いたエスケープを実際の制御文字にする。

    Python の unicode_escape は使わない（非 ASCII が latin-1 扱いで壊れる）。
    対応: \n \r \t \e \0 \a \b \f \v \xNN \NNN(8進) \\
    """
    simple = {
        chr(110): chr(0x0A), chr(114): chr(0x0D), chr(116): chr(0x09),
        chr(101): chr(0x1B), chr(97): chr(0x07), chr(98): chr(0x08),
        chr(102): chr(0x0C), chr(118): chr(0x0B), chr(92): chr(92),
    }
    out: list[str] = []
    i = 0
    while i < len(text):
        c = text[i]
        if c != "\\" or i + 1 >= len(text):
            out.append(c)
            i += 1
            continue
        nxt = text[i + 1]
        if nxt in simple:
            out.append(simple[nxt])
            i += 2
        elif nxt == "x" and i + 3 < len(text) + 1:
            hexpart = text[i + 2 : i + 4]
            try:
                out.append(chr(int(hexpart, 16)))
                i += 2 + len(hexpart)
            except ValueError:
                out.append(c)
                i += 1
        elif nxt.isdigit():
            j = i + 1
            while j < len(text) and j < i + 4 and text[j] in "01234567":
                j += 1
            out.append(chr(int(text[i + 1 : j], 8)))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ctrl", default=DEFAULT_CTRL, help=f"制御チャネル (既定 {DEFAULT_CTRL})")
    ap.add_argument("--log", default=DEFAULT_LOG, help=f"ログチャネル (既定 {DEFAULT_LOG})")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("log", help="ログを有限時間だけ読む")
    p.add_argument("--timeout", type=float, default=5.0)
    p.add_argument("--until", default=None, help="この文字列が出たら終了")
    p.set_defaults(func=cmd_log)

    p = sub.add_parser("boot", help="リセットして起動ログを取る")
    p.add_argument("--timeout", type=float, default=15.0)
    p.set_defaults(func=cmd_boot)

    p = sub.add_parser("ping", help="制御チャネルの疎通確認")
    p.add_argument("--timeout", type=float, default=3.0)
    p.set_defaults(func=cmd_ping)

    p = sub.add_parser("shot", help="画面を PNG で取得")
    p.add_argument("out", nargs="?", default="shot.png")
    p.add_argument("--timeout", type=float, default=20.0)
    p.add_argument("--verify", action="store_true", help="テストパターンの基準画像と比較")
    p.add_argument("--verify-vt", default=None, metavar="FILE",
                   help="VT フィクスチャをホストで描いた結果と比較")
    p.add_argument("--preedit", default=None, metavar="FILE",
                   help="日本語入力の未確定文字列を書いたファイル（--verify-vt と併用）")
    p.add_argument("--raw", default=None, help="生の RGB565 も保存する")
    p.set_defaults(func=cmd_shot)

    p = sub.add_parser("type", help="テキストを入力として送る")
    p.add_argument("text")
    p.set_defaults(func=cmd_type)

    p = sub.add_parser("key", help="キーイベントを送る (例: ctrl+c, f1, up)")
    p.add_argument("keys", nargs="+")
    p.set_defaults(func=cmd_key)

    p = sub.add_parser("build", help="idf.py build")
    p.add_argument("--board", choices=BOARDS, default=None,
                   help="基板 (既定 cores3)。変えると sdkconfig を作り直す")
    p.set_defaults(func=cmd_build)

    p = sub.add_parser("flash", help="ビルド済みバイナリを書き込む")
    p.add_argument("--baud", type=int, default=921600)
    p.set_defaults(func=cmd_flash)

    p = sub.add_parser("vt", help="端末を初期化してバイト列を流す（画面検証用）")
    p.add_argument("file")
    p.set_defaults(func=cmd_vt)

    p = sub.add_parser("dict", help="SKK 辞書を作って skkdict へ焼く")
    p.add_argument("input", nargs="?", default=None,
                   help="SKK-JISYO.L など。省略すると既存の .bin をそのまま焼く")
    p.add_argument("-o", "--out", default=None, help="中間の .bin (既定 build/skkdict.bin)")
    p.add_argument("--encoding", default="euc_jp")
    p.add_argument("--no-annotations", action="store_true")
    p.add_argument("--build-only", action="store_true", help="焼かずに作るだけ")
    p.add_argument("--baud", type=int, default=921600)
    p.set_defaults(func=cmd_dict)

    p = sub.add_parser("cam", help="実機のパネルをカメラで撮る")
    p.add_argument("out", nargs="?", default="camera.jpg")
    p.add_argument("--index", type=int, default=None, help="カメラ index")
    p.set_defaults(func=cmd_cam)

    p = sub.add_parser("selftest", help="boot + ping + shot --verify")
    p.add_argument("--timeout", type=float, default=20.0)
    p.set_defaults(func=cmd_selftest)

    args = ap.parse_args()
    if getattr(args, "text", None) is not None:
        args.text = unescape(args.text)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
