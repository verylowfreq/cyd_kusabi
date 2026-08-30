//! GPIO を点滅させる。`wasm run apps/rs_blink.wasm <ピン> [回数]`
//!
//! 画面が固まらないことの確認にもなる（アプリは別タスクで走っている）。
#![no_std]

extern crate alloc;

use cdk_app::prelude::*;

cdk_app::main!(run);

fn run() -> Result<()> {
    let pin_no = args::int_or(1, 1);
    let times = args::int_or(2, 10);

    // 予約ピンならここで Err になる。C 版と違って握り潰せない。
    let pin = gpio::Pin::output(pin_no).map_err(|e| {
        println!("G{pin_no} は使えません: {e}");
        e
    })?;

    for i in 1..=times {
        pin.high()?;
        time::delay(250);
        pin.low()?;
        time::delay(250);
        print!("\r{i} / {times}");
    }
    println!();
    println!("終わり");
    Ok(())
}
