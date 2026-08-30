//! 外部 UART。**1 本しかない。**
//!
//! UART0 はコンソール、UART1 は PC との制御チャネルで埋まっている。
//! 残る 1 本を貸し出す形なので、2 本目を開こうとすると [`Error::NoMem`]。
//!
//! [`Error::NoMem`]: crate::Error::NoMem

use crate::error::{Result, check, check_ok, len_i32};
use crate::sys;

/// 開いている UART。`Drop` で閉じる。
pub struct Uart(i32);

impl Uart {
    /// 開く。ピンは予約表を通る。
    pub fn open(tx: i32, rx: i32, baud: u32) -> Result<Uart> {
        let baud = len_i32(baud as usize)?;
        let h = unsafe { sys::uart_open(tx, rx, baud) };
        Ok(Uart(check(h)?))
    }

    pub fn write(&mut self, data: &[u8]) -> Result<usize> {
        if data.is_empty() {
            return Ok(0);
        }
        let n = len_i32(data.len())?;
        let r = unsafe { sys::uart_write(self.0, data.as_ptr(), n) };
        Ok(check(r)? as usize)
    }

    /// **待たない。** 何も無ければ `Ok(0)`。
    pub fn read(&mut self, buf: &mut [u8]) -> Result<usize> {
        if buf.is_empty() {
            return Ok(0);
        }
        let n = len_i32(buf.len())?;
        let r = unsafe { sys::uart_read(self.0, buf.as_mut_ptr(), n) };
        Ok(check(r)? as usize)
    }

    pub fn close(self) -> Result<()> {
        let h = self.0;
        core::mem::forget(self);
        check_ok(unsafe { sys::uart_close(h) })
    }
}

impl Drop for Uart {
    fn drop(&mut self) {
        unsafe {
            let _ = sys::uart_close(self.0);
        }
    }
}
