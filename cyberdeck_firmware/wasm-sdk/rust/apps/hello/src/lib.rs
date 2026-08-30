//! いちばん短いユーザーアプリ（Rust 版）。C 版の hello.c と同じことをする。
#![no_std]

extern crate alloc;

use cdk_app::prelude::*;

cdk_app::main!(run);

fn run() -> Result<()> {
    println!("こんにちは、wasm から");

    let (cols, rows) = con::size()?;
    println!("端末は {cols}桁 x {rows}行");

    for (i, a) in args::iter().enumerate() {
        println!("引数[{i}] = {a}");
    }
    Ok(())
}
