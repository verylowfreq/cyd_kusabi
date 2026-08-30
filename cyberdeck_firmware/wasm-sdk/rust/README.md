# cdk-app — サイバーデックのユーザーアプリを Rust で書く

独自 wasm ABI（`cdk` モジュール / 45 関数）に当てた `no_std` クレート。
C 版の [`wasm-sdk/include/cdk_app.h`](../include/cdk_app.h) と**同じ ABI**で、
`Result` と RAII が付いた形になっている。

ABI そのものの説明は [`DESIGN.md` §15](../../DESIGN.md)、使い方は
[取扱説明書の 10 章](../../../docs/MANUAL.md)。

## 要るもの

```
rustup target add wasm32-unknown-unknown
```

それだけ。**WASI SDK も wasm-pack も要らない。**

## 書く

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

`main!` が `cdk_main` を作る。`run` が `Err` を返したら**理由を端末に出して**
終了コード 1 で返る。

## 組んで実機へ

```
python tools/wasmrs.py --all
python tools/cdk.py push wasm-apps/build/rs_hello.wasm apps/rs_hello.wasm
```

実機で `wasm run apps/rs_hello.wasm`。止めるのは Ctrl-C。

`cargo build --release` だけでも同じものが出る（ターゲットとリンク指定は
`.cargo/config.toml` にある）。`wasmrs.py` はそのあとの配置と、
**ファームが受け取れる形かの検査**をする。

## 新しいアプリを足す

`apps/<名前>/` を作って `Cargo.toml` と `src/lib.rs` を置く。
ワークスペースの `members` が `apps/*` なので、登録は要らない。

```toml
[lib]
crate-type = ["cdylib"]      # ← これが要る。bin にしない

[dependencies]
cdk-app = { path = "../../cdk-app" }
```

## サンプル

| | 何をするか |
|---|---|
| `hello` | 挨拶して端末の大きさと引数を出す（C 版と同じ出力） |
| `echo` | 打った文字を返す |
| `flood` | 200 行を一息に出す（出力が落ちないかの確認） |
| `spin` | 回り続ける（Ctrl-C の確認） |
| `blink` | GPIO を点滅させる |
| `i2cscan` | I2C を探す |
| `uartecho` | 端末と外部 UART をつなぐ |
| `httpget` | 素の TCP で HTTP GET |
| **`du`** | **Rust で書く利点の見本。** `Vec` / `sort_by` / `format!` / `?` / イテレータ |
| `boom` | パニック表示の確認用 |

## C 版との違い

**同じ ABI なので、できることは同じ。** 違うのは書き味と落ち方。

| | C | Rust |
|---|---|---|
| 戻り値 | 負の数。見忘れても気づかない | `Result`。`?` で書ける |
| パニック | `落ちました: out of bounds` だけ | **`file:line` とメッセージが端末に出る** |
| 文字列 | `snprintf` すら無い | `format!` / `String` / `Vec` |
| 後始末 | 自分で閉じる | `Drop` で閉じる（下記の注意あり） |
| 大きさ | hello が 1.5KB | hello が 7.5KB（上限 256KB なので問題にならない） |

## 気をつけること

**確保は解放されない。** アロケータはバンプで、`Vec` や `String` を捨てても
戻らない。線形メモリは 1MB なので、**確保を繰り返すループはいつか必ず落ちる**
（実機で測ったところ、70 バイトの `format!` を約 15,000 回）。

```rust
// 落ちる: 毎回 String を作る
loop {
    let s = alloc::format!("{i} 番目");
    con::write(s.as_bytes())?;
}

// 落ちない: println! は core::fmt が直接書くので確保しない
loop {
    println!("{i} 番目");
}
```

同じループを C で書くと確保が起きないので、**これは Rust だけが踏む。**

**トラップで巻き戻ると `Drop` は走らない。** Ctrl-C で止められたときは
ホストがトラップさせるので、Rust のスタック巻き戻しは起きない。
資源はファーム側が終了時に全部閉じるので**漏れはしない**が、
「閉じる前にログを書く」ような後始末は当てにしないこと。

**端末は CR+LF が要る。** `println!` は面倒を見る。`con::write` に生の
バイト列を渡すときは自分で付ける。

## ABI がずれていないか

インポート名の一覧はファームの表・C の SDK・`sys.rs` の 3 か所にある。
ctest の `abi_names_match` が一致を見ているので、ABI を足すときは
**3 か所とも直す**（片方だけだと、その言語のアプリだけが動かなくなる）。

```
ctest --test-dir build-host -R abi_names_match
```
