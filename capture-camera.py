#!/usr/bin/env python3

from datetime import datetime
from pathlib import Path
import sys

import cv2


DEFAULT_CAMERA_INDEX = 3
FRAME_WIDTH = 1280
FRAME_HEIGHT = 720

USAGE = """usage: capture-camera.py [出力パス] [カメラindex]

引数なしなら camera-YYYYMMDDhhmmss.jpg に保存する。
出力パスを渡すとそこに保存する（cdk.py cam から使う）。
保存したパスを標準出力に 1 行で出す。"""


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    if any(a in ("-h", "--help") for a in sys.argv[1:]):
        print(USAGE)
        return 0

    out_arg = args[0] if len(args) >= 1 else None
    index = int(args[1]) if len(args) >= 2 else DEFAULT_CAMERA_INDEX

    camera = cv2.VideoCapture(index, cv2.CAP_DSHOW)

    if not camera.isOpened():
        print(
            f"エラー: カメラを開けませんでした。index={index}",
            file=sys.stderr,
        )
        return 1

    try:
        camera.set(cv2.CAP_PROP_FRAME_WIDTH, FRAME_WIDTH)
        camera.set(cv2.CAP_PROP_FRAME_HEIGHT, FRAME_HEIGHT)

        # 実際に設定された解像度を確認
        actual_width = int(camera.get(cv2.CAP_PROP_FRAME_WIDTH))
        actual_height = int(camera.get(cv2.CAP_PROP_FRAME_HEIGHT))

        if (actual_width, actual_height) != (FRAME_WIDTH, FRAME_HEIGHT):
            print(
                f"警告: 1280x720を要求しましたが、"
                f"実際の解像度は{actual_width}x{actual_height}です。",
                file=sys.stderr,
            )

        # 自動露出やホワイトバランスが安定するまで読み捨てる
        frame = None
        for _ in range(10):
            success, frame = camera.read()
            if not success:
                print(
                    "エラー: カメラから画像を取得できませんでした。",
                    file=sys.stderr,
                )
                return 1

        if out_arg:
            output_path = Path(out_arg)
            output_path.parent.mkdir(parents=True, exist_ok=True)
        else:
            timestamp = datetime.now().strftime("%Y%m%d%H%M%S")
            output_path = Path(f"camera-{timestamp}.jpg")

        if not cv2.imwrite(str(output_path), frame):
            print(
                f"エラー: 画像を保存できませんでした: {output_path}",
                file=sys.stderr,
            )
            return 1

        print(output_path)
        return 0

    finally:
        camera.release()


if __name__ == "__main__":
    raise SystemExit(main())