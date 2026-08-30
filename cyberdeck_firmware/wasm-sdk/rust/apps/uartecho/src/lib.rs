//! 外部 UART と端末をつなぐ。`wasm run apps/rs_uartecho.wasm <tx> <rx> [baud]`
#![no_std]

extern crate alloc;

use cdk_app::prelude::*;

cdk_app::main!(run);

fn run() -> Result<()> {
    let tx = args::int_or(1, 43);
    let rx = args::int_or(2, 44);
    let baud = args::int_or(3, 115_200);

    let mut port = uart::Uart::open(tx, rx, baud as u32).map_err(|e| {
        println!("uart_open に失敗: {e}");
        e
    })?;
    println!("TX=G{tx} RX=G{rx} {baud} bps。Ctrl-C で終わり");

    let mut buf = [0u8; 64];
    loop {
        let mut idle = true;

        let n = con::read(&mut buf)?;
        if n > 0 {
            port.write(&buf[..n])?;
            idle = false;
        }

        let n = port.read(&mut buf)?;
        if n > 0 {
            con::write(&buf[..n])?;
            idle = false;
        }

        if idle {
            time::delay(10);
        }
    }
}
