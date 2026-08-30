//! パニックの理由を端末に出してから止まる。
//!
//! **これが Rust を足す一番の実利。** C 版だと配列の外を踏んでも
//! ファームが `落ちました: out of bounds memory access` と言うだけで、
//! アプリのどこが悪いのかは分からない。ここで `file:line` と
//! メッセージを出しておけば、実機の画面だけで原因まで辿れる。
//!
//! 出したあとは `unreachable` でトラップする。ファーム側はそれを
//! 「落ちました」として報告し、開いていたファイルなどを片づける。

use core::fmt::Write as _;
use core::panic::PanicInfo;

use crate::con;

#[panic_handler]
fn on_panic(info: &PanicInfo) -> ! {
    // 端末が生きていないかもしれないので、失敗は全部握り潰す。
    // ここで二重にパニックすると理由が何も出なくなる。
    let _ = con::write(b"\r\n\x1b[31mpanic: ");

    let mut out = con::Console;
    let _ = write!(out, "{}", info.message());

    if let Some(loc) = info.location() {
        let _ = write!(out, " ({}:{})", loc.file(), loc.line());
    }
    let _ = con::write(b"\x1b[0m\r\n");

    core::arch::wasm32::unreachable()
}
