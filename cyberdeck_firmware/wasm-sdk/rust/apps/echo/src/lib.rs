//! 打った文字をそのまま返す。Ctrl-C で終わる。
//!
//! `con::read` は待たないので、暇なときは `time::delay` で譲る。
//! 回し続けるとメインループが画面を描く隙が減る。
#![no_std]

extern crate alloc;

use cdk_app::prelude::*;

cdk_app::main!(run);

fn run() -> Result<()> {
    println!("打つとそのまま返します。Ctrl-C で終わり。");

    let mut buf = [0u8; 64];
    loop {
        let n = con::read(&mut buf)?;
        if n > 0 {
            con::write(&buf[..n])?;
        } else {
            // Ctrl-C はここで効く。戻ってこない。
            time::delay(20);
        }
    }
}
