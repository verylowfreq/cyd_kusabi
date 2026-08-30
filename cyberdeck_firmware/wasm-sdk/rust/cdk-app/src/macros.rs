//! `print!` / `println!` / `main!`。

/// 端末へ書く。改行は付かない。
#[macro_export]
macro_rules! print {
    ($($arg:tt)*) => {{
        use core::fmt::Write as _;
        let _ = write!($crate::con::Console, $($arg)*);
    }};
}

/// 端末へ 1 行書く。
///
/// **末尾は `\r\n`。** 端末は CR が無いと行頭に戻らないので、
/// `\n` だけで済ませると字が階段状に流れていく。そこを SDK が吸収する。
#[macro_export]
macro_rules! println {
    () => {{
        let _ = $crate::con::write(b"\r\n");
    }};
    ($($arg:tt)*) => {{
        use core::fmt::Write as _;
        let _ = write!($crate::con::Console, $($arg)*);
        let _ = $crate::con::write(b"\r\n");
    }};
}

/// アプリの入口を作る。
///
/// ```ignore
/// #![no_std]
/// extern crate alloc;
///
/// cdk_app::main!(run);
///
/// fn run() -> cdk_app::Result<()> {
///     cdk_app::println!("やあ");
///     Ok(())
/// }
/// ```
///
/// 渡した関数が `Err` を返したら**理由を端末に出して**終了コード 1 で返る。
/// C 版だと戻り値を見忘れて黙って進むことがあったので、ここで面倒を見る。
#[macro_export]
macro_rules! main {
    ($run:path) => {
        #[unsafe(no_mangle)]
        pub extern "C" fn cdk_main(_argc: i32) -> i32 {
            let f: fn() -> $crate::Result<()> = $run;
            match f() {
                Ok(()) => 0,
                Err(e) => {
                    $crate::println!("\x1b[31mエラー: {}\x1b[0m", e);
                    1
                }
            }
        }
    };
}
