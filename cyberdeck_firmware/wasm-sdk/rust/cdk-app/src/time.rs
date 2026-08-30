//! 時計と待ち。

use crate::error::{Result, check_ok, len_i32};
use crate::sys;

/// 起動からの経過ミリ秒。
pub fn now_ms() -> u64 {
    unsafe { sys::sys_now_ms() as u64 }
}

/// 待つ。
///
/// **Ctrl-C が来たらここでアプリごと止まり、戻ってこない。**
/// 呼び出しの後ろに置いた後始末は走らないので、`Drop` に頼らないこと
/// （資源はファーム側が片づける）。
pub fn delay(ms: u32) {
    let ms = ms.min(i32::MAX as u32) as i32;
    unsafe {
        let _ = sys::sys_delay(ms);
    }
}

/// 乱数で埋める。
pub fn random(buf: &mut [u8]) -> Result<()> {
    if buf.is_empty() {
        return Ok(());
    }
    let n = len_i32(buf.len())?;
    let r = unsafe { sys::sys_random(buf.as_mut_ptr(), n) };
    check_ok(r)
}

/// `u32` の乱数を 1 つ。
pub fn random_u32() -> Result<u32> {
    let mut b = [0u8; 4];
    random(&mut b)?;
    Ok(u32::from_le_bytes(b))
}

/// 終了する。**戻ってこない。**
pub fn exit(code: i32) -> ! {
    unsafe { sys::sys_exit(code) }
}

/// `ms` ミリ秒ぶん、何かが起きるまで待つ。
///
/// `f` が `Some` を返したらそれを返す。時間切れなら `None`。
/// `con_read` や `sock_recv` のように「待たない」口を回すときの定型。
pub fn poll_for<T>(ms: u32, step_ms: u32, mut f: impl FnMut() -> Option<T>) -> Option<T> {
    let step = step_ms.max(1);
    let mut waited = 0;
    loop {
        if let Some(v) = f() {
            return Some(v);
        }
        if waited >= ms {
            return None;
        }
        delay(step);
        waited += step;
    }
}

/// 経過時間を測る。
pub struct Stopwatch(u64);

impl Stopwatch {
    pub fn start() -> Self {
        Stopwatch(now_ms())
    }
    pub fn elapsed_ms(&self) -> u64 {
        now_ms().saturating_sub(self.0)
    }
}
