//! microSD 上のファイル。
//!
//! **触れるのは `/sdcard` の下だけ。** 相対パスは `/sdcard` からの相対で、
//! `..` で外へ出ようとすると [`Error::Perm`]。判定はファーム側が
//! パスを畳んでから行うので、`a/../../etc` のような書き方でも抜けられない。
//!
//! 同時に開けるのは **4 つまで**（FATFS の枠のうち 1 本は本体が使う）。
//!
//! [`Error::Perm`]: crate::Error::Perm

use alloc::string::String;
use alloc::vec;
use alloc::vec::Vec;

use crate::error::{Error, Result, check, check_ok, len_i32};
use crate::sys;

/// 開き方。
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum Mode {
    /// 読むだけ。無ければ失敗。
    Read,
    /// 書く。あれば中身を捨てる。無ければ作る。
    Write,
    /// 末尾に足す。無ければ作る。
    Append,
}

impl Mode {
    fn code(self) -> i32 {
        match self {
            Mode::Read => sys::CDK_OPEN_READ,
            Mode::Write => sys::CDK_OPEN_WRITE,
            Mode::Append => sys::CDK_OPEN_APPEND,
        }
    }
}

/// `seek` の基準。
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum Seek {
    Start(i32),
    Current(i32),
    End(i32),
}

/// 開いているファイル。
///
/// `Drop` で閉じる。ただし **Ctrl-C でトラップして抜けたときは走らない**
/// （ファーム側が片づけるので漏れはしない）。
#[derive(Debug)]
pub struct File(i32);

impl File {
    fn open_mode(path: &str, mode: Mode) -> Result<File> {
        let n = len_i32(path.len())?;
        let fd = unsafe { sys::fs_open(path.as_ptr(), n, mode.code()) };
        Ok(File(check(fd)?))
    }

    /// 読むために開く。
    pub fn open(path: &str) -> Result<File> {
        File::open_mode(path, Mode::Read)
    }

    /// 書くために開く（あれば中身を捨てる）。
    pub fn create(path: &str) -> Result<File> {
        File::open_mode(path, Mode::Write)
    }

    /// 末尾に足すために開く。
    pub fn append(path: &str) -> Result<File> {
        File::open_mode(path, Mode::Append)
    }

    /// 読む。返り値が 0 なら終わり。
    pub fn read(&mut self, buf: &mut [u8]) -> Result<usize> {
        if buf.is_empty() {
            return Ok(0);
        }
        let n = len_i32(buf.len())?;
        let r = unsafe { sys::fs_read(self.0, buf.as_mut_ptr(), n) };
        Ok(check(r)? as usize)
    }

    /// 書く。返り値は実際に書けたバイト数。
    pub fn write(&mut self, buf: &[u8]) -> Result<usize> {
        if buf.is_empty() {
            return Ok(0);
        }
        let n = len_i32(buf.len())?;
        let r = unsafe { sys::fs_write(self.0, buf.as_ptr(), n) };
        Ok(check(r)? as usize)
    }

    /// 全部書けるまで繰り返す。
    pub fn write_all(&mut self, mut buf: &[u8]) -> Result<()> {
        while !buf.is_empty() {
            let n = self.write(buf)?;
            if n == 0 {
                return Err(Error::Io);
            }
            buf = &buf[n..];
        }
        Ok(())
    }

    /// 文字列を書く。
    pub fn write_str(&mut self, s: &str) -> Result<()> {
        self.write_all(s.as_bytes())
    }

    /// 終わりまで読む。
    pub fn read_to_end(&mut self) -> Result<Vec<u8>> {
        let mut out = Vec::new();
        let mut chunk = [0u8; 512];
        loop {
            let n = self.read(&mut chunk)?;
            if n == 0 {
                return Ok(out);
            }
            out.extend_from_slice(&chunk[..n]);
        }
    }

    /// 位置を動かす。返り値は動かしたあとの位置。
    pub fn seek(&mut self, to: Seek) -> Result<u32> {
        let (off, whence) = match to {
            Seek::Start(o) => (o, sys::CDK_SEEK_SET),
            Seek::Current(o) => (o, sys::CDK_SEEK_CUR),
            Seek::End(o) => (o, sys::CDK_SEEK_END),
        };
        let r = unsafe { sys::fs_seek(self.0, off, whence) };
        Ok(check(r)? as u32)
    }

    /// 明示的に閉じる。失敗を見たいときに使う（`Drop` は握り潰す）。
    pub fn close(self) -> Result<()> {
        let fd = self.0;
        core::mem::forget(self); // Drop で二重に閉じない
        check_ok(unsafe { sys::fs_close(fd) })
    }
}

impl Drop for File {
    fn drop(&mut self) {
        unsafe {
            let _ = sys::fs_close(self.0);
        }
    }
}

impl core::fmt::Write for File {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        self.write_all(s.as_bytes()).map_err(|_| core::fmt::Error)
    }
}

/// 大きさ（バイト）。
pub fn size(path: &str) -> Result<u32> {
    let n = len_i32(path.len())?;
    let r = unsafe { sys::fs_size(path.as_ptr(), n) };
    Ok(check(r)? as u32)
}

/// 消す。
pub fn remove(path: &str) -> Result<()> {
    let n = len_i32(path.len())?;
    check_ok(unsafe { sys::fs_unlink(path.as_ptr(), n) })
}

/// ディレクトリを作る。
pub fn create_dir(path: &str) -> Result<()> {
    let n = len_i32(path.len())?;
    check_ok(unsafe { sys::fs_mkdir(path.as_ptr(), n) })
}

/// 中身を全部読む。
pub fn read(path: &str) -> Result<Vec<u8>> {
    File::open(path)?.read_to_end()
}

/// 文字列として全部読む。
pub fn read_to_string(path: &str) -> Result<String> {
    String::from_utf8(read(path)?).map_err(|_| Error::Inval)
}

/// 中身を丸ごと書く。
pub fn write(path: &str, data: &[u8]) -> Result<()> {
    File::create(path)?.write_all(data)
}

/// ディレクトリの中身。名前だけを順に返す。
///
/// `Drop` で閉じる（`File` と同じく、トラップで抜けたときは走らない）。
#[derive(Debug)]
pub struct ReadDir(i32);

/// ディレクトリを開く。
pub fn read_dir(path: &str) -> Result<ReadDir> {
    let n = len_i32(path.len())?;
    let dh = unsafe { sys::fs_opendir(path.as_ptr(), n) };
    Ok(ReadDir(check(dh)?))
}

impl Iterator for ReadDir {
    type Item = String;

    fn next(&mut self) -> Option<String> {
        let mut buf = vec![0u8; sys::NAME_MAX];
        let r = unsafe { sys::fs_readdir(self.0, buf.as_mut_ptr(), buf.len() as i32) };
        // 0 は「終わり」であってエラーではない。負が本当の失敗。
        if r <= 0 {
            return None;
        }
        buf.truncate(r as usize);
        String::from_utf8(buf).ok()
    }
}

impl Drop for ReadDir {
    fn drop(&mut self) {
        unsafe {
            let _ = sys::fs_closedir(self.0);
        }
    }
}
