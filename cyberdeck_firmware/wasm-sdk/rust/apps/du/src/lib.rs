//! microSD の中身を大きい順に並べる。`wasm run apps/rs_du.wasm [dir]`
//!
//! **Rust で書くとどう楽になるかの見本。** 同じことを C 版の SDK でやると、
//! 可変長の配列も並べ替えも文字列の組み立ても自前で書くことになる。
//! ここで使っているのは `Vec` / `sort_by` / `format!` / `?` と、
//! `read_dir` のイテレータ。
#![no_std]

extern crate alloc;

use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;
use cdk_app::prelude::*;

cdk_app::main!(run);

const KB: u32 = 1024;
const MB: u32 = 1024 * 1024;

fn human(bytes: u32) -> String {
    if bytes >= MB {
        format!("{}.{}M", bytes / MB, (bytes / (MB / 10)) % 10)
    } else if bytes >= KB {
        format!("{}.{}K", bytes / KB, (bytes / (KB / 10)) % 10)
    } else {
        format!("{bytes}B")
    }
}

fn run() -> Result<()> {
    let dir = args::get(1).unwrap_or_else(|_| String::from("apps"));

    // 名前と大きさを集める。読めなかったものは飛ばす。
    let mut entries: Vec<(String, u32)> = fs::read_dir(&dir)?
        .filter_map(|name| {
            let path = format!("{dir}/{name}");
            fs::size(&path).ok().map(|sz| (name, sz))
        })
        .collect();

    if entries.is_empty() {
        println!("{dir} に読めるファイルがありません");
        return Ok(());
    }

    // 大きい順。同じ大きさなら名前順。
    entries.sort_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(&b.0)));

    let total: u32 = entries.iter().map(|e| e.1).sum();
    let widest = entries.iter().map(|e| e.0.len()).max().unwrap_or(0);

    for (name, size) in &entries {
        println!("{name:<widest$}  {:>7}", human(*size));
    }
    println!("{:-<1$}", "", widest + 9);
    println!("{:<widest$}  {:>7}", format!("{} 個", entries.len()), human(total));
    Ok(())
}
