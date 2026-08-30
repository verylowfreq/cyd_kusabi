//! 大量に出力する。出力リング (4KB) をアプリが追い越す状況を作る。
//!
//! 捨てる実装だと途中の行が黙って消える。待つ実装なら全部出る。
//! 最後に行数を出すので、数えれば落ちていないか分かる。
#![no_std]

extern crate alloc;

use cdk_app::prelude::*;

cdk_app::main!(run);

fn run() -> Result<()> {
    const LINES: u32 = 200;
    for i in 1..=LINES {
        print!("{i} ");
    }
    println!();
    println!("--- {LINES} 行 ---");
    Ok(())
}
