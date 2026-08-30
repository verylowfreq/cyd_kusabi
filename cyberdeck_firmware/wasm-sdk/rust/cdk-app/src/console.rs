//! コンソール。
//!
//! 端末は **CR+LF を要る**。`println!` は末尾に `\r\n` を付けるので、
//! そこを気にするのは生のバイト列を出す [`write`] を使うときだけ。

use crate::error::{Result, check, len_i32};
use crate::sys;

/// 端末へ書く。UTF-8 と VT エスケープ列はそのまま通る。
///
/// リングが埋まっているとファーム側で空くまで待つ。**待っている間に
/// Ctrl-C が来るとトラップして戻ってこない。**
pub fn write(bytes: &[u8]) -> Result<usize> {
    if bytes.is_empty() {
        return Ok(0);
    }
    let n = len_i32(bytes.len())?;
    let r = unsafe { sys::con_write(bytes.as_ptr(), n) };
    Ok(check(r)? as usize)
}

/// 文字列を書く。
pub fn write_str(s: &str) -> Result<usize> {
    write(s.as_bytes())
}

/// キーの入力を読む。**待たない。** 何も無ければ `Ok(0)`。
///
/// 中身は普通の端末と同じバイト列（矢印なら `ESC [ A` など）。
/// 暇なときは [`crate::time::delay`] で譲ること。回し続けると
/// メインループが画面を描く隙が減る。
pub fn read(buf: &mut [u8]) -> Result<usize> {
    if buf.is_empty() {
        return Ok(0);
    }
    let n = len_i32(buf.len())?;
    let r = unsafe { sys::con_read(buf.as_mut_ptr(), n) };
    Ok(check(r)? as usize)
}

/// 端末の桁数と行数。
pub fn size() -> Result<(u32, u32)> {
    let mut cols: u32 = 0;
    let mut rows: u32 = 0;
    let r = unsafe { sys::con_size(&mut cols, &mut rows) };
    check(r)?;
    Ok((cols, rows))
}

/// `write!` / `writeln!` を使うための受け皿。
///
/// `core::fmt` は失敗を握り潰す作りなので、途中で書けなくなっても
/// `fmt::Error` になるだけで理由は残らない。理由が欲しいときは
/// [`write`] を直接呼ぶこと。
pub struct Console;

impl core::fmt::Write for Console {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        write(s.as_bytes()).map(|_| ()).map_err(|_| core::fmt::Error)
    }
}
