//! 生の ABI 宣言。**このファイルだけが `cdk` モジュールの形を知っている。**
//!
//! 上の安全な層はここを通してしか外に出ない。C 版の
//! `wasm-sdk/include/cdk_app.h` と 1 対 1 で対応していて、名前・順番・型を
//! そのまま写してある。ずれると実機でしか出ない壊れ方をするので、
//! ctest `abi_names_match` が 3 か所（ファームの表・C ヘッダ・ここ）の
//! 名前が一致することを機械的に見ている。
//!
//! ## 引数の約束
//!
//! - ポインタはゲストの線形メモリのオフセット（wasm32 なので i32）。
//! - `(ptr, len)` は**隣接かつこの順**。`sock_connect` だけ長さが `port` の
//!   前に来る、`i2c_wrrd` は書き込みの組と読み出しの組が続く、といった
//!   並びまで含めてヘッダに合わせてある。
//! - 長さは i32。**負の長さを渡すとホスト側で u64 に広げられて必ずトラップする**
//!   （`m3ApiCheckMem`）。安全な層で必ず検査すること。
//!
//! ## 戻ってこない呼び出し
//!
//! `sys_exit` は必ずトラップする。`sys_delay` と `con_write` は Ctrl-C が
//! 来たときにトラップする。さらにファーム側は停止要求が立ったあと
//! **どの native でも入口でトラップする**ので、止められたあとに正常に
//! 戻ってくる呼び出しは無い。**巻き戻ると `Drop` は走らない。**

#[link(wasm_import_module = "cdk")]
unsafe extern "C" {
    // ---- sys ----
    /// 終了する。**戻ってこない**（ホストがトラップする）。
    pub fn sys_exit(code: i32) -> !;
    /// 起動からの経過ミリ秒。**ABI で唯一 i64 を返す。**
    pub fn sys_now_ms() -> i64;
    /// 待つ。Ctrl-C が来たらここでトラップして戻ってこない。
    pub fn sys_delay(ms: i32) -> i32;
    pub fn sys_argc() -> i32;
    /// `out` に引数を書いて長さを返す。
    pub fn sys_arg(idx: i32, out: *mut u8, cap: i32) -> i32;
    pub fn sys_random(out: *mut u8, len: i32) -> i32;

    // ---- コンソール ----
    /// 端末へ。UTF-8 と VT エスケープ列はそのまま通る。
    /// リングが埋まっていると空くまで待ち、待っている間に Ctrl-C が来ると
    /// トラップする。
    pub fn con_write(p: *const u8, n: i32) -> i32;
    /// **待たない。** 何も無ければ 0。中身は普通の端末のバイト列。
    pub fn con_read(p: *mut u8, n: i32) -> i32;
    /// 桁数と行数。4 バイトの出力ポインタを 2 本取る（ホストが LE で書く）。
    pub fn con_size(cols: *mut u32, rows: *mut u32) -> i32;

    // ---- ファイル ----
    pub fn fs_open(path: *const u8, plen: i32, mode: i32) -> i32;
    pub fn fs_read(fd: i32, p: *mut u8, n: i32) -> i32;
    pub fn fs_write(fd: i32, p: *const u8, n: i32) -> i32;
    pub fn fs_seek(fd: i32, off: i32, whence: i32) -> i32;
    pub fn fs_close(fd: i32) -> i32;
    pub fn fs_size(path: *const u8, plen: i32) -> i32;
    pub fn fs_unlink(path: *const u8, plen: i32) -> i32;
    pub fn fs_mkdir(path: *const u8, plen: i32) -> i32;
    pub fn fs_opendir(path: *const u8, plen: i32) -> i32;
    /// 名前を書いて長さを返す。**0 は「終わり」でエラーではない。**
    pub fn fs_readdir(dh: i32, out: *mut u8, cap: i32) -> i32;
    pub fn fs_closedir(dh: i32) -> i32;

    // ---- GPIO / ADC ----
    pub fn gpio_mode(pin: i32, mode: i32) -> i32;
    pub fn gpio_set(pin: i32, level: i32) -> i32;
    pub fn gpio_get(pin: i32) -> i32;
    /// ミリボルト。
    pub fn adc_read(ch: i32) -> i32;

    // ---- I2C ----
    pub fn i2c_open(sda: i32, scl: i32, hz: i32) -> i32;
    pub fn i2c_write(addr: i32, p: *const u8, n: i32) -> i32;
    pub fn i2c_read(addr: i32, p: *mut u8, n: i32) -> i32;
    /// 書いてから読む（リピートスタート）。レジスタ読みはこれ。
    pub fn i2c_wrrd(addr: i32, w: *const u8, wn: i32, r: *mut u8, rn: i32) -> i32;
    pub fn i2c_close() -> i32;

    // ---- UART ----
    pub fn uart_open(tx: i32, rx: i32, baud: i32) -> i32;
    pub fn uart_write(h: i32, p: *const u8, n: i32) -> i32;
    /// **待たない。** 何も無ければ 0。
    pub fn uart_read(h: i32, p: *mut u8, n: i32) -> i32;
    pub fn uart_close(h: i32) -> i32;

    // ---- ネットワーク ----
    pub fn net_up() -> i32;
    pub fn sock_open(ty: i32) -> i32;
    /// **長さが `port` の前に来る。** ヘッダの並びに合わせてある。
    pub fn sock_connect(sd: i32, host: *const u8, hlen: i32, port: i32) -> i32;
    pub fn sock_send(sd: i32, p: *const u8, n: i32) -> i32;
    /// 200ms 待って何も来なければ 0。相手が閉じたら `CDK_E_IO`。
    pub fn sock_recv(sd: i32, p: *mut u8, n: i32) -> i32;
    pub fn sock_close(sd: i32) -> i32;

    // ---- 画面描画（予約。いまはどれも CDK_E_NOSYS を返す） ----
    pub fn gfx_size(w: *mut u32, h: *mut u32) -> i32;
    pub fn gfx_clear(rgb565: i32) -> i32;
    pub fn gfx_fill_rect(x: i32, y: i32, w: i32, h: i32, rgb565: i32) -> i32;
    pub fn gfx_blit(x: i32, y: i32, w: i32, h: i32, px: *const u8, n: i32) -> i32;
    pub fn gfx_text(x: i32, y: i32, s: *const u8, n: i32, fg: i32, bg: i32) -> i32;
    pub fn gfx_present() -> i32;
}

// ---- 定数。cdk_wasm_abi.h と対で持つ ----

pub const CDK_OPEN_READ: i32 = 0;
pub const CDK_OPEN_WRITE: i32 = 1;
pub const CDK_OPEN_APPEND: i32 = 2;

pub const CDK_SEEK_SET: i32 = 0;
pub const CDK_SEEK_CUR: i32 = 1;
pub const CDK_SEEK_END: i32 = 2;

pub const CDK_GPIO_IN: i32 = 0;
pub const CDK_GPIO_IN_PU: i32 = 1;
pub const CDK_GPIO_IN_PD: i32 = 2;
pub const CDK_GPIO_OUT: i32 = 3;

pub const CDK_SOCK_TCP: i32 = 0;
pub const CDK_SOCK_UDP: i32 = 1;

/// ファームが受け取る `.wasm` の上限（`CDK_WASM_MODULE_MAX`）。
pub const MODULE_MAX: usize = 256 * 1024;
/// 同時に開けるファイル（`CDK_WASM_FILES_MAX`）。
pub const FILES_MAX: usize = 4;
/// `fs_readdir` が返す名前の上限（`CDK_WASM_NAME_MAX`）。
pub const NAME_MAX: usize = 128;
/// パスの上限（`CDK_WASM_PATH_MAX`）。
pub const PATH_MAX: usize = 192;
/// 引数の数と合計バイト数（`CDK_WASM_ARGV_MAX` / `_BYTES`）。
pub const ARGV_MAX: usize = 8;
pub const ARGV_BYTES: usize = 256;
