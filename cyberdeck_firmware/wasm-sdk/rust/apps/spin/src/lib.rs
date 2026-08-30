//! Ctrl-C の検証用。native を呼びながら回り続ける。
//!
//! native を一度も呼ばない `loop {}` は wasm3 の構造上止められない
//! （DESIGN.md §15.5）。こちらは `time::delay` を呼ぶので止まる。
#![no_std]

extern crate alloc;

use cdk_app::prelude::*;

cdk_app::main!(run);

fn run() -> Result<()> {
    println!("回り続けます。Ctrl-C で止めてください。");
    let mut i: u32 = 0;
    loop {
        print!("\r{i}");
        time::delay(100);
        i += 1;
    }
}
