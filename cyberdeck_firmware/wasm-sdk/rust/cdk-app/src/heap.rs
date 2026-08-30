//! バンプ確保。**解放しない。**
//!
//! C 版の `cdk_alloc` と同じ考え方で、リンカが置いた静的データの終わり
//! (`__heap_base`) から順に配る。アプリは短命なので、これで足りるうちは足りる。
//!
//! 足りなくなったら `memory.grow` を試す。それも駄目なら確保が失敗し、
//! `handle_alloc_error` からパニックへ落ちる。パニックハンドラが理由を
//! 端末に出すので、**黙って止まることはない。**
//!
//! ## 上限
//!
//! ファーム側が線形メモリを **1MB (16 ページ)** に制限している。
//! そこを超える `memory.grow` はホストが断るので、ここで伸ばせるのも実質そこまで。
//!
//! ## なぜ AtomicUsize なのか
//!
//! エディション 2024 では `static mut` への参照がエラーになる。
//! `wasm32-unknown-unknown` にスレッドは無いので排他の意味は無く、
//! `static mut` を避けるためだけに使っている。

use core::alloc::{GlobalAlloc, Layout};
use core::sync::atomic::{AtomicUsize, Ordering};

unsafe extern "C" {
    /// リンカが置く、静的データの終わり。**アドレスがヒープの先頭。**
    static __heap_base: u8;
}

const PAGE: usize = 65536;

/// 次に配る位置（0 = まだ初期化していない）。
static NEXT: AtomicUsize = AtomicUsize::new(0);

fn heap_start() -> usize {
    (&raw const __heap_base) as usize
}

/// いま確保されている線形メモリの終わり。
fn memory_end() -> usize {
    core::arch::wasm32::memory_size(0) * PAGE
}

pub struct Bump;

unsafe impl GlobalAlloc for Bump {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let align = layout.align().max(1);
        let size = layout.size();
        if size == 0 {
            return align as *mut u8; // 0 バイトでも整列した非 NULL を返す約束
        }

        let mut cur = NEXT.load(Ordering::Relaxed);
        if cur == 0 {
            cur = heap_start();
        }

        // 整列させる。溢れたら諦める。
        let Some(aligned) = cur.checked_add(align - 1) else {
            return core::ptr::null_mut();
        };
        let start = aligned & !(align - 1);
        let Some(end) = start.checked_add(size) else {
            return core::ptr::null_mut();
        };

        // 足りなければ伸ばす。伸ばせなければ失敗（呼び出し側がパニックする）。
        let have = memory_end();
        if end > have {
            let need = end - have;
            let pages = need.div_ceil(PAGE);
            if core::arch::wasm32::memory_grow(0, pages) == usize::MAX {
                return core::ptr::null_mut();
            }
        }

        NEXT.store(end, Ordering::Relaxed);
        start as *mut u8
    }

    unsafe fn dealloc(&self, _ptr: *mut u8, _layout: Layout) {
        // 返さない。アプリは短命で、走り終わればモジュールごと捨てられる。
    }
}

/// いままでに配ったバイト数。自分の使用量を出したいアプリのために公開する。
pub fn used() -> usize {
    let cur = NEXT.load(Ordering::Relaxed);
    if cur == 0 { 0 } else { cur - heap_start() }
}
