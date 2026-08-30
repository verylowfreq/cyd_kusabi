//! ホストの ctest 用の fixture。
//!
//! **Rust の SDK がファームと同じ ABI を話していることの証拠がこれ。**
//! 実機と同じ `port_wasm_vm.c` / `port_wasm_natives.c` にリンクして走らせ、
//! 出力を突き合わせる。
//!
//! C 版の `f_hello` / `f_args` / `f_fs` / `f_nosys` がばらばらに見ていた
//! ことを 1 本にまとめてある（Rust 側は 1 本のほうが作り直しが楽なので）。
#![no_std]

extern crate alloc;

use cdk_app::prelude::*;

cdk_app::main!(run);

fn run() -> Result<()> {
    // コンソール。改行はテストしやすいように入れない。
    print!("hello");

    // 引数。複製が効いていないと化ける。
    print!(" argc={}", args::count());
    for (i, a) in args::iter().enumerate() {
        print!(" [{i}]={a}");
    }

    // 端末の大きさ（偽バックエンドの既定は 80x26）。
    let (cols, rows) = con::size()?;
    print!(" size={cols}x{rows}");

    // ファイル。書いて読み戻す。
    {
        let mut f = fs::File::create("note.txt")?;
        f.write_str("abcdef")?;
    }
    let s = fs::read_to_string("note.txt")?;
    print!(" read={s}");

    // サンドボックス。どちらも Perm のはず。
    print!(" esc1={:?}", fs::File::open("../secret").unwrap_err());
    print!(" esc2={:?}", fs::File::open("/etc/passwd").unwrap_err());

    // 予約したままの gfx_* は NoSys。
    print!(" gfx={:?}", gfx::present().unwrap_err());

    // alloc が効いていること（Vec と format! を通す）。
    let v: alloc::vec::Vec<u32> = (1..=5).collect();
    let sum: u32 = v.iter().sum();
    print!(" sum={sum}");

    Ok(())
}
