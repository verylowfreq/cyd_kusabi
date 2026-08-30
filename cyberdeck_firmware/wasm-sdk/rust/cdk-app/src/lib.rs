//! # cdk-app — サイバーデックのユーザーアプリを Rust で書く
//!
//! WASI でも POSIX でもない、この機械だけの小さな ABI に当てたクレート。
//! C 版 (`wasm-sdk/include/cdk_app.h`) と同じ 45 個のインポートを、
//! `Result` と RAII の付いた形で包んでいる。
//!
//! ```ignore
//! #![no_std]
//! extern crate alloc;
//!
//! cdk_app::main!(run);
//!
//! fn run() -> cdk_app::Result<()> {
//!     cdk_app::println!("やあ");
//!     Ok(())
//! }
//! ```
//!
//! ビルドは `python tools/wasmrs.py --all`、実機へは
//! `python tools/cdk.py push wasm-apps/build/rs_hello.wasm apps/rs_hello.wasm`。
//!
//! ## 知っておくべき 3 つのこと
//!
//! **1. トラップで巻き戻ると `Drop` は走らない。**
//! Ctrl-C で止められたとき、[`time::delay`] や [`con::write`] の中から
//! ホストがトラップさせる。Rust のスタック巻き戻しではないので、
//! [`fs::File`] などの `Drop` は呼ばれない。**資源は漏れない**
//! （ファーム側が終了時に開きっぱなしを全部閉じる）が、
//! 「閉じる前にログを書く」ような後始末は当てにしないこと。
//!
//! **2. 端末は CR+LF を要る。** [`println!`] は面倒を見るが、
//! [`con::write`] に生のバイト列を渡すときは自分で付ける。
//!
//! **3. 確保は解放されない。** アロケータはバンプで、`Vec` や `String` を
//! 捨てても戻らない。線形メモリは 1MB なので、**確保を繰り返すループは
//! いつか必ず落ちる**（実測で 70 バイトの `format!` を約 15,000 回）。
//! 長く回るアプリでは確保をループの外へ出すこと。詳しくは [`heap`]。
//! なお [`println!`] は `core::fmt` が直接書くので確保しない。
//!
//! **4. 上限がある。** モジュールは 256KB、線形メモリは 1MB、
//! 同時に開けるファイルは 4 つ、外部 UART は 1 本、I2C は 1 本。
//! 超えるとファームが断る。値は [`sys`] に定数で置いてある。
//!
//! ## パニック
//!
//! パニックすると **`file:line` とメッセージが端末に出てから**止まる。
//! C 版では境界を踏んでも「落ちました」としか出なかったので、
//! ここが Rust で書くときの一番の利点になる。

#![no_std]

extern crate alloc;

pub mod sys;

pub mod error;

pub mod args;
pub mod fs;
pub mod gfx;
pub mod gpio;
pub mod i2c;
pub mod net;
pub mod time;
pub mod uart;

/// コンソール。
///
/// **ファイル名が `console.rs` なのは Windows のため。** `con` は
/// `CON` / `PRN` / `AUX` / `NUL` / `COM1` などと同じ予約デバイス名で、
/// `con.rs` を置くと Win32 の `open()` が「そんなファイルは無い」と言う。
/// 実際 git がこのファイルだけ add できずに転んだ。
/// 呼び名のほうは ABI の `con_*` に揃えたいので `con` のままにしてある。
#[path = "console.rs"]
pub mod con;

mod macros;

#[cfg(feature = "heap")]
pub mod heap;

#[cfg(feature = "heap")]
#[global_allocator]
static ALLOC: heap::Bump = heap::Bump;

#[cfg(feature = "panic-handler")]
mod panic;

pub use error::{Error, Result};

/// よく使うものをまとめて持ってくる。
pub mod prelude {
    pub use crate::error::{Error, Result};
    pub use crate::{args, con, fs, gfx, gpio, i2c, net, time, uart};
    pub use crate::{main, print, println};
}
