//! GPIO と ADC。
//!
//! **基板が使っているピンは触れない。** 液晶・microSD・USB・フラッシュ・
//! PC との通信に使っているピンを指定すると [`Error::Perm`] が返る。
//! どのピンが埋まっているかは基板で違う（ファーム側が `cdk_board.h` を
//! 唯一の出どころにして判定する）。
//!
//! [`Error::Perm`]: crate::Error::Perm

use crate::error::{Result, check, check_ok};
use crate::sys;

/// ピンの向き。
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum Mode {
    Input,
    InputPullUp,
    InputPullDown,
    Output,
}

impl Mode {
    fn code(self) -> i32 {
        match self {
            Mode::Input => sys::CDK_GPIO_IN,
            Mode::InputPullUp => sys::CDK_GPIO_IN_PU,
            Mode::InputPullDown => sys::CDK_GPIO_IN_PD,
            Mode::Output => sys::CDK_GPIO_OUT,
        }
    }
}

/// 1 本のピン。
///
/// 出力にしたピンは読み戻せる（ファーム側が INPUT_OUTPUT にしている）。
#[derive(Copy, Clone, Debug)]
pub struct Pin(i32);

impl Pin {
    /// 向きを決めて掴む。**予約ピンならここで失敗する。**
    pub fn new(pin: i32, mode: Mode) -> Result<Pin> {
        check_ok(unsafe { sys::gpio_mode(pin, mode.code()) })?;
        Ok(Pin(pin))
    }

    /// 出力として掴む。
    pub fn output(pin: i32) -> Result<Pin> {
        Pin::new(pin, Mode::Output)
    }

    /// 入力として掴む。
    pub fn input(pin: i32) -> Result<Pin> {
        Pin::new(pin, Mode::Input)
    }

    /// 途中で向きを変える。
    pub fn set_mode(&self, mode: Mode) -> Result<()> {
        check_ok(unsafe { sys::gpio_mode(self.0, mode.code()) })
    }

    pub fn set(&self, high: bool) -> Result<()> {
        check_ok(unsafe { sys::gpio_set(self.0, i32::from(high)) })
    }

    pub fn high(&self) -> Result<()> {
        self.set(true)
    }

    pub fn low(&self) -> Result<()> {
        self.set(false)
    }

    pub fn get(&self) -> Result<bool> {
        let r = unsafe { sys::gpio_get(self.0) };
        Ok(check(r)? != 0)
    }

    /// いまと逆にする。
    pub fn toggle(&self) -> Result<()> {
        let v = self.get()?;
        self.set(!v)
    }

    /// ピン番号。
    pub fn number(&self) -> i32 {
        self.0
    }
}

/// ADC をミリボルトで読む。
///
/// 校正が使えないチャネルでは [`Error::Io`] になる（生値は返さない。
/// 使えないと分かったほうがアプリ側で分岐できる）。
///
/// [`Error::Io`]: crate::Error::Io
pub fn adc_mv(channel: i32) -> Result<u32> {
    let r = unsafe { sys::adc_read(channel) };
    Ok(check(r)? as u32)
}
