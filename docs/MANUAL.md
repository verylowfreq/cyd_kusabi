# サイバーデック 取扱説明書

ESP32-S3 の携帯端末。**SSH で入った先を、この機械だけで操作する**ためのもの。
日本語入力とテキストエディタを内蔵しているので、PC を開かずに書ける。

対象: cyberdeck v1（3.5 インチ 480x320 / USB キーボード / microSD）
最終更新: 2026-08-29

---

## 1. まず動かす

電源を入れると 10 秒ほどで起動して、こんな画面になる
（下は 2 倍表示のときの短い版。等倍だともう少し長いものが出る）。

```
+----------------------------------+
| CYBERDECK  ESP32-S3 term         |
+----------------------------------+
font  : 東雲ゴシック 12px x2
日本語: あいうえお 漢字 ｱｲｳｴｵ
attrs : bold under rev
sd    : SL16G 15193MB
skk   : 175791 語 学習 10
wifi  : 接続 192.168.1.97

help でコマンド一覧
$
```

`sd` `skk` `wifi` の行は起動時の状態。**繋がらなかったものは黄色か赤で出る**
ので、ここを見れば何が使えるかが分かる。

`$` が出ていれば使える状態。**USB キーボードをつないでそのまま打てる。**

`help` と打つとコマンドの一覧が出る。

---

## 2. キーボード

USB のキーボードを OTG ポートに挿す。挿したまま電源を入れても、
あとから挿してもよい。認識すると下の帯に `KBD` が出る。

### 配列

**既定は JIS 配列。** US 配列のキーボードを使うときは:

```
set keymap us
```

すぐ効く（再起動は要らない）。逆は `set keymap jis`。

配列が合っていないと、英字は正しいのに**記号だけ違う**という出方をする。
見分け方は `@` を打ってみること。

| 打った結果 | いま何になっているか |
|---|---|
| `@` が出る | 合っている |
| `[` が出る | JIS のキーボードに US の設定 → `set keymap jis` |
| 何も出ない / 別の記号 | US のキーボードに JIS の設定 → `set keymap us` |

### 使えないもの

- **`ろ` と `￥` 以外の日本語キー**（変換・無変換・かなキー）は無視する。
  かな入力の切り替えは `Ctrl-J`（下記）。
- テンキーの NumLock は見ていない。常に数字が出る。

---

## 3. 日本語を書く

SKK 方式。**`Ctrl-J` でかな入力に入る。**

```
Ctrl-J   かな入力を始める / 変換を確定する
l        英数に戻る
q        ひらがな ⇄ カタカナ
L        全角英数
```

いまのモードは画面下の帯に `あ` `ア` `ｱ` `Ａ` で出る。英数のときは何も出ない。

モードを変えるキーは**打ちかけのローマ字が残っていても効く**。
たとえば `nihon` と打った直後は末尾の `n` が残っているが、そこで `l` を
押せば `ん` を出しきってから英数に戻る。打った字が消えることはない。

小書き文字は `x` 系（`xa` → ぁ、`xtu` → っ）、`くぁ` は `kw` 系（`kwa`）。
**`l` と `q` は文字を打つのには使わない。**

### 漢字にする

**大文字で始めると変換が始まる。** 画面に `▽` が出る。

```
Nihon        →  ▽にほん
[Space]      →  ▼日本          候補が出る
[Space]      →  ▼二本          次の候補
x            →  前の候補に戻る
[Enter]      →  日本            確定
```

送り仮名があるときは、**送りの先頭を大文字にする。**

```
KaKu         →  ▽か*く  →  [Space] →  ▼書く
```

### 覚える

確定した変換は microSD の `/sdcard/skk/user.dic` に貯まる。
次からは候補の先頭に出る。SD が無いときは電源を切るまでの間だけ覚える。

### 辞書を引く

```
skkdict にほん
```

### 注意

`▽` や `▼` の途中でも **`Ctrl-S` や `Ctrl-Q` は通る**（エディタの保存と終了）。
かな入力中にキーが効かなくなることはない。

---

## 4. シェル

`$` のところでコマンドを打つ。

```
↑ ↓          履歴をたどる
← →          カーソル移動
Tab          コマンド名を補完する
Ctrl-A / E   行頭 / 行末
Ctrl-B / F   1 文字 戻る / 進む
Ctrl-U       カーソルより前を消す
Ctrl-K       カーソルより後ろを消す
Ctrl-W       直前の単語を消す
```

### 画面からあふれた出力を見る

**`Shift+PgUp` で遡れる。** 200 行ぶん覚えている。

```
Shift+PgUp   1 画面ぶん遡る
Shift+PgDn   1 画面ぶん戻る
```

遡っている間は帯の左に `^12` のように**何行遡っているか**が出る。
**何か打てば最下部に戻る**ので、慌てて戻す操作は要らない。

SSH でつないだ先でも同じように効く（`Shift+PgUp` は相手には送らない）。
ただし **`vi` や `less` のように画面を占有するプログラムの中では効かない。**
それらは自分の画面を持っていて、こちらの履歴とは関係がないため
（エディタの中でも同じ）。

### コマンド一覧

| | |
|---|---|
| `help` | コマンド一覧 |
| `clear` | 画面を消す |
| `echo` | 引数をそのまま出す |
| `history` | コマンド履歴 |
| `info` | 機器と接続の状態 |
| `free` | メモリの空き |
| `set` | 設定（→ 7 章） |
| `wifi` | `wifi status` / `wifi connect` |
| `ssh` | SSH（→ 6 章） |
| `hostkey` | 接続先のホスト鍵（`hostkey forget` で忘れる） |
| `sd` | `sd mount` / `sd unmount` |
| `ls` `cat` `rm` `mkdir` | microSD のファイル操作 |
| `sdtest` | SD に書いて読み戻す |
| `skk` | `skk on` / `skk off` / `skk kata` |
| `skkdict` | 辞書を引く |
| `edit` | テキストを編集する |
| `gpio` `adc` `i2c` | 外部端子をいじる |
| `reboot` | 再起動 |
| `wasm run <file>` | ユーザーアプリを走らせる（→ 10 章） |

---

## 5. テキストを書く

```
edit memo.txt
```

ファイル名は必須。`/` で始めない限り microSD の直下になる
（`memo.txt` は `/sdcard/memo.txt`）。無ければ新規。

```
Ctrl-S       保存
Ctrl-Q       終了（変更があるともう一度聞く）
Ctrl-A/E     行頭 / 行末
↑↓←→        移動
PgUp/PgDn    ページ送り
```

日本語もそのまま書ける（`Ctrl-J` でかな入力）。
**エディタとシェルでかな入力の状態は別に覚えている**ので、
エディタでかなにしたまま抜けてもシェルは英数のまま。

長い行は画面の幅で折り返して表示する（ファイルには改行は入らない）。

---

## 6. SSH

### つなぐ

```
set wifi.ssid  <アクセスポイント名>
set wifi.psk   <パスワード>
set ssh.host   <接続先>
set ssh.user   <ユーザー名>
ssh
```

`ssh close` で抜ける。リモートで `exit` しても戻る。

### 公開鍵でログインする（おすすめ）

**この機械で鍵を作れる。**

```
ssh keygen
```

ECDSA P-256 の鍵を microSD に作り、公開鍵を 1 行で表示する。
その行を接続先の `~/.ssh/authorized_keys` に貼れば、パスワード無しで入れる。

```
ssh key      いまの鍵の指紋と公開鍵をもう一度出す
```

貼ったあと `ssh` して、うまくいけばパスワードは消してよい。

```
set ssh.pass ""
```

> **`ssh-keygen` で作った ed25519 の鍵は使えない。**
> この機械の SSH ライブラリが対応していない。RSA か ECDSA のみ。
> `ssh keygen` が作るのは ECDSA なので、そのまま使えばよい。

> **秘密鍵は microSD にそのまま置かれる。** SD には権限が無いので、
> カードを抜けば誰でも読める。人に渡す機械では鍵を消すこと。

### 貼ったのに入れないとき

指紋が合っているか、接続先で確かめる。

```
ssh-keygen -lf ~/.ssh/authorized_keys
```

ここに `ssh key` で出た指紋が並んでいなければ、貼れていない。
よくあるのは:

- **ファイル名が違う**（`.authorized_keys` のように先頭に `.` が付いている）
- **長い base64 が改行で折れている**（1 行でなければならない）

### 切れたとき

通信が切れると自動で繋ぎ直す（1 → 2 → 4 → 8 → 15 秒、最大 8 回）。
**待っている間もシェルは使える。どのキーを押しても中止する。**

繋ぎ直さないのは:

- リモートで `exit` したとき（抜けたいのに戻されると困るので）
- `ssh close` したとき

自動で繋ぎ直してほしくないときは `set ssh.recon off`。

### ホスト鍵

初めての接続先は鍵を覚える（TOFU）。次から食い違ったら**必ず接続を断る**。
接続先を作り直したときなどは `hostkey forget` で忘れさせる。

---

## 7. 設定

`set` だけ打つと今の値が並ぶ。`set <キー> <値>` で変える。
**空文字にすると消える**（`set ssh.pass ""`）。

| キー | 内容 | 反映 |
|---|---|---|
| `wifi.ssid` `wifi.psk` | アクセスポイント | 次の `wifi connect` |
| `ssh.host` `ssh.user` `ssh.pass` | 接続先 | 次の `ssh` |
| `ssh.key` | 秘密鍵の場所（既定 `/sdcard/id_ecdsa`） | 次の `ssh` |
| `ssh.recon` | 自動で繋ぎ直すか（既定 `on`） | すぐ |
| `keymap` | キー配列 `jis` / `us` | すぐ |
| `gamma` | 画面の明るさ補正 10〜40（既定 22 = 2.2） | すぐ |
| `fontscale` | 文字の大きさ `1` / `2` | **再起動** |
| `statusbar` | 下の帯を出すか `on` / `off` | **再起動** |

`(既定)` と出ているものは、設定しなくてもその値で動いている。設定は不要。

パスワードは `(設定済み 12 文字)` のように伏せて出る。
接続先とユーザー名は伏せない（合っているか確かめられないと困るので）。

---

## 8. 展示で使うとき

離れたところから読ませるなら **文字を 2 倍**にする。

```
set fontscale 2
set statusbar on
reboot
```

| | 桁 x 行 |
|---|---|
| 等倍 | 80 x 26（帯を出すと 80 x 25） |
| 2 倍 | 40 x 13（帯を出すと 40 x 12） |

**2 倍にすると SSH の画面は 40 桁 x 12 行しかない。** `top` などはほぼ埋まる。
遠くから読めることを取るか、情報量を取るかの選択になる。

戻すときは `set fontscale 1` と `reboot`。

画面の色が暗くて読みにくいときは `set gamma` を上げる（22 → 26 など）。
すぐ反映される。10 が無補正、40 が最大。

---

## 9. 困ったとき

| 症状 | 見るところ |
|---|---|
| 記号だけ違う字が出る | キー配列（→ 2 章） |
| 画面が暗くて読めない | `set gamma 26` |
| かな入力から抜けられない | `l`（小文字のエル）。打ちかけがあっても効く |
| SSH に入れない | `wifi status` で IP が出ているか。`info` で状態を見る |
| 公開鍵で入れない | 接続先の `authorized_keys`（→ 6 章） |
| ホスト鍵が違うと言われる | 接続先を作り直したなら `hostkey forget` |
| SD が見えない | `sd mount`。刺し直して `reboot` |
| 設定を変えたのに効かない | `fontscale` と `statusbar` は再起動が要る |
| 画面が固まったように見える | **`Shift+PgUp` で遡っていませんか**（帯の左に `^12` のように出ます）。何か打てば戻ります |
| 出力が流れて読めなかった | `Shift+PgUp` で遡れます（→ 4 章） |
| それでも固まっている | `reboot`。それでも駄目なら電源を入れ直す |

`info` と `free` が状態を出す。`wifi status` は直近の切断理由も出す。

---

## 10. 自分でアプリを書く

**ファームウェアを焼き直さずに機能を足せる。** WebAssembly (`.wasm`) を
microSD に置いて `wasm run` で走らせる。

センサを読んで SD に記録する、GPIO を叩く、ネットワークから何か取ってくる、
といったことが、C を書いて `.wasm` に変換するだけでできる。

### 走らせる

```
wasm ls                          apps/ にあるアプリを並べる
wasm run apps/hello.wasm         走らせる
wasm run apps/blink.wasm 1 20    引数も渡せる
wasm info                        ABI と上限を見る
```

**止めるのは `Ctrl-C`。** アプリが動いている間もシェルは止まっているので、
`wasm stop` のようなコマンドは無い。

アプリが動いている間も**画面と時計は動き続ける**。固まったように見えたら、
それはアプリが何か待っているだけ。

### 付いてくるアプリ

`wasm-apps/` にソースがある。読めばだいたいの書き方が分かる。

| | 何をするか |
|---|---|
| `hello.wasm` | 挨拶して端末の大きさと引数を出す |
| `echo.wasm` | 打った文字をそのまま返す（入力の例） |
| `flood.wasm` | 200 行を一息に出す（出力が落ちないかの確認） |
| `spin.wasm` | 回り続ける（Ctrl-C の確認） |
| `blink.wasm` | GPIO を点滅させる。`wasm run apps/blink.wasm <ピン> [回数]` |
| `i2cscan.wasm` | I2C を探す。`wasm run apps/i2cscan.wasm <SDA> <SCL>` |
| `uartecho.wasm` | 端末と外部 UART をつなぐ |
| `httpget.wasm` | `wasm run apps/httpget.wasm example.com /` |

### 書く

```c
#include "cdk_app.h"

CDK_MAIN int cdk_main(int argc)
{
    (void)argc;
    cdk_print("やあ\r\n");

    int fd = fs_open(CDK_S("log.txt"), CDK_OPEN_APPEND);
    if (fd >= 0) {
        fs_write(fd, CDK_S("動いた\n"));
        fs_close(fd);
    }
    return 0;
}
```

**改行は `\r\n`。** `\n` だけだと行頭に戻らず、階段状に流れていく。

使える関数は `wasm-sdk/include/cdk_app.h` に全部書いてある。
コンソール・ファイル・待ち・GPIO・ADC・I2C・UART・TCP/UDP ソケット。

### 変換する

PC 側で clang を使う。**WASI SDK のような別のものを入れる必要は無い。**

```
python tools/wasmcc.py wasm-apps/hello.c      # → wasm-apps/build/hello.wasm
python tools/wasmcc.py --all                  # 全部
```

### 実機へ送る

**microSD を抜かなくていい。** ケーブルが繋がっていれば PC から直接書ける。

```
python tools/cdk.py push wasm-apps/build/hello.wasm apps/hello.wasm
```

置き場所を省くと `apps/` の下に同じ名前で入る。

### Rust で書く

C の代わりに Rust でも書ける。**できることは同じ**（同じ API を使う）。
違うのは書き味と、落ちたときに何が分かるか。

```
rustup target add wasm32-unknown-unknown
```

これだけ。ほかに入れるものは無い。

```rust
#![no_std]
extern crate alloc;

use cdk_app::prelude::*;

cdk_app::main!(run);

fn run() -> Result<()> {
    println!("やあ");

    let mut f = fs::File::append("log.txt")?;
    f.write_str("動いた\n")?;
    Ok(())
}
```

`wasm-sdk/rust/apps/<名前>/` に置いて:

```
python tools/wasmrs.py --all
python tools/cdk.py push wasm-apps/build/rs_hello.wasm apps/rs_hello.wasm
```

実機では `wasm run apps/rs_hello.wasm`。C 版と同じ名前のサンプルが
`rs_` 付きで並ぶので、`wasm ls` で見比べられる。

#### 何が嬉しいか

**間違えたときに、どこで間違えたかが画面に出る。**

```
panic: index out of bounds: the len is 3 but the index is 7 (apps\boom\src\lib.rs:21)
```

C 版だと `落ちました: out of bounds memory access` としか出ないので、
アプリのどこが悪いのかは自分で探すことになる。

ほかに、失敗を `?` で流せること、`format!` で文字列を組めること
（C 版の SDK には `snprintf` が無い）。`wasm-sdk/rust/apps/du/` が
その辺りを一通り使った見本になっている。

#### Rust だけが踏む落とし穴

**確保したメモリは返ってこない。** `Vec` や `String` を捨てても解放されず、
使えるのは全部で 1MB。つまり **確保を繰り返すループはいつか必ず落ちる。**
実際に測ったところ、70 バイトの文字列を作るループは約 15,000 回で尽きた。

```rust
// 落ちる: 毎回 String を作る
loop {
    let s = alloc::format!("{i} 番目");
    con::write(s.as_bytes())?;
}

// 落ちない: println! は文字列を作らずに直接書く
loop {
    println!("{i} 番目");
}
```

同じことを C で書くと文字列を作らないので、**この違いは Rust にだけ出る。**
長く回すアプリでは、確保をループの外に出すか、固定長の配列を使う。

#### そのほか

- **Ctrl-C で止めたとき、`Drop` は走らない。** ファイルなどはファーム側が
  閉じるので放っておいてよいが、「閉じる前にログを書く」ような後始末は
  当てにしないこと。
- **`println!` は改行の面倒を見る。** `con::write` に生のバイト列を渡す
  ときだけ `\r\n` を自分で付ける。
- 出来上がりは C 版より少し大きい（hello が 1.5KB → 7.5KB）。上限は
  256KB なので、実用上は気にしなくてよい。

詳しくは `wasm-sdk/rust/README.md`。

### できないこと・気をつけること

- **触れるのは microSD の中だけ。** `..` で外へ出ようとすると断られる。
  設定や秘密鍵はアプリからは見えない。
- **液晶に絵は描けない。** `gfx_*` という名前だけ用意してあるが、呼ぶと
  「未実装」が返る。文字は `con_write` で出す（色や位置は端末の
  エスケープ列がそのまま使える）。
- **同時に動くのは 1 本だけ。** アプリを動かしながらシェルは使えない。
- **`while (1) { }` のように何も呼ばずに回るアプリは Ctrl-C で止まらない。**
  `sys_delay` や `con_write` を挟んでいれば止まる。止まらなくなったら
  電源を入れ直すしかない。
- **アプリの大きさは 256KB まで。** 開けるファイルは同時に 4 つまで。
- **外部 UART は 1 本だけ。** 残りはコンソールと PC との通信で埋まっている。
- **液晶・microSD・USB・PC との通信に使っているピンは触れない。**
  名前を出して断るので、何に使われているかはその場で分かる。
- **I2C と UART は、実際のデバイスを繋いだ確認をしていない。**
  ピンの拒否とバスを開けるところまでしか試していない。

### 何かおかしいとき

| 出たもの | たぶんこれ |
|---|---|
| `SD がマウントされていません` | `sd mount` |
| `... は開けません（microSD の外です）` | パスが `/sdcard` の外を指している |
| `読めません: ...` | `.wasm` が壊れているか、`cdk_main` を出していない |
| `落ちました: out of bounds ...` | アプリが配列の外を触った（本体は無事） |
| `G12 は LCD SCLK に使われています` | そのピンは基板が使っている |

---

## 11. 分かっていること・できないこと

正直に書いておく。

- **ed25519 の鍵は使えない。** RSA か ECDSA のみ（→ 6 章）。
- **秘密鍵は microSD で保護されていない。** カードを抜けば読める。
- **電池残量は出ない。** 分圧比が決まっていないので、間違った電圧を出すより
  出さないことにしている。
- **2 倍表示のとき下の帯も 2 倍**になるので、そのぶん本文が 1 行減る。
- **USB キーボードは 1 台だけ。** ハブ経由や複数台は試していない。
- **再接続の中止（キーを押して止める）は実機で通していない。**
  相手が生きていると 1 回目で繋がってしまい、試す機会が無かった。

---

## 12. ファームウェアを更新する

### 用意するもの

- **ESP-IDF v5.5.5**（他の版では試していない）
- USB ケーブル 2 本。**用途が違うので両方挿す**

| 口 | 何に使うか | 例 |
|---|---|---|
| UART0 | **書き込み**とログ | COM76 |
| UART1 | 画面の取得などの制御 | COM75 |

> **USB-OTG の口は書き込みに使えない。** そこはキーボード専用。
> ESP32-S3 の USB は 1 組しかなく、この機械はホスト側で使い切っている。

### ビルドと書き込み

PowerShell で ESP-IDF を有効にしてから、`tools/cdk.py` を使う。

```powershell
cd cyberdeck_firmware
. C:\DATA-0\Espressif\esp-idf-v5.5.5\export.ps1

python tools\cdk.py build --board v1     # ビルド（この機械は v1）
python tools\cdk.py flash                # 書き込む
python tools\cdk.py boot                 # リセットして起動ログを見る
```

`--board` を付けると `.cdk_board` に覚えるので、次からは `build` だけでよい。
以降 `flash` や `boot` はポートを自動で選ぶ。

> **基板を変えるときは `sdkconfig` を消す。** ESP-IDF は既存の
> `sdkconfig` を defaults より優先するので、消さないと前の基板の設定が残る
> （PSRAM のモードが違うと起動時に必ず落ちる）。
> `cdk.py build --board` が自動で消すので、手で `idf.py` を叩くときだけ注意。

### うまくいかないとき

| 症状 | 見るところ |
|---|---|
| `flash` がポートを開けない | ケーブルと COM 番号。`python tools\cdk.py --prog COM76 flash` のように明示できる（`--prog` `--ctrl` `--log` は**サブコマンドの前**に置く） |
| 書けたのに起動ログが出ない | ログは UART0。制御用の口を見ていないか |
| 起動直後に落ちる | 基板違い。`sdkconfig` を消して `build --board v1` から |

### 動いているか確かめる

```powershell
python tools\cdk.py selftest
```

リセット → 疎通確認 → **画面を取得して PC 側で描いた基準画像と 1 バイト単位で
比べる**、までを自動でやる。`検証 OK` が出れば、表示まで含めて壊れていない。

### 辞書を入れ直す

SKK の辞書はファームとは別のパーティションにある。普段は焼き直さなくてよい。

```powershell
python tools\cdk.py dict SKK-JISYO.L.unannotated
```

---

## 付録: 開発者向け

PC から操作する道具は `cyberdeck_firmware/tools/cdk.py`。
**どのコマンドも必ず有限時間で終わる**（`idf.py monitor` は使わない）。

```
python tools/cdk.py log --timeout 5         ログを 5 秒ぶん読む
python tools/cdk.py ping                    制御チャネルの疎通確認
python tools/cdk.py shot out.png --verify   画面を取得して基準画像と比べる
python tools/cdk.py type "ls\n"             文字を流し込む
python tools/cdk.py key ctrl+j              キーを 1 つ送る
python tools/cdk.py cam out.jpg             パネルをカメラで撮る
python tools/cdk.py push a.wasm apps/a.wasm microSD へファイルを書く
```

ユーザーアプリ（→ 10 章）:

```
python tools/wasmcc.py wasm-apps/hello.c    .c → .wasm（clang だけで足りる）
python tools/wasmcc.py --all
python tools/wasmrs.py --all                Rust のアプリ（→ 10 章）
python tools/wasminfo.py out.wasm           .wasm がファームに渡せる形か見る
python tools/cdk.py push wasm-apps/build/hello.wasm apps/hello.wasm
```

ホスト側のテスト（実機は要らない）:

```powershell
cmake -S host -B build-host -G Ninja -DCMAKE_C_COMPILER=clang
ninja -C build-host
ctest --test-dir build-host
```

設計の意図と経緯は `DESIGN.md`、基板は `HARDWARE_DESIGN.md`、
作っている最中の記録は `DEV_STATS.md`。
