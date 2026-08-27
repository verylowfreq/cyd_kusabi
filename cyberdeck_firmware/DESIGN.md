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
│   ├── cdk_proto/              # ホストPC↔デバイスの制御フレーミング       [M0]
│   ├── cdk_text/               # UTF-8 / 東アジア文字幅                     [M1]
│   ├── cdk_canvas/             # RGB565 キャンバス、ダーティ行管理          [M0]
│   ├── cdk_font/               # フォントブロブ + shinonome12.cdkfont       [M1]
│   ├── cdk_term/               # VT パーサ + 画面バッファ                   [M1]
│   ├── cdk_keys/               # キーイベント → 端末が期待するバイト列      [M1]
│   ├── cdk_screen/             # セルグリッド → キャンバス描画              [M1]
│   ├── cdk_shell/              # コマンドラインパーサ + 組み込みコマンド    [M3]
│   ├── cdk_skk/                # SKK エンジン + 辞書リーダ                  [M5]
│   └── cdk_edit/               # テキストエディタのバッファ/編集ロジック    [M6]
├── components_port/            # ── ESP-IDF 依存の薄いポート層 ──
│   ├── port_display/           # esp_lcd + BSP、キャンバスをパネルへ転送    [M0]
│   ├── port_link/              # cdk_proto を USB-Serial-JTAG に載せる      [M0]
│   ├── port_input/             # USB HID → キーイベント                     [M7]
│   ├── port_net/               # WiFi + SSH クライアント                    [M2]
│   ├── port_storage/           # microSD (FATFS) + NVS                      [M4]
│   └── port_periph/            # GPIO/I2C/ADC/UART の実操作                 [M3]
├── host/                       # ── ホスト側ビルド ──
│   ├── CMakeLists.txt          # components/ をそのまま取り込む
│   ├── tests/                  # ユニットテスト（ctest）
│   ├── tools/cdk_render.c      # 基準レンダラ（実機の検証にも使う）
│   └── fixtures/vt/            # VT フィクスチャ。実機とホストが同じものを描く
└── tools/                      # ── PC 側スクリプト（Python）──
    ├── cdk.py                  # デバイス制御 CLI（後述）
    ├── mkfont.py               # 東雲BDF → フォントブロブ
    └── skk_build.py            # SKK-JISYO.L → バイナリ辞書   [M5]
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

M0 で踏んだ罠: **`usb_serial_jtag_write_bytes()` は TX リングバッファより
大きい書き込みができない。** 内部で丸ごとリングに積むので、リングを超える要求は
何度呼んでも 0 を返し続ける。`port_link` では 512 バイトずつに分割して渡している。

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
`bsp_*` を直接呼ばず `cdk_display_init()` / `cdk_display_flush()` の裏に隠す。

M0 で実測した BSP の性質:

- `bsp_display_new()` が AXP2101 / AW9523 の電源投入、SPI3 初期化、
  ILI9342C の reset/init/invert までやってくれる。LVGL は不要。
- ただし `esp_lcd_panel_disp_on_off(panel, true)`、`bsp_display_brightness_init()`、
  `bsp_display_backlight_on()` は自分で呼ぶ必要がある。
- 回転・反転は不要（`swap_xy` / `mirror_x` / `mirror_y` すべて false）。
- **`BSP_LCD_BIGENDIAN = 1`**。パネルはビッグエンディアンの RGB565 を期待する。
  キャンバスはネイティブ（リトルエンディアン）なので転送時に `__builtin_bswap16` する。
- **PSRAM は quad**。`CONFIG_SPIRAM_MODE_OCT=y` にすると
  `PSRAM chip is not connected, or wrong PSRAM line mode` で起動時に abort する。
- 起動時の `i2c.master: Please check pull-up resistances` は CoreS3 では正常。

### 4.2 キャンバスと転送

- キャンバス: `RGB565`、PSRAM 上。480x320 で 300KB、320x240 で 150KB。
- 描画はすべてキャンバスに対して行う。
- **行単位のダーティ**を持ち、変更のあった行帯だけ `esp_lcd_panel_draw_bitmap` で転送する。
  端末は全画面書き換えが少ないので、これで SPI 帯域はまず問題にならない。

> 注意: キャンバスのダーティは**行ごとのビットではなく min/max の 1 本の帯**。
> カーソルが 1 行目から最終行へ飛ぶと、間の行が無傷でも全画面を転送する。
> 320x240 で 150KB、40MHz SPI で約 30ms + バイトスワップ。
> M1 では問題にならないが、SSH で対話していると効いてくる。
> 実際に体感したら行ごと、あるいは連続する行の塊ごとの管理に変えること。
> それまでは前倒しで最適化しない。
- 転送は DMA。**バウンスバッファを 2 枚用意して交互に使う**。
  `esp_lcd_panel_draw_bitmap` の色データは非同期だが、次の呼び出しが
  CASET/RASET コマンドを送る前に in-flight の転送を必ず回収する
  （`esp_lcd_panel_io_spi` の実装）。したがって交互に使えば、
  これから書き込もうとしているバッファの転送は必ず完了している。
  `on_color_trans_done` の登録は不要（BSP が作った io には登録できない）。
- ILI9342C はビッグエンディアンの RGB565 を期待するので、
  バウンスバッファへ写すときに `__builtin_bswap16` する。

### 4.3 テキストグリッド → ピクセル

セルは (文字コード, 前景色, 背景色, 属性) を持つ。属性は bold / reverse / underline / 全角右半分。
再描画はダーティセルのみ 6x12 (or 12x12) を書き込む。

---

## 5. フォント

### 5.1 東雲 12px

- ライセンス: **Public Domain**
- 入手: `http://openlab.ring.gr.jp/efont/dist/shinonome/shinonome-0.9.11p1.tar.bz2`
  （**https は繋がらない**。http で取ること）
- 使うファイル:

| ファイル | 収録 | 寸法 | 割り当て |
|---|---|---|---|
| `shnm6x12a.bdf` | ISO 8859-1 | 6x12 | U+0020..U+00FF |
| `shnm6x12r.bdf` | JIS X 0201 | 6x12 | U+FF61..U+FF9F（半角カナ） |
| `shnmk12.bdf` | JIS X 0208 | 12x12 | euc_jp 経由で Unicode へ |

`tools/mkfont.py` が BDF を読んでバイナリブロブ
`components/cdk_font/shinonome12.cdkfont` を作る。
**ブロブはリポジトリにコミットする**（BDF アーカイブは 4.6MB あるので入れない）。
ブロブは `EMBED_FILES` でフラッシュに焼き込む。SD もファイルシステムも要らない。

```
python tools/mkfont.py <shinonome-0.9.11/bdf> -o components/cdk_font/shinonome12.cdkfont
```

実測 (2026-08-27):

| データ | サイズ |
|---|---|
| グリフ 7,269 字（漢字 6,868 / 罫線 128 / ブロック 32 / 記号 43） | — |
| 索引 (u16 コードポイント + u32 エントリ) | 42.6 KB |
| ビットマップ | 164 KB |
| **合計** | **207 KB** |

ブロブ形式は `tools/mkfont.py` の docstring が正。
`cdk_font_open()` が全セクションの範囲を検証してから開く。

### 5.2 自前で描いたグリフ

東雲に無い、または全角しか無くて使えないものを `mkfont.py` が生成する。

- **罫線素片 U+2500..U+257F（128 字）**
  JIS X 0208 にも入っているが**全角**。端末は East Asian Width = Ambiguous に従って
  **半角**として数えるので、全角グリフを当てると必ず桁がずれる。
  上下左右の腕の太さ（無し / 細 / 太 / 二重）から 6x12 で組み立てる。
  腕はセルの端まで届かせて、隣のセルと線が繋がるようにする。
  破線は実線、角丸は普通の角で近似。
- **ブロック要素 U+2580..U+259F（32 字）** — 塗り分けなので手続き的に生成。
- **記号 43 字** — `SYMBOLS_SRC` に 6x12 の点図として直接書いてある。
  `─ – — … • ← ↑ → ↓ ⇒ ≠ ≤ ≥ ■ □ ▲ ▼ ◆ ○ ● ★ ✓ ✗ π` など、
  CLI が実際に使うもの。DEC 特殊グラフィクス (`ESC ( 0`) の写像先も全部ここで埋めている。
- **notdef（げた）** — 半角/全角それぞれ。斜線ではなく枠にして空白と区別する。

### 5.3 幅とグリフの不一致を許さない

`cdk_font_glyph(font, cp, cells, &g)` は、見つかったグリフの幅が要求セル数と
違えば **notdef を返す**。グリフがあっても使わせない。
桁がずれるくらいなら げた を出す方がましだから。

この規則の帰結として、Ambiguous 幅でグリフが全角しか無い文字
（ギリシャ文字、キリル文字、丸数字など）は げた になる。
CLI 用途では実害が小さいと判断して M1 では放置する。
将来 `set ambiwidth double` を入れるなら、`cdk_char_width` と
グリフ選択の両方を同時に切り替えること。

### 5.4 まだ出せないもの

- **Powerline / Nerd Font 記号 (U+E0A0〜, U+F000〜)**: 非対応。
  → SSH 先のプロンプト設定でこれらを使わないようにしてもらう。
- **絵文字**: 非対応。げたで表示し、幅だけは全角 2 セルとして正しく数える。
- **ハングル・キリル拡張など**: グリフ無し。幅は正しい。

「表示できないこと」より「桁がずれること」の方が端末としては致命的なので、
**幅計算は必ず Unicode East Asian Width に従い、グリフの有無と切り離す**。

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

`TERM=xterm-256color` を名乗る。`CSI c` (DA) には `ESC[?62;22c`（VT220 相当）を返す。
上記に無いシーケンスが来ても壊れないことを、ホスト側テストで確認する
（`t->stat_unknown` に捨てた数が溜まるので、実機でも異常を検知できる）。

**太字の扱い**: SGR 1 は字を太らせず、前景色を明るい方（パレット 0-7 → 8-15）に振る。
6px 幅で字を太らせても潰れて読めないため。xterm の既定と同じ振る舞い。

### 6.4 ホスト側テスト

```
host/fixtures/vt/NNN_name.in    # 入力バイト列
host/fixtures/vt/NNN_name.txt   # 期待するグリッド（テキスト + 属性）
```

`vttest` の出力や実際の `claude` / `codex` CLI のログを流し込んで回帰させる。

同じフィクスチャを実機の検証にも使う（M1 で実装済み）。

```
cdk_render vt <fixture.vt> <width> <height> <out.raw>
python tools/cdk.py shot out.png --verify-vt host/fixtures/vt/banner.vt
```

実機が描いた画面と、ホストで同じ純 C コードが描いた基準画像をバイト単位で比較する。
起動バナー `host/fixtures/vt/banner.vt` は**ファームにも `EMBED_FILES` で焼き込んで**
いるので、実機とホストが文字通り同じバイト列を食べている。
これで「実機だけで壊れている」ことを機械的に切り分けられる。

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

### 8.2 SSH クライアント — libssh2 に決定 (2026-08-27)

**採用: libssh2 1.11.1 + mbedTLS。** `components_port/libssh2/` にベンダリング済み。
実機で `libssh2_init()` まで通ることを確認済み。

当初は導入コストの低さから wolfSSH を第一候補にしていたが、
実際に調べた結果 **クライアント側 PTY が使えない** ことが分かったので覆した。

#### wolfSSH を採らなかった理由

| # | 事実 | 影響 |
|---|---|---|
| 1 | PTY 要求 `SendChannelTerminalRequest()` が `#if defined(WOLFSSH_TERM) && !defined(NO_FILESYSTEM)` の中にある（`src/internal.c:15995`） | 条件を満たさないと関数ごと消える |
| 2 | ベンダー純正の ESP32 用 `user_settings.h` が **`NO_FILESYSTEM` を定義**している（439 行目） | 既定では PTY 要求がコンパイルされない |
| 3 | 上書き手段は wolfSSL の `user_settings.h` をプロジェクト側で丸ごと抱えることだけ | コンポーネント更新のたびに衝突する |
| 4 | 有効化しても `GetTerminalInfo()` が非 POSIX ターゲットでは **80x24 と "xterm" をハードコード**（`src/internal.c:16231`） | 実機は 53x20。`wolfSSH_ChangeTerminalSize()` で後追い修正が要る |

PTY は「Claude Code / Codex の CLI が動く」という目標に対して譲れない。
`libssh2_channel_request_pty_ex(ch, "xterm-256color", 14, NULL, 0, cols, rows, 0, 0)` は
端末名もサイズも引数で受け取るので、回避策なしで正しい値を渡せる。

GPLv3 の許容は不要になった（libssh2 は BSD-3-Clause）。

#### ベンダリングの方針

コンポーネントレジストリに libssh2 は無いので上流の配布物を取り込む。
`components_port/libssh2/README.md` に版・入手元・更新手順を書いてある。

- ビルド対象は上流 `src/Makefile.inc` の `CSOURCES` のみ。
- **`upstream/src/` からファイルを削らないこと。** `agent.c` は `agent_win.c` を
  **無条件に** `#include` し、`crypto.c` はバックエンドの `.c` を `#include` する。
  中身側で `#ifdef` 分岐しているので、消すとビルドが壊れる。
- 設定は `libssh2_config.h`（`HAVE_CONFIG_H` 経由で `libssh2_setup.h` が読む）。
  **`HAVE_POLL` は定義しないこと。** lwIP / newlib に `poll.h` は無い。`select` を使う。

#### M2 で実装するもの

- 鍵交換 `curve25519-sha256`、ホスト鍵 `ssh-ed25519` / `rsa-sha2-256`
- 暗号 `aes128-ctr`（ESP32-S3 のハード AES が効く）/ `chacha20-poly1305`
- `libssh2_channel_request_pty_ex()` に `cdk_term` の実際の `cols` / `rows` を渡す。
  **80x26 ではなく試作機の 53x20。** 固定値を書かないこと。
- `libssh2_channel_request_pty_size_ex()` で画面サイズ変更を通知
- ノンブロッキング + `select` でメインループに組み込む

**ホスト鍵検証**: `known_hosts` を SD に置く（M4）。それまでは指紋をログに出して先へ進む。

#### タスク構成

SSH の受信は**メインループが `select` で他と一緒にポーリングする**。
専用タスク + キューにはしない。M2 の規模では単純さが勝るため。

この選択は M4 で効いてくる。SD と LCD が SPI3 を共有するので、
キャンバスへの描画と SD アクセスを直列化するミューテックスが要る。
描画がメインループ内で完結していれば、ロックはそこ 1 箇所で済む。

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

### M0 — 基盤 ✅ 完了 (2026-08-27)

- [x] ESP-IDF プロジェクト骨格、`components/` と `components_port/` の分離
- [x] `host/` の CMake + ctest（`test_proto` / `test_canvas` / `no_idf_in_core`）
- [x] CoreS3 BSP で LCD 点灯、PSRAM キャンバス確保、テストパターン描画
- [x] `cdk_proto` フレーミング（再同期つき）+ `port_link`
- [x] `tools/cdk.py` の `build` / `flash` / `log` / `boot` / `ping` / `shot` / `type` / `key`
- [x] **完了条件**: `cdk.py shot out.png --verify` で実機の画面を取得し、
      ホスト側で同じ純 C コードが描いた基準画像と 153,600 バイト完全一致することを確認

実測値: アプリ 279KB / 内部ヒープ空き 311KB / PSRAM 空き 8.2MB。

**未検証**: `selftest` はキャンバスと転送経路だけを見ており、`cdk_display_flush()`
（バイトスワップ、ダーティ帯の座標、バウンスバッファの交互使用）は通っていない。
画面が真っ黒でも selftest は緑になる。実機の目視確認が要る:

- 右上マーカーが**緑**、左下が**青** → 向きは正しい
- 上下または左右が入れ替わっている → mirror / rotation
- 中段のグレー勾配に色が付いて見える → バイトスワップが逆

### M1 — ターミナル ✅ 完了 (2026-08-27)

- [x] `cdk_text` — UTF-8 ストリームデコーダ + East Asian Width
- [x] `cdk_font` — 東雲 12px ブロブ (207KB) をフラッシュに焼き込み
- [x] `tools/mkfont.py` — BDF → ブロブ。罫線 128 字 / ブロック 32 字 / 記号 43 字を自前生成
- [x] `cdk_term` — VT パーサ（§6.2 の全部）+ 画面バッファ + 代替画面
- [x] `cdk_keys` — キーイベント → 端末バイト列
- [x] `cdk_screen` — セルグリッド → キャンバス。256色 / truecolor / 属性
- [x] `cdk_render vt` — ホストとデバイスで共有する基準レンダラ
- [x] ホスト側テスト 8 スイート（`test_proto` `test_text` `test_canvas` `test_font`
      `test_term` `test_keys` `test_screen` `no_idf_in_core`）
- [x] **完了条件**: `cdk.py selftest` が
      `shot --verify-vt host/fixtures/vt/banner.vt` まで通り、
      実機の画面がホストの基準画像と 153,600 バイト完全一致

実測値: アプリ 503KB（うちフォント 207KB）/ 内部ヒープ空き 310KB / PSRAM 空き 8.19MB /
端末 53桁 x 20行 = 34KB。

実機で確認できたこと: 単線・二重線・太線の枠が正しく接続する、
ブロック要素の塗り分け、矢印・✓✗●○◆・≤≥≠π、日本語（かな漢字・半角カナ）、
16色/明色/truecolor、太字（明色化）・下線・反転・淡色。

### M2 — WiFi + SSH ✅ 基本達成 (2026-08-27)

- [x] WiFi STA 接続（`port_net`）。切断時の自動再接続あり
- [x] libssh2 1.11.1 のベンダリングと実機での初期化
- [x] SSH ハンドシェイク → 認証 → **PTY 付きシェル**
- [x] キー入力と端末応答 (DA/DSR) を SSH チャネルへ配線
- [x] SSH 受信をメインループから汲んで `cdk_term` へ
- [x] **完了条件**: 実機から SSH でログインし、`top` が 53x20 で正しく描画される

実機で確認できたこと:

```
COLS=53 LINES=20 TERM=xterm-256color
```

- PTY のサイズが `cdk_term` の実値でリモートに伝わっている（80x26 決め打ちではない）
- `top` の反転ヘッダ・桁揃え・連続再描画が実機パネルで崩れない（カメラで確認）
- `clear` / `ls --color` / 色付きプロンプトが正しく出る

まだ残っているもの:

- [ ] `known_hosts` によるホスト鍵検証（M4。SD が要る）。いまは指紋をログに出すだけ
- [ ] 認証情報を NVS へ（いまは `secrets.h` にビルド時に焼いている）
- [ ] 公開鍵認証
- [ ] 画面サイズ変更時の `cdk_ssh_resize()` 呼び出し（API はある、呼ぶ側が未実装）
- [ ] 再接続 UI（切れたら手で再起動するしかない）

踏んだ罠:

- **認証の失敗理由が切り分けられない。** `libssh2_userauth_password` は
  「パスワードが違う」も「その方式をサーバーが出していない」も同じ
  `LIBSSH2_ERROR_AUTHENTICATION_FAILED` (-18) を返す。
  `libssh2_userauth_list()` の結果をログに出すこと。
  今回はこれで「サーバーは `publickey,password` を出している」と分かり、
  資格情報側の問題だと確定できた（実際 `.env` のユーザー名にタイポがあった）。
- `keyboard-interactive` しか出さない sshd もあるのでフォールバックを入れてある。

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
