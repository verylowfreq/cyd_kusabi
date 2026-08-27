# cyberdeck_firmware

ESP32-S3 サイバーデックのファームウェア。設計は [DESIGN.md](DESIGN.md)、
プロジェクト全体の背景は [../ABOUT.md](../ABOUT.md)。

現在の到達点: **M1 完了**

- M0: ビルド基盤 / ホスト側テスト / LCD 表示 / スクリーンショット転送 / 有限時間のログ取得
- M1: VT パーサ / 東雲 12px フォント / テキストコンソール（試作機で 53桁 x 20行）

次は M2（WiFi + SSH）。

---

## 前提

| もの | 場所 |
|---|---|
| ESP-IDF v5.5.5 | `C:\DATA-0\Espressif\esp-idf-v5.5.5` |
| IDF の python (pyserial 入り) | `C:\DATA-0\Espressif\python_env\idf5.5_py3.14_env\Scripts\python.exe` |
| clang | `C:\Program Files\LLVM\bin` |
| 試作機 M5Stack CoreS3 | 制御チャネル **COM22** (USB-Serial-JTAG) |
| FT232 | ログチャネル **COM75** ← CoreS3 PORT.C (G17=TX / G18=RX / GND) |

ポートは `--ctrl` / `--log` か環境変数 `CDK_CTRL` / `CDK_LOG` で変えられる。

---

## ホスト側（実機不要）

VT パーサ、文字幅、フォント描画、SKK エンジンといった中身は
ESP-IDF に依存しない純 C で `components/` に置いてある。
実機を触らずにここで回すのが基本。

```powershell
cmake -S host -B build-host -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

`no_idf_in_core` テストが `components/` に ESP-IDF 依存が混ざっていないことを
機械的に見張っている。ESP-IDF を使う処理は `components_port/` に書くこと。

---

## 実機

PowerShell で ESP-IDF を有効化してからビルドする。

```powershell
. C:\DATA-0\Espressif\esp-idf-v5.5.5\export.ps1
idf.py build
```

書き込みと動作確認は `tools/cdk.py`。**どのコマンドも必ず有限時間で終わる**
（`idf.py monitor` は終了しないのでエージェントからは使わない）。

```powershell
$py = "C:\DATA-0\Espressif\python_env\idf5.5_py3.14_env\Scripts\python.exe"

& $py tools\cdk.py flash                    # ビルド済みバイナリを書き込む
& $py tools\cdk.py boot                     # リセットして #CDK-READY まで待つ
& $py tools\cdk.py log --timeout 5          # ログを 5 秒ぶん読む
& $py tools\cdk.py ping                     # 制御チャネルの疎通確認
& $py tools\cdk.py shot out.png --verify    # テストパターンと比較
& $py tools\cdk.py shot out.png --verify-vt host\fixtures\vt\banner.vt
& $py tools\cdk.py type "hello\n"           # テキストを入力として流す
& $py tools\cdk.py key ctrl+c up f1         # キーイベントを送る
& $py tools\cdk.py selftest                 # boot + ping + shot --verify
```

`selftest` が完了条件そのもの。緑なら、実機の画面と
ホスト側で同じ純 C コードが描いた基準画像がバイト単位で一致している。

`--verify-vt` は VT フィクスチャをターミナル・フォント・描画まで通して比較する。
バナー `host/fixtures/vt/banner.vt` はファームにも焼き込んであるので、
実機とホストが文字通り同じバイト列を食べている。

---

## 構成

```
components/        ESP-IDF に依存しない純 C コア。ホストでもビルドする
  cdk_proto/         ホスト↔デバイスのフレーミング（再同期する）
  cdk_text/          UTF-8 デコード + East Asian Width
  cdk_canvas/        RGB565 キャンバス、ダーティ行管理、テストパターン
  cdk_font/          フォントブロブの読み出し + shinonome12.cdkfont
  cdk_term/          VT パーサ + 画面バッファ
  cdk_keys/          キーイベント → 端末が期待するバイト列
  cdk_screen/        セルグリッド → キャンバス描画
components_port/   ESP-IDF 依存の薄いポート層
  port_display/      BSP + esp_lcd。キャンバスをパネルへ転送
  port_link/         USB-Serial-JTAG の制御チャネル
main/              起動とタスク構成だけ
host/              ホスト側 CMake / ctest / 基準レンダラ / VT フィクスチャ
tools/             PC 側スクリプト（cdk.py / mkfont.py）
```

## フォント

東雲フォント 12px（Public Domain）。生成済みブロブ
`components/cdk_font/shinonome12.cdkfont` (207KB) をリポジトリに入れてあるので、
普段は何もしなくてよい。作り直すときだけ BDF が要る:

```powershell
# https では繋がらない。http で取ること。
curl -O http://openlab.ring.gr.jp/efont/dist/shinonome/shinonome-0.9.11p1.tar.bz2
tar xjf shinonome-0.9.11p1.tar.bz2
python tools/mkfont.py shinonome-0.9.11/bdf -o components/cdk_font/shinonome12.cdkfont
```

罫線素片・ブロック要素・CLI が使う記号は `mkfont.py` が 6x12 の半角で自前生成する。
東雲はこれらを全角でしか持たないが、端末は East Asian Width = Ambiguous に従って
半角として数えるため、全角グリフを当てると必ず桁がずれる。詳細は DESIGN.md §5。

---

## ハマりどころ（実機で踏んだもの）

- **CoreS3 の PSRAM は quad**。`CONFIG_SPIRAM_MODE_OCT=y` にすると
  `PSRAM chip is not connected, or wrong PSRAM line mode` で起動時に abort する。
- **`usb_serial_jtag_write_bytes` は TX リングバッファより大きい書き込みができない。**
  内部で丸ごとリングに積むので、リング超えの要求は何度呼んでも 0 を返す。
  必ず分割して渡す（`LINK_TX_SLICE`）。
- **CoreS3 の LCD 電源とバックライトは AXP2101 / AW9523 経由**。
  BSP の `bsp_display_new()` に任せる。自前でピンを叩くと無言で真っ黒になる。
- **ILI9342C はビッグエンディアンの RGB565 を期待する。**
  キャンバスはネイティブ（リトルエンディアン）なので転送時に入れ替える。
- **LCD と microSD が SPI3 を共有している**（MOSI 37 / MISO 35 / SCK 36）。
  さらに LCD の DC が MISO と同じ G35。M4 で排他制御が要る。
- ブート時の `i2c.master: Please check pull-up resistances` は CoreS3 では正常。
- **フォントのビット並びは「バイトの MSB が左端」**。BDF は幅ぴったりのビット列で
  持っているので、ブロブに書くときに左詰めし直す必要がある。
  ずれると 2 ビットぶん右に寄って右端の列が消える。
- **JIS X 0208 は ° ± £ など Latin-1 の文字も全角で持っている。**
  半角版を全角版で上書きすると、その文字だけ げた になる。
