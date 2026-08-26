# Cyberdeck Firmware — 設計メモ

ABOUT.md の要求を実装可能な形に落としたもの。**この文書が実装の唯一の正**とし、
決定が変わったらここを更新する。

最終更新: 2026-08-27

---

## 0. 確定事項

ABOUT.md の「決めてほしいこと」と、2026-08-27 のヒアリングで確定した内容。

| 項目 | 決定 |
|---|---|
| 言語 / SDK | C11 + ESP-IDF v5.5.5 (`C:\DATA-0\Espressif\esp-idf-v5.5.5`) |
| ターゲット | `esp32s3` |
| 試作機 | M5Stack CoreS3 (COM22 に接続確認済み。ESP32-S3 rev v0.1 / 16MB flash / USB-Serial-JTAG) |
| 最終ハード液晶 | 480x320 SPI |
| 端末セル | 半角 6x12 / 全角 12x12 → **80桁 x 26行** |
| フォント | 東雲フォント 12px (BDF, Public Domain) |
| 試作機の入力 | PC からシリアル経由でキーイベントを注入 |
| 開発優先順位 | M0 基盤 → **M1 VT+コンソール → M2 WiFi+SSH** → M3 シェル/周辺 → M4 SD → M5 SKK → M6 エディタ |
| ワープロ | microSD 上のプレーンテキストを編集する簡易エディタ (SKK 併用) |
| GUI ライブラリ | 使わない。テキストグリッドを自前描画（LVGL は依存とサイズが見合わない） |

### 画面レイアウト (480x320)

```
480 / 6  = 80 桁
320 / 12 = 26 行 = 312px、余り 8px を最下段のステータス帯に使う

+---------------------------------------------------+ y=0
| テキストグリッド 80 x 26 (6x12 セル)              |
|                                                   |
+---------------------------------------------------+ y=312
| ステータス帯 8px: WiFi / SKKモード / SD / 電池    |
+---------------------------------------------------+ y=320
```

試作機 CoreS3 は 320x240 なので同じフォントで **53桁 x 20行**。
桁数はコンパイル時定数ではなく実行時にパネル寸法から算出する（両方で同じバイナリ設計が動くこと）。

---

## 1. 最重要の設計方針: ホスト検証可能なコア

ABOUT.md 「パソコン上で検証できるところは積極的にパソコン上で検証する」を
アーキテクチャ要件として扱う。

**ESP-IDF のヘッダを一切 include しない純 C のコア**と、**薄いポート層**に分ける。

ホスト側でユニットテストするもの（＝ `esp_*` / `driver/` に依存してはいけない）:

- VT/ANSI エスケープパーサと画面バッファ
- UTF-8 デコーダ + East Asian Width（全角判定）
- グリフラスタライザ（テキストグリッド → RGB565 キャンバス）
- SKK 変換エンジン（ローマ字かな変換、辞書検索、送り仮名処理）
- 行編集・ヒストリ、シェルのコマンドラインパーサ

ポート層に押し込むもの:

- `esp_lcd` / SPI、WiFi、SDMMC/SDSPI、GPIO/I2C/ADC/UART、FreeRTOS、NVS、USB

この境界を最初のコミットで作る。VT パーサに `ESP_LOGI` が生えてから分離するのは書き直しになる。

---

## 2. ディレクトリ構成

```
cyberdeck_firmware/
├── CMakeLists.txt              # ESP-IDF プロジェクト
├── sdkconfig.defaults
├── partitions.csv
├── DESIGN.md                   # この文書
├── main/
│   ├── CMakeLists.txt
│   └── main.c                  # 起動・タスク構成のみ。ロジックを置かない
├── components/                 # ── 純 C コア（ホストでもビルドする）──
│   ├── cdk_proto/              # ホストPC↔デバイスの制御フレーミング
│   ├── cdk_text/               # UTF-8 / 東アジア文字幅
│   ├── cdk_font/               # グリフテーブル参照 + ラスタライズ
│   ├── cdk_canvas/             # RGB565 キャンバス、ダーティ行管理
│   ├── cdk_term/               # VT パーサ + 画面バッファ
│   ├── cdk_shell/              # コマンドラインパーサ + 組み込みコマンド
│   ├── cdk_skk/                # SKK エンジン + 辞書リーダ
│   └── cdk_edit/               # テキストエディタのバッファ/編集ロジック
├── components_port/            # ── ESP-IDF 依存の薄いポート層 ──
│   ├── port_display/           # esp_lcd + BSP、キャンバスをパネルへ転送
│   ├── port_input/             # シリアル注入 / (将来)USB HID → キーイベント
│   ├── port_link/              # cdk_proto をシリアルに載せる
│   ├── port_net/               # WiFi + SSH クライアント
│   ├── port_storage/           # microSD (FATFS) + NVS
│   └── port_periph/            # GPIO/I2C/ADC/UART の実操作
├── host/                       # ── ホスト側ビルド ──
│   ├── CMakeLists.txt          # components/ をそのまま取り込む
│   ├── tests/                  # ユニットテスト（ctest）
│   └── fixtures/               # 期待画面のスナップショット、PNG 期待値
└── tools/                      # ── PC 側スクリプト（Python）──
    ├── cdk.py                  # デバイス制御 CLI（後述）
    ├── bdf2c.py                # 東雲BDF → グリフテーブル C ソース
    ├── skk_build.py            # SKK-JISYO.L → バイナリ辞書
    └── raw2png.py              # スクリーンショット RAW → PNG
```

ビルド入口は 2 つ:

```
# デバイス
idf.py set-target esp32s3 && idf.py build flash

# ホスト（clang or MSVC）
cmake -S host -B build-host -G Ninja && cmake --build build-host && ctest --test-dir build-host
```

---

## 3. 開発ループ（AI エージェントが自律的に回せること）

ABOUT.md の「AIエージェントが自律的にコーディング、実行、デバッグできるように」への回答。

### 3.1 ログは必ず有限時間で取れること

`idf.py monitor` は終了しないのでエージェントからは使えない。
`tools/cdk.py` に**タイムアウトとセンチネル付き**のシリアルリーダを実装する。

```
python tools/cdk.py flash             # ビルド + 書き込み + リセット
python tools/cdk.py log --timeout 10  # 10秒 or センチネルまで読んで終了
python tools/cdk.py boot              # リセットして起動ログを取り、READY 到達で終了
```

ファーム側は起動完了時に `#CDK-READY` を出す。これがセンチネル。

### 3.2 キー入力の注入

```
python tools/cdk.py type "ls -la\n"
python tools/cdk.py key ctrl+c
python tools/cdk.py key f1
```

ファーム側では「シリアルから来たキーイベント」と「USB キーボードから来たキーイベント」を
`cdk_key_event_t` に正規化して同じ経路に流す。試作機で書いたテストが実機でもそのまま通る。

### 3.3 スクリーンショット

**パネルから読み戻さない。** SPI 液晶コントローラのリードバックは信頼できないか、そもそも無い。
代わりに PSRAM 上のキャンバスを唯一の真実とし、描画は必ずキャンバス経由で行い、
コマンドでその内容をダンプする。

```
python tools/cdk.py shot out.png      # RAW 取得 → ホストで PNG 化
```

副作用として、ホスト側のユニットテストでも同じキャンバスをレンダリングして
PNG を diff できる（実機なしで表示の回帰テストができる）。

### 3.4 チャネル構成

**既定は 2 チャネル**にする。

| チャネル | 物理 | 用途 |
|---|---|---|
| CTRL | USB-Serial-JTAG (**COM22**) | `cdk_proto` バイナリフレーム専用。ログを一切流さない |
| LOG | UART1 → CoreS3 PORT.C (G17=TX / G18=RX) → FT232 (**COM75**) | `ESP_LOG` のテキスト出力専用 |

FT232 は 2026-08-27 に接続確認済み（`FTDIBUS\VID_0403+PID_6001`）。

理由: 1 本に多重化すると、ROM ブートローダのチャタリング、パニック時のバックトレース、
`ESP_LOG` に混じるバイナリなど**こちらが制御できない出力**がフレームの中に割り込む。
デバッグチャネル自体が信用できなくなると、以降の全マイルストーンが遅くなる。
配線 3 本（TX/RX/GND）で問題のクラスごと消せるので、そちらを取る。

CH340 を繋げない状況のために **1 チャネル縮退モード**も持つ（`CDK_LINK_SINGLE`）。
そのときは CTRL と LOG が COM22 に同居する。フレーム形式は縮退モード前提で設計しておく:

```
magic(4) = 0x1B 'C' 'D' 'K'   ESC で始めるのでテキスト行として解釈されない
type(1) len(4,le) payload(len) crc32(4,le)
```

ホスト側リーダは**再同期する**こと。すなわち magic を見つけてもフレームを信用せず、
CRC が合わなければ 1 バイト捨てて magic を探し直す。長さフィールドを盲信して
その分読み飛ばす実装にしない（同期外れが一度起きたら復帰できなくなる）。

実装は `components/cdk_proto/`（ホストとデバイスで同一ソースを共有）。

---

## 4. 表示

### 4.1 パネル

CoreS3 の LCD (ILI9342C) はバックライトと周辺電源が **AXP2101 PMIC と AW9523 I/O エキスパンダ経由**で
ゲートされている。この初期化順序を外すと画面は真っ黒のまま無言で失敗する。
ピンマップと電源投入シーケンスは自前で書かず、公式 BSP から取る。

```
idf.py add-dependency "espressif/m5stack_core_s3^2.0.0"
```

最終ハードは BSP を持たないので、`port_display` は
`bsp_*` を直接呼ばず `cdk_display_init()` / `cdk_display_blit()` の裏に隠す。

### 4.2 キャンバスと転送

- キャンバス: `RGB565`、PSRAM 上。480x320 で 300KB、320x240 で 150KB。
- 描画はすべてキャンバスに対して行う。
- **行単位のダーティフラグ**を持ち、変更のあった行帯だけ `esp_lcd_panel_draw_bitmap` で転送する。
  端末は全画面書き換えが少ないので、これで SPI 帯域はまず問題にならない。
- 転送は DMA、`esp_lcd` の `on_color_trans_done` で二重バッファ。

### 4.3 テキストグリッド → ピクセル

セルは (文字コード, 前景色, 背景色, 属性) を持つ。属性は bold / reverse / underline / 全角右半分。
再描画はダーティセルのみ 6x12 (or 12x12) を書き込む。

---

## 5. フォント

### 5.1 東雲 12px

- ライセンス: **Public Domain**
- 収録: JIS X 0201 + JIS X 0208 (+ ISO 8859-1)
- 寸法: 半角 6x12 / 全角 12x12
- 形式: BDF

`tools/bdf2c.py` で BDF → C 配列に変換し、**フラッシュの rodata に焼き込む**（SD 不要）。

| データ | サイズ |
|---|---|
| 半角 6x12 × 約 256 字 (12 バイト/字) | 約 3 KB |
| 全角 12x12 × 約 6,879 字 (24 バイト/字) | 約 165 KB |
| Unicode → グリフ番号 索引 (u16 ソート配列 + 二分探索) | 約 14 KB |
| **合計** | **約 185 KB** |

16MB フラッシュに対して無視できる。辞書と違って SD 依存にしない。

### 5.2 収録外文字の扱い（要注意）

東雲は JIS X 0208 の範囲しか持たない。CLI が使う文字で足りないものがある。

- **罫線素片 (U+2500〜U+257F)**: JIS X 0208 にある分は対応表でマップ。
  太線・二重線など JIS に無い字形は **自前で 12x12 グリフを追加**する（数十字なので手描きで足りる）。
- **ブロック要素 (U+2580〜U+259F)**: プログラム生成で追加（塗り分けだけなので自動生成できる）。
- **Powerline / Nerd Font 記号 (U+E0A0〜, U+F000〜)**: **非対応**。
  → 代替: 端末側で近似 ASCII に置換するテーブルを持つ。
  → もしくは SSH 先の設定でこれらを使わないプロンプトにしてもらう。
- **絵文字**: 非対応。`〓`（げた）で表示し、桁数だけは全角 2 セルとして正しく数える。

「表示できないこと」より「桁がずれること」の方が端末としては致命的なので、
**幅計算は必ず Unicode East Asian Width に従い、グリフの有無と切り離す**。

---

## 6. ターミナルエミュレータ

### 6.1 パーサ

Paul Williams の DEC ANSI 状態機械をそのまま実装する（GROUND / ESCAPE / CSI_* / OSC_* / DCS_*）。
未知のシーケンスは黙って捨てる（画面を壊さないことが最優先）。

### 6.2 対応する制御

ABOUT.md のリストをそのまま実装可能な粒度に展開したもの。

| 分類 | シーケンス |
|---|---|
| C0 | `BEL LF CR BS HT`（`VT` `FF` は LF 扱い） |
| カーソル移動 | `CUU CUD CUF CUB` (`CSI A/B/C/D`), `CNL CPL` (`E/F`), `CHA` (`G`), `CUP` (`H` / `f`), `VPA` (`d`) |
| 画面消去 | `ED` (`CSI J`) 0/1/2/3 |
| 行消去 | `EL` (`CSI K`) 0/1/2 |
| 文字操作 | `ICH IL DL DCH ECH` (`@ L M P X`) — CLI の行再描画で必須 |
| SGR | 0,1,2,4,7,22,24,27, 30-37, 40-47, 90-97, 100-107, `38;5;n` / `48;5;n`（256色）, `38;2;r;g;b`（truecolor → RGB565 に丸め） |
| カーソル保存/復元 | `DECSC` / `DECRC` (`ESC 7` / `ESC 8`), `SCOSC` / `SCORC` (`CSI s` / `CSI u`) |
| スクロール | `DECSTBM` (`CSI r`), `SU` / `SD` (`CSI S` / `CSI T`), `RI` (`ESC M`) |
| モード | `DECTCEM`(25: カーソル表示), `1049` / `47`（代替画面）, `2004`（bracketed paste）, `1000` / `1002` / `1006`（マウス。**受理して無視**） |
| 問い合わせ | `DA` (`CSI c` → `ESC[?6c`), `DSR` (`CSI 6n` → カーソル位置) |
| OSC | `0` / `2`（タイトル → ステータス帯に出す）。それ以外は読み捨て |
| 文字集合 | `ESC ( B` のみ受理。DEC 特殊グラフィクス `ESC ( 0` は罫線にマップ |

**省略するもの**: DCS / Sixel、ReGIS、ソフトフォント (DECDLD)、二重幅行 (DECDWL)、
プリンタ制御、Shift_JIS の直接受理。CLI 用途で必要にならない。

### 6.3 端末種別

`TERM=xterm-256color` を名乗る。上記に無いシーケンスが来ても壊れないことを、
ホスト側テストで実際の CLI 出力キャプチャを流して確認する。

### 6.4 ホスト側テスト

```
host/fixtures/vt/NNN_name.in    # 入力バイト列
host/fixtures/vt/NNN_name.txt   # 期待するグリッド（テキスト + 属性）
```

`vttest` の出力や実際の `claude` / `codex` CLI のログを流し込んで回帰させる。

---

## 7. 入力

### 7.1 キーイベント

```c
typedef struct {
    uint16_t keycode;   // HID usage 相当
    uint8_t  mods;      // ctrl / shift / alt / gui
    uint32_t codepoint; // 文字が確定するなら Unicode、無ければ 0
} cdk_key_event_t;
```

入力源（シリアル注入 / USB HID / 将来の BLE）はすべてこの型に正規化してキューへ。

### 7.2 レイヤ構成

```
入力源 → キーイベント → [SKK レイヤ] → [端末エンコーダ] → バイト列 → SSH / シェル / エディタ
```

SKK が OFF のときはレイヤを素通りする。端末エンコーダは
カーソルキー → `ESC[A`、Ctrl+文字 → 制御コード、Alt+文字 → `ESC` + 文字、
テンキー、F1-F12 (`ESC[11~` 系) を担当。

### 7.3 USB キーボード（最終ハード）

**ESP32-S3 の USB PHY は 1 つしか無い。** USB ホスト（キーボード）と USB-CDC コンソールは
同時に使えない。したがって最終ハードでは:

- USB = ホスト固定（キーボード用）
- デバッグコンソール = UART に出す（ピンヘッダを必ず出しておくこと）

試作段階では USB = デバイス（USB-Serial-JTAG）のまま。
コンソール出力先は `port_link` の背後に抽象化し、ビルド時に切り替える。

内蔵 USB ホストは `usb/usb_host` + HID クラスドライバ（`espressif/usb_host_hid`）を使う。
バスパワー給電が必要なので、最終ハードには 5V を VBUS に流す回路が要る（電源設計の宿題）。

### 7.4 キー配列

US 配列と JIS 配列の両方の変換表を持ち、実行時に切り替える（`set keymap jis`）。
どちらか一方に決め打ちしない。

---

## 8. ネットワークと SSH

### 8.1 WiFi

`esp_wifi` STA モード。認証情報は NVS に保存し、シェルの `wifi` コマンドで設定。
**ソースにも sdkconfig にも書かない**（ABOUT.md に平文で載っている PSK / SSH パスワードは
テスト用と理解しているが、ファームには焼かない）。

### 8.2 SSH クライアント — 最大の不確実性

ESP-IDF 上で PTY 付きの対話 SSH セッションが張れるかがプロジェクトの成否を決める。
**M2 で最優先に潰す。**

| 候補 | ライセンス | 備考 |
|---|---|---|
| **wolfSSH + wolfSSL**（第一候補） | GPLv3 or 商用 | `wolfssl/wolfssh` 1.4.15 がコンポーネントレジストリにあり `idf.py add-dependency` 一発。ESP32 のハード暗号アクセラレータ対応も入っている |
| libssh2 | BSD-3 | mbedTLS バックエンドあり。ESP-IDF の component 化は手作業 |
| 自前実装 | — | 現実的でない |

**GPLv3 は本プロジェクトでは問題にしない**という判断（2026-08-27）。
導入コストが最も低い wolfSSH を第一候補とする。
wolfSSH でクライアント PTY が期待通り動かなければ libssh2 に切り替える。

必要なもの:

- 鍵交換: `curve25519-sha256`（mbedTLS で対応）
- ホスト鍵: `ssh-ed25519` / `rsa-sha2-256`
- 暗号: `aes128-ctr` / `chacha20-poly1305`（ハード AES アクセラレータが効く前者を優先）
- PTY 要求で `xterm-256color` と 80x26 を渡すこと
- ウィンドウサイズ変更 (`window-change`) の通知
- ノンブロッキング I/O でメインループに組み込む

RAM 見積り: 40〜60KB 程度。PSRAM に逃がせるので問題にならない見込み。

**ホスト鍵検証**: `known_hosts` を SD に置く。初回は指紋を表示して確認を求める。

**リスク**: mbedTLS のバージョン差で libssh2 のバックエンドが噛み合わない可能性。
M2 の最初の 1 日でビルドが通らなければ wolfSSH に切り替える（ライセンスは要相談）。

---

## 9. SKK 日本語入力

### 9.1 辞書

SKK-JISYO.L は EUC-JP のテキストで数MB〜十数MB規模。デバイス上でパースしない。

`tools/skk_build.py` で PC 上でバイナリ化する:

```
[header]           magic "SKKD", version, okuri_ari_count, okuri_nasi_count, offsets
[okuri-ari  索引]  u32 key_offset[]     ← 見出し語をソート順に
[okuri-nasi 索引]  u32 key_offset[]
[key blob]         UTF-8 の見出し語 + u32 候補オフセット
[cand blob]        候補列（'/' 区切り、注釈付き）
```

- **送り仮名あり / なしを別テーブルに分ける。** ここを一緒くたにすると送り仮名変換が静かに壊れる。
- デバイスは索引を二分探索（`fseek` + `fread` のみ、RAM 使用量は一定）。
- 索引だけ（数百KB〜1MB）は起動時に PSRAM へ載せると検索が速い。

**配置**:

1. microSD `/sdcard/skk/SKK-JISYO.bin`（本命。辞書の差し替えが容易）
2. フラッシュの `skkdict` パーティション（SD 無しでも動く縮小版 = SKK-JISYO.M 相当）

`skk_build.py` は入力辞書を選べるようにして、「あとで差し替えられるように」という要求を満たす。

### 9.2 エンジン

- ローマ字かな変換表（`ddskk` の `skk-rom-kana-base-rule-list` 相当をデータとして持つ）
- モード: `かな` / `カナ` / `半角カナ` / `全英` / `abbrev` / `▽`(見出し入力) / `▼`(候補選択)
- 送り仮名: `▽おく*R` → 見出し `おくr` + 送り `る` で okuri-ari を引く
- 個人辞書 `/sdcard/skk/user.dic`（追記のみ、確定時に学習）
- 数値変換 `#`、注釈表示は後回しで可

エンジンは純 C・状態機械。**入力文字列 → 出力文字列 + 表示状態** の関数としてホストで全テストする。

---

## 10. シェルと周辺操作

ESP-IDF の `console` コンポーネント（`esp_console` + linenoise）は端末を直接握るので使わない。
コマンドライン編集は `cdk_shell` に自前実装し、入出力は端末バッファ経由にする
（SSH セッションと同じ土俵に乗せるため）。

コマンド案:

```
help / echo / clear / history
ls cat rm cp mv mkdir df           ファイル (microSD)
edit <file>                        テキストエディタ
wifi scan | connect <ssid> | status
ssh <user>@<host> [-p port]
gpio get|set|mode <pin> [value]
i2c scan [bus] | i2c read <addr> <reg> <len> | i2c write <addr> <reg> <data...>
adc read <ch>
uart open <tx> <rx> <baud> | uart send <hex|str> | uart monitor
pwm <pin> <freq> <duty>
skk on|off|reload
set <key> <value>                  keymap, tabwidth, ...
shot [path]                        SD にスクリーンショット保存
reboot / info / free
```

`uart monitor` は端末を占有するので Ctrl-] で抜ける。

---

## 11. ストレージ

| 用途 | 場所 |
|---|---|
| WiFi 認証情報、設定 | NVS |
| SKK 辞書、個人辞書 | microSD `/sdcard/skk/` |
| known_hosts、SSH 鍵 | microSD `/sdcard/ssh/` |
| テキストファイル | microSD 任意 |
| フォント | フラッシュ rodata（SD 不要） |

CoreS3 の microSD は LCD と SPI バスを共有する。同一バスの排他制御が要る
（`esp_lcd` の SPI と `sdspi` を同じホストに載せ、トランザクション単位で共存させる）。
**LCD 転送中に SD アクセスが割り込むと画面が壊れる**ので、
キャンバス転送タスクと SD アクセスをミューテックスで直列化する。

パーティション（16MB）。`gen_esp32part.py --flash-size 16MB` で検証済み:

```
# Name,   Type, SubType, Offset,  Size
nvs,      data, nvs,     ,        24K     # 0x009000
phy_init, data, phy,     ,        4K      # 0x00f000
factory,  app,  factory, ,        4M      # 0x010000
skkdict,  data, 0x40,    ,        4M      # 0x410000
storage,  data, fat,     ,        4M      # 0x810000
                                          # 0xc10000 以降 約4MB 空き
```

`skkdict` は SD 無しでも SKK が使えるようにするための予備領域。
SD が本命なので、不要なら削って `storage` か OTA に回せる（M5 までに判断すれば良い）。
OTA は当面不要（`factory` 単独）。必要になったら空き 4MB を `ota_1` にする。

---

## 12. 実装困難・不能な箇所と代替案

ABOUT.md 「実装困難・不能な箇所についての指摘と代替案」への回答。

| # | 問題 | 影響 | 代替案 |
|---|---|---|---|
| 1 | **80桁の確保** | 480x320 で 8px フォントだと 60桁しか出ず CLI の UI が崩れる | 東雲 12px（半角 6x12）を採用して 80桁 x 26行 を確保。**解決済み** |
| 2 | **26行しか無い** | Claude Code / Codex CLI は縦にも長い。ページャや差分表示が窮屈 | 回避不能。SSH 側に `LINES=26` を正しく伝え、CLI 側の折返しに任せる。フォント 12px 固定のまま縦だけ稼ぐ手段は無い |
| 3 | **USB ホストと USB コンソールの排他** | 最終ハードで USB キーボードを挿すと USB 経由のログが取れない | UART をデバッグ用に常設。試作機は USB デバイスのまま |
| 4 | **Nerd Font / Powerline / 絵文字** | 今どきの CLI プロンプトが化ける | 近似 ASCII 置換テーブル + 幅計算だけは正しく行う。表示は諦める |
| 5 | **SSH ライブラリが ESP-IDF で素直に組めない可能性** | プロジェクトの根幹 | wolfSSH を第一候補（GPLv3 は許容と判断済み）、libssh2 を第二候補。M2 冒頭でスパイクして早期に判定 |
| 6 | **SKK-JISYO.L が実機で扱えない** | 変換不能 | PC 上でバイナリ索引化し、SD から二分探索。RAM 一定 |
| 7 | **LCD と microSD の SPI バス共有 (CoreS3)** | 画面破壊・SD エラー | 転送を排他制御。最終ハードでは SD を SDMMC で別バスにする設計を推奨 |
| 8 | **SPI 液晶のリードバック不可** | スクリーンショットが取れない | PSRAM キャンバスを真実とし、描画を必ず経由させてダンプ |
| 9 | **フル画面書き換え時の SPI 帯域** | スクロールが遅い | 行ダーティ管理 + ハードウェア縦スクロール（`DECSTBM` を `esp_lcd` の vertical scroll に対応付ける最適化を M1 後に検討） |
| 10 | **truecolor → RGB565** | 色が丸まる | 仕様として許容。SGR は受理し最近傍に丸める |
| 11 | **日本語 IME の候補表示領域** | 26行のうち 1〜2行を食う | 候補はステータス帯直上に 1 行のオーバーレイで出し、端末バッファは汚さない |
| 12 | **ABOUT.md に生の PSK / SSH パスワードがコミットされている** | 資格情報の漏洩 | ファームには焼かない。テスト後にローテートし、資料からは伏せ字にすることを推奨 |

---

## 13. マイルストーン

各マイルストーンの完了条件は「エージェントがコマンド一発で検証できること」。

### M0 — 基盤（最優先）

- ESP-IDF プロジェクト骨格、`components/` と `components_port/` の分離
- `host/` の CMake + ctest が通る
- CoreS3 BSP で LCD 点灯、PSRAM キャンバス確保、描画
- `cdk_proto` フレーミング + `tools/cdk.py` の `flash` / `log` / `shot` / `type`
- **完了条件**: `python tools/cdk.py shot out.png` で実機の画面が PNG で取れる

### M1 — ターミナル

- `cdk_text`（UTF-8 + 幅）、`cdk_font`（東雲 12px 焼き込み）、`cdk_canvas`
- `cdk_term` VT パーサ（§6.2 の全部）
- 画面へのレンダリング、ダーティ行転送
- ホスト側 VT 回帰テスト一式
- **完了条件**: `tools/cdk.py type` で流したエスケープ列が期待通り描画され、ホストテストが緑

### M2 — WiFi + SSH（不確実性が最大。早く潰す）

- WiFi STA 接続、NVS 保存
- libssh2 スパイク → PTY + シェル + ウィンドウサイズ通知
- テストサーバへ接続して `vim` / `top` が壊れずに動くこと
- **完了条件**: 実機から SSH で CLI を起動し、80x26 で操作できる（or 不可の判定と報告）

### M3 — シェルと周辺操作

- `cdk_shell` 行編集・ヒストリ・パーサ
- `gpio` / `i2c` / `adc` / `uart` / `pwm` コマンド

### M4 — ストレージ

- microSD マウント、SPI バス排他
- ファイル系コマンド、`shot` の SD 保存

### M5 — SKK

- `tools/skk_build.py`、ローマ字かな、辞書検索、送り仮名、候補 UI、個人辞書

### M6 — エディタ（ワープロ）

- `edit` コマンド。SD 上のプレーンテキストを SKK 付きで編集

### M7 — 最終ハード移行

- USB HID ホスト、UART コンソール、480x320 パネル、電源

---

## 14. コーディング規約

- C11、`snake_case`、公開シンボルは `cdk_` プレフィクス
- ファイルは UTF-8（BOM なし）、LF
- `components/` では `assert` と戻り値エラーコード。`ESP_LOG` / `ESP_ERROR_CHECK` は禁止
- `components_port/` では `esp_err_t` を返し `ESP_LOGx` を使う
- 動的確保は起動時のみを原則とする。端末バッファ・キャンバス・辞書索引は起動時に確保して以後固定
