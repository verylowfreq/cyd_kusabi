# cyberdeck_firmware

ESP32-S3 サイバーデックのファームウェア。設計は [DESIGN.md](DESIGN.md)、
プロジェクト全体の背景は [../ABOUT.md](../ABOUT.md)。

**使い方は [../docs/MANUAL.md](../docs/MANUAL.md)**（機械を触る人向け）。

現在の到達点: **M8 完了**

- M0: ビルド基盤 / ホスト側テスト / LCD 表示 / スクリーンショット転送 / 有限時間のログ取得
- M1: VT パーサ / 東雲 12px フォント / テキストコンソール（試作機で 53桁 x 20行）
- M2: WiFi + libssh2 による **PTY 付き SSH**。実機で `top` が動く
- M3: 行編集つきシェル + GPIO / ADC / I2C。認証情報を NVS へ
- M4: microSD（SPI バス共有の解決）+ ファイル系コマンド + `known_hosts` によるホスト鍵検証
- M5: **SKK 日本語入力**。SKK-JISYO.L 全体（17.5 万語）をフラッシュに載せ、学習も残す
- M6: **テキストエディタ**。`edit <file>` で SD 上の文章を日本語入力つきで書く
- M7: **最終ハード (v1) 移行**。USB キーボード / 480x320 ILI9488 / ステータス帯 /
  公開鍵認証 / SSH 再接続
- M8: **WebAssembly ユーザーアプリ**。`wasm run` で `.wasm` を走らせる。
  WASI でも POSIX 互換でもない独自 ABI（コンソール / ファイル / GPIO / ADC /
  I2C / UART / TCP・UDP / TLS）。焼き直さずに機能を足せる。
  SDK は **C と Rust の両方**（`wasm-sdk/` と `wasm-sdk/rust/`）

加えて **ターミナルのスクロールバック 200 行**（`Shift+PgUp`。DESIGN.md §6.5）、
`scp`（Windows の OpenSSH にも）、`wget`（HTTP / HTTPS）、シリアル端末 `uart`。

残件と「やらないこと」は DESIGN.md §13 / §15.15 を参照。

---

## 前提

場所と COM 番号は開発機の例。

| もの | 場所 |
|---|---|
| ESP-IDF v5.5.5 | `C:\DATA-0\Espressif\esp-idf-v5.5.5` |
| IDF の python (pyserial 入り) | `C:\DATA-0\Espressif\python_env\idf5.5_py3.14_env\Scripts\python.exe` |
| clang / CMake / Ninja | `C:\Program Files\LLVM\bin` |
| Python 3 | ホスト側テストの一部（`board_pins` など 4 件）と `tools/*.py` |
| 試作機 M5Stack CoreS3 | 制御チャネル **COM22** (USB-Serial-JTAG) |
| FT232 | ログチャネル **COM75** ← CoreS3 PORT.C (G17=TX / G18=RX / GND) |

**v1 (最終ハード) はポートの乗り方が違う。** USB-Serial-JTAG が無いので、
書き込みとログが UART0 (**COM76**)、制御チャネルが UART1 (**COM75**)。
`flash` / `boot` などは `.cdk_board` を見てポートを使い分ける。
`.cdk_board` を書くのは `build --board`。省くと前回の基板を引き継ぐ（初回は `cores3`）。

上書きするときは `--ctrl` / `--log` / `--prog` を**サブコマンドの前**に置く
（`cdk.py --prog COM5 flash`）か、環境変数 `CDK_CTRL` / `CDK_LOG` / `CDK_PROG`。

**初回は `main/secrets.h` を作る。** リポジトリ直下の `.env`
（`TEST_WIFI_SSID` などの 5 つ。値は空でもよい）から `python tools\gen_secrets.py` で生成する。
無いとビルドが通らない（DESIGN.md §8.1）。

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

PowerShell で ESP-IDF を有効化してから `cdk.py build` でビルドする。
`idf.py build` 単体では基板ごとの `sdkconfig.defaults.<board>` が重ならないので使わない。

```powershell
. C:\DATA-0\Espressif\esp-idf-v5.5.5\export.ps1
```

書き込みと動作確認は `tools/cdk.py`。**どのコマンドも必ず有限時間で終わる**
（`idf.py monitor` は終了しないのでエージェントからは使わない）。

```powershell
$py = "C:\DATA-0\Espressif\python_env\idf5.5_py3.14_env\Scripts\python.exe"

& $py tools\cdk.py build --board v1         # 最終ハード向け
& $py tools\cdk.py build --board cores3     # 試作機
& $py tools\cdk.py build                    # 前回の基板のまま（初回は cores3）
& $py tools\cdk.py flash                    # ビルド済みバイナリを書き込む
& $py tools\cdk.py boot                     # リセットして #CDK-READY まで待つ
& $py tools\cdk.py log --timeout 5          # ログを 5 秒ぶん読む
& $py tools\cdk.py ping                     # 制御チャネルの疎通確認
& $py tools\cdk.py shot out.png --verify    # テストパターンと比較
& $py tools\cdk.py shot out.png --verify-vt host\fixtures\vt\banner.vt
& $py tools\cdk.py type "hello\n"           # テキストを入力として流す
& $py tools\cdk.py key ctrl+c up f1         # キーイベントを送る
& $py tools\cdk.py push app.wasm apps/app.wasm   # PC のファイルを microSD へ書く
& $py tools\cdk.py vt host\fixtures\vt\banner.vt # 端末を初期化してバイト列を流す
& $py tools\cdk.py cam out.jpg              # パネルをカメラで撮る
& $py tools\cdk.py selftest                 # boot + ping + vt(banner) + shot --verify-vt
```

`shot --verify` / `--verify-vt` と `selftest` は基準画像を `build-host/cdk_render` で描くので、
先に上の「ホスト側」をビルドしておくこと。

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
  cdk_shell/         行編集・履歴・補完・コマンド実行
  cdk_skk/           SKK 日本語入力（ローマ字かな・辞書・学習）
  cdk_edit/          テキストエディタ（折り返し・移動・編集）
  cdk_hid/           USB HID ブートキーボードのレポート → キーイベント
  cdk_status/        ステータス帯の文字列
  cdk_retry/         再接続の指数バックオフ
  cdk_sshkey/        OpenSSH 形式の公開鍵行・指紋
  cdk_wasm/          ユーザーアプリ ABI の中身
components_port/   ESP-IDF 依存の薄いポート層
  port_board/        基板ごとのピンと能力（cdk_board.h）
  port_display/      キャンバスをパネルへ転送（CoreS3: BSP / v1: ILI9488）
  port_link/         制御チャネル（CoreS3: USB-Serial-JTAG / v1: UART1）
  port_usbhid/       USB キーボード（v1）
  port_cfg/          NVS に置く設定（認証情報）
  port_net/          WiFi + SSH クライアント + known_hosts 検証 + scp / wget
  port_periph/       GPIO / ADC / I2C / UART
  port_skk/          辞書をフラッシュ / SD から読む口
  port_spibus/       SPI の所有権（CoreS3 は G35 を LCD DC と SD MISO で切り替える）
  port_storage/      microSD (FATFS/SDSPI)
  port_wasm/         .wasm を専用タスクで走らせる
  libssh2/           ベンダリングした libssh2 1.11.1
  wasm3/             ベンダリングした wasm3
main/              起動・メインループ・シェルのコマンド（cdk_*cmds.c）
host/              ホスト側 CMake / ctest / 基準レンダラ / フィクスチャ
wasm-sdk/          ユーザーアプリの SDK（C）
  rust/              同じ ABI の Rust クレート cdk-app + サンプル
wasm-apps/         サンプルアプリ（C）
tools/             PC 側スクリプト（cdk.py / gen_secrets.py / mkfont.py / skk_build.py /
                   wasmcc.py / wasmrs.py / wasminfo.py / check_board_pins.py /
                   check_banner_width.py）
```

## SKK 辞書

辞書はリポジトリに入れない（4MB あり、ライセンスも別）。
初回だけ落としてフラッシュへ焼く:

```powershell
curl -O https://skk-dev.github.io/dict/SKK-JISYO.L.unannotated.gz
gzip -d SKK-JISYO.L.unannotated.gz
python tools/cdk.py dict SKK-JISYO.L.unannotated   # バイナリ化 → skkdict へ書き込み
```

**注釈なしの版を使う。** 語彙は注釈つきの `SKK-JISYO.L` と同じ
（送りあり 15,996 / 送りなし 159,795）で、候補の説明文が付かないだけ。
バイナリが 6.86MB → 6.32MB になる。

`skkdict` パーティション（8MB）に収まる。焼き先はビルド済みの
パーティション表から引くので、オフセットを指定しなくてよい。
注釈つきの辞書を焼きたければ `SKK-JISYO.L` をそのまま渡せばよい
（`skkdict` コマンドで注釈が出るようになる）。

SD の `/sdcard/skk/SKK-JISYO.bin` があればそちらが優先される。
学習は `/sdcard/skk/user.dic` に追記される。

使い方は `skk` コマンドか `Ctrl-J`。詳細は DESIGN.md §9。

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
  さらに LCD の DC が MISO と同じ G35。排他だけでは足りず、
  所有者が変わるたびにピンの向きを変える（`port_spibus`）。
- **FATFS の既定は長いファイル名が無効**（`CONFIG_FATFS_LFN_NONE`）。
  8.3 を超える名前は `fopen` が失敗する。`known_hosts` が開けず、
  libssh2 からは `rc=-16` としか見えなかった。
  見分け方は `ls`: 実ファイルが `LICENC~1.BRO` のように出ていれば LFN が無効。
- ブート時の `i2c.master: Please check pull-up resistances` は CoreS3 では正常。
- **フォントのビット並びは「バイトの MSB が左端」**。BDF は幅ぴったりのビット列で
  持っているので、ブロブに書くときに左詰めし直す必要がある。
  ずれると 2 ビットぶん右に寄って右端の列が消える。
- **JIS X 0208 は ° ± £ など Latin-1 の文字も全角で持っている。**
  半角版を全角版で上書きすると、その文字だけ げた になる。
