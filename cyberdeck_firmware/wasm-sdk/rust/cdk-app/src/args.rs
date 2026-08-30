//! コマンドラインの引数。
//!
//! `argv[0]` は走らせたファイル自身（`wasm run apps/blink.wasm 1 20` なら
//! `apps/blink.wasm`）。ファーム側の上限は 8 個 / 合計 256 バイト。

use alloc::string::String;
use alloc::vec;

use crate::error::{Error, Result, check};
use crate::sys;

/// 引数の数。
pub fn count() -> usize {
    let n = unsafe { sys::sys_argc() };
    if n < 0 { 0 } else { n as usize }
}

/// `idx` 番目の引数。
pub fn get(idx: usize) -> Result<String> {
    if idx >= count() {
        return Err(Error::Inval);
    }
    // 合計で 256 バイトしか無いので、1 個ぶんはこれで必ず足りる。
    let mut buf = vec![0u8; sys::ARGV_BYTES];
    let n = unsafe { sys::sys_arg(idx as i32, buf.as_mut_ptr(), buf.len() as i32) };
    let n = check(n)? as usize;
    buf.truncate(n);
    String::from_utf8(buf).map_err(|_| Error::Inval)
}

/// 引数を順に返す。読めなかったものは飛ばす。
pub fn iter() -> impl Iterator<Item = String> {
    (0..count()).filter_map(|i| get(i).ok())
}

/// `idx` 番目を 10 進の整数として読む。無ければ / 数でなければ `fallback`。
///
/// サンプルがどれも同じ物を書いていたので SDK に上げた。
pub fn int_or(idx: usize, fallback: i32) -> i32 {
    match get(idx) {
        Ok(s) => s.trim().parse::<i32>().unwrap_or(fallback),
        Err(_) => fallback,
    }
}
