//! I2C を探す。`wasm run apps/rs_i2cscan.wasm <SDA> <SCL>`
#![no_std]

extern crate alloc;

use alloc::vec::Vec;
use cdk_app::prelude::*;

cdk_app::main!(run);

fn run() -> Result<()> {
    let sda = args::int_or(1, 2);
    let scl = args::int_or(2, 1);

    let mut bus = i2c::I2c::open_default(sda, scl).map_err(|e| {
        println!("i2c_open に失敗: {e}");
        e
    })?;
    println!("SDA=G{sda} SCL=G{scl} で探します");

    let found: Vec<u8> = (0x08..=0x77u8).filter(|a| bus.probe(*a)).collect();
    for a in &found {
        println!("  0x{a:02x}");
    }
    println!("{} 個", found.len());
    Ok(())
}
