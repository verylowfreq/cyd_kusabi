//! ABI の戻り値（負の数）を Rust の `Result` に畳む。
//!
//! C 版は「負なら失敗」を呼ぶ側が毎回思い出す必要があった。ここで型に
//! させておくと、`?` で書けるうえに取りこぼしがコンパイルで分かる。

use core::fmt;

/// ABI のエラー。`cdk_wasm_abi.h` の `CDK_E_*` と対。
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
#[non_exhaustive]
pub enum Error {
    /// 引数がおかしい (-1)
    Inval,
    /// 禁止。予約ピン、または microSD の外を指すパス (-2)
    Perm,
    /// 無い (-3)
    NoEnt,
    /// 入出力の失敗 (-4)。ソケットでは「相手が閉じた」もこれ
    Io,
    /// 資源が足りない。ハンドル表が埋まっている等 (-5)
    NoMem,
    /// いまは何も無い (-6)
    Again,
    /// 未実装 (-7)。`gfx_*` は必ずこれを返す
    NoSys,
    /// 時間切れ (-8)
    Time,
    /// Ctrl-C で止められた (-9)。
    ///
    /// **実際にはまず観測できない。** ファーム側は停止要求が立つと
    /// native の入口でトラップするので、この値が戻り値として返ってくる
    /// 経路が無い。ABI の表と揃えるために持っているだけ。
    Abort,
    /// 表に無い負の値。ファームと SDK の版がずれている
    Unknown(i32),
}

impl Error {
    /// 負の戻り値を `Error` にする。
    pub fn from_code(code: i32) -> Self {
        match code {
            -1 => Error::Inval,
            -2 => Error::Perm,
            -3 => Error::NoEnt,
            -4 => Error::Io,
            -5 => Error::NoMem,
            -6 => Error::Again,
            -7 => Error::NoSys,
            -8 => Error::Time,
            -9 => Error::Abort,
            other => Error::Unknown(other),
        }
    }

    /// ABI の数値に戻す。
    pub fn code(self) -> i32 {
        match self {
            Error::Inval => -1,
            Error::Perm => -2,
            Error::NoEnt => -3,
            Error::Io => -4,
            Error::NoMem => -5,
            Error::Again => -6,
            Error::NoSys => -7,
            Error::Time => -8,
            Error::Abort => -9,
            Error::Unknown(c) => c,
        }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let s = match self {
            Error::Inval => "引数がおかしい",
            Error::Perm => "禁止されている（予約ピン / microSD の外）",
            Error::NoEnt => "見つからない",
            Error::Io => "入出力に失敗した",
            Error::NoMem => "資源が足りない",
            Error::Again => "いまは何も無い",
            Error::NoSys => "実装されていない",
            Error::Time => "時間切れ",
            Error::Abort => "止められた",
            Error::Unknown(c) => return write!(f, "不明なエラー ({c})"),
        };
        f.write_str(s)
    }
}

pub type Result<T> = core::result::Result<T, Error>;

/// 「0 以上なら成功、負なら失敗」を畳む。戻り値をそのまま返す。
pub(crate) fn check(code: i32) -> Result<i32> {
    if code < 0 {
        Err(Error::from_code(code))
    } else {
        Ok(code)
    }
}

/// 値に意味が無い（成功か失敗かだけ）呼び出し用。
pub(crate) fn check_ok(code: i32) -> Result<()> {
    check(code).map(|_| ())
}

/// スライスの長さを ABI の i32 に落とす。
///
/// **ここで弾かないとホスト側でトラップする。** 長さは u64 に広げて
/// 比べられるので、i32 に落とすときに負になった値は「巨大な長さ」として
/// 境界検査に引っかかる。アプリから見ると理由の分からない異常終了になる。
pub(crate) fn len_i32(n: usize) -> Result<i32> {
    if n > i32::MAX as usize {
        Err(Error::Inval)
    } else {
        Ok(n as i32)
    }
}
