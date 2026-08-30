//! 画面描画。**まだ実装されていない。**
//!
//! 名前と型だけ先に決めてある。呼ぶと必ず [`Error::NoSys`] が返る。
//!
//! キャンバスはファームのメインループが専有していて、所有権の受け渡しを
//! 設計しないと開放できない。それは M8 の範囲外（DESIGN.md §15.7）。
//! ここに口があるのは、あとから実装したときにアプリを組み直さずに済ませるため。
//!
//! 文字を出すだけなら [`crate::con`] を使う。位置指定も色も、端末の
//! エスケープ列がそのまま通る。
//!
//! [`Error::NoSys`]: crate::Error::NoSys

use crate::error::{Result, check, check_ok};
use crate::sys;

/// 画面の大きさ（画素）。
pub fn size() -> Result<(u32, u32)> {
    let mut w: u32 = 0;
    let mut h: u32 = 0;
    check(unsafe { sys::gfx_size(&mut w, &mut h) })?;
    Ok((w, h))
}

pub fn clear(rgb565: u16) -> Result<()> {
    check_ok(unsafe { sys::gfx_clear(i32::from(rgb565)) })
}

pub fn fill_rect(x: i32, y: i32, w: i32, h: i32, rgb565: u16) -> Result<()> {
    check_ok(unsafe { sys::gfx_fill_rect(x, y, w, h, i32::from(rgb565)) })
}

pub fn blit(x: i32, y: i32, w: i32, h: i32, pixels: &[u8]) -> Result<()> {
    let n = crate::error::len_i32(pixels.len())?;
    check_ok(unsafe { sys::gfx_blit(x, y, w, h, pixels.as_ptr(), n) })
}

pub fn text(x: i32, y: i32, s: &str, fg: u16, bg: u16) -> Result<()> {
    let n = crate::error::len_i32(s.len())?;
    check_ok(unsafe { sys::gfx_text(x, y, s.as_ptr(), n, i32::from(fg), i32::from(bg)) })
}

pub fn present() -> Result<()> {
    check_ok(unsafe { sys::gfx_present() })
}
