//! パニックの表示を確かめるためだけのアプリ。
//!
//! 配列の外を踏む。C 版だとファームが「落ちました: out of bounds」としか
//! 言えないが、Rust なら**どのファイルの何行目で何が起きたか**が端末に出る。
#![no_std]

extern crate alloc;

use alloc::vec::Vec;
use cdk_app::prelude::*;

cdk_app::main!(run);

fn run() -> Result<()> {
    println!("これから範囲外を踏みます");

    let v: Vec<u32> = (0..3).collect();
    let i = args::int_or(1, 7) as usize;
    println!("v[{i}] を読みます（長さ {}）", v.len());

    println!("値は {}", v[i]);
    Ok(())
}
