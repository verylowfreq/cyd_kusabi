//! I2C。**同時に開けるのは 1 本だけ。**
//!
//! ピンは予約表を通る（液晶や SD が使っているピンは [`Error::Perm`]）。
//!
//! [`Error::Perm`]: crate::Error::Perm

use crate::error::{Result, check_ok, len_i32};
use crate::sys;

/// 開いている I2C バス。`Drop` で閉じる。
#[derive(Debug)]
pub struct I2c(());

impl I2c {
    /// バスを開く。`hz` は 1..=1_000_000。
    pub fn open(sda: i32, scl: i32, hz: u32) -> Result<I2c> {
        let hz = len_i32(hz as usize)?;
        check_ok(unsafe { sys::i2c_open(sda, scl, hz) })?;
        Ok(I2c(()))
    }

    /// 標準の 100kHz で開く。
    pub fn open_default(sda: i32, scl: i32) -> Result<I2c> {
        I2c::open(sda, scl, 100_000)
    }

    pub fn write(&mut self, addr: u8, data: &[u8]) -> Result<()> {
        let n = len_i32(data.len())?;
        check_ok(unsafe { sys::i2c_write(i32::from(addr), data.as_ptr(), n) })
    }

    pub fn read(&mut self, addr: u8, buf: &mut [u8]) -> Result<()> {
        let n = len_i32(buf.len())?;
        check_ok(unsafe { sys::i2c_read(i32::from(addr), buf.as_mut_ptr(), n) })
    }

    /// 書いてから読む（リピートスタート）。**レジスタ読みはこれ。**
    pub fn write_read(&mut self, addr: u8, w: &[u8], r: &mut [u8]) -> Result<()> {
        let wn = len_i32(w.len())?;
        let rn = len_i32(r.len())?;
        check_ok(unsafe {
            sys::i2c_wrrd(i32::from(addr), w.as_ptr(), wn, r.as_mut_ptr(), rn)
        })
    }

    /// レジスタを 1 バイト読む。
    pub fn read_reg(&mut self, addr: u8, reg: u8) -> Result<u8> {
        let mut b = [0u8; 1];
        self.write_read(addr, &[reg], &mut b)?;
        Ok(b[0])
    }

    /// レジスタに 1 バイト書く。
    pub fn write_reg(&mut self, addr: u8, reg: u8, val: u8) -> Result<()> {
        self.write(addr, &[reg, val])
    }

    /// そのアドレスに誰か居るか。1 バイト読めるかで判定する
    /// （0 バイトの書き込みは通らないため）。
    pub fn probe(&mut self, addr: u8) -> bool {
        let mut b = [0u8; 1];
        self.read(addr, &mut b).is_ok()
    }

    /// 明示的に閉じる。失敗を見たいときに使う。
    pub fn close(self) -> Result<()> {
        core::mem::forget(self);
        check_ok(unsafe { sys::i2c_close() })
    }
}

impl Drop for I2c {
    fn drop(&mut self) {
        unsafe {
            let _ = sys::i2c_close();
        }
    }
}
