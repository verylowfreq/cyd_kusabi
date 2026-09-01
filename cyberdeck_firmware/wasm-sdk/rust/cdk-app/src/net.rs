//! 素の TCP / UDP ソケット。
//!
//! SSH とは別物。あちらはファームが 1 本だけ握る専用の口で、こちらは
//! アプリが自由に開ける（同時に 4 本まで）。
//!
//! **TLS はファーム側 (ESP-IDF) が終端する。** `TcpStream::connect_tls` で
//! 開けば、あとは素の TCP と同じ `write` / `read` が使える。TLS は同時 2 本まで。
//!
//! どの呼び出しも有限時間で戻る。`connect` は 10 秒で時間切れ、
//! `read` は 200ms 待って何も無ければ 0 を返す。

use alloc::vec::Vec;

use crate::error::{Error, Result, check, check_ok, len_i32};
use crate::sys;

/// WiFi が繋がっているか。
pub fn is_up() -> bool {
    unsafe { sys::net_up() == 1 }
}

/// 繋がっている TCP。`Drop` で閉じる。
#[derive(Debug)]
pub struct TcpStream(i32);

impl TcpStream {
    /// 名前を解決して繋ぐ。10 秒で時間切れ。
    ///
    /// 待っている間に Ctrl-C が来ると、そこで止まって戻ってこない。
    pub fn connect(host: &str, port: u16) -> Result<TcpStream> {
        Self::connect_with(host, port, sys::CDK_SOCK_TCP)
    }

    /// TLS で繋ぐ。証明書はファームが持つバンドルで検証する。
    ///
    /// 中身の TLS はファーム側 (ESP-IDF) が終端するので、繋がったあとは
    /// 素の TCP と同じ `write` / `read` が使える。**同時 2 本まで。**
    pub fn connect_tls(host: &str, port: u16) -> Result<TcpStream> {
        Self::connect_with(host, port, sys::CDK_SOCK_TLS)
    }

    /// 証明書を検証せずに TLS で繋ぐ。
    ///
    /// **自己署名の相手を試すとき以外に使わないこと。** 相手が誰であっても
    /// 繋がってしまう。
    pub fn connect_tls_insecure(host: &str, port: u16) -> Result<TcpStream> {
        Self::connect_with(host, port, sys::CDK_SOCK_TLS_INSECURE)
    }

    fn connect_with(host: &str, port: u16, ty: i32) -> Result<TcpStream> {
        if !is_up() {
            return Err(Error::Io);
        }
        let sd = check(unsafe { sys::sock_open(ty) })?;
        let s = TcpStream(sd); // ここから先で失敗しても Drop が閉じる

        let hn = len_i32(host.len())?;
        check_ok(unsafe { sys::sock_connect(sd, host.as_ptr(), hn, i32::from(port)) })?;
        Ok(s)
    }

    pub fn write(&mut self, data: &[u8]) -> Result<usize> {
        if data.is_empty() {
            return Ok(0);
        }
        let n = len_i32(data.len())?;
        let r = unsafe { sys::sock_send(self.0, data.as_ptr(), n) };
        Ok(check(r)? as usize)
    }

    pub fn write_all(&mut self, mut data: &[u8]) -> Result<()> {
        while !data.is_empty() {
            let n = self.write(data)?;
            if n == 0 {
                return Err(Error::Io);
            }
            data = &data[n..];
        }
        Ok(())
    }

    /// 読む。**`Ok(0)` は「まだ来ていない」**で、切断ではない。
    /// 相手が閉じたときは [`Error::Io`]。
    pub fn read(&mut self, buf: &mut [u8]) -> Result<usize> {
        if buf.is_empty() {
            return Ok(0);
        }
        let n = len_i32(buf.len())?;
        let r = unsafe { sys::sock_recv(self.0, buf.as_mut_ptr(), n) };
        Ok(check(r)? as usize)
    }

    /// 相手が閉じるまで読み切る。
    ///
    /// `Ok(0)`（まだ来ていない）のときは少し待って読み直す。
    pub fn read_to_end(&mut self) -> Result<Vec<u8>> {
        let mut out = Vec::new();
        let mut chunk = [0u8; 512];
        loop {
            match self.read(&mut chunk) {
                Ok(0) => crate::time::delay(20),
                Ok(n) => out.extend_from_slice(&chunk[..n]),
                // 相手が閉じた。ここまで読めたぶんを返す。
                Err(Error::Io) => return Ok(out),
                Err(e) => return Err(e),
            }
        }
    }

    pub fn close(self) -> Result<()> {
        let sd = self.0;
        core::mem::forget(self);
        check_ok(unsafe { sys::sock_close(sd) })
    }
}

impl Drop for TcpStream {
    fn drop(&mut self) {
        unsafe {
            let _ = sys::sock_close(self.0);
        }
    }
}

/// UDP。`Drop` で閉じる。
#[derive(Debug)]
pub struct UdpSocket(i32);

impl UdpSocket {
    /// 相手を決めて開く（`connect` 済みの UDP として使う）。
    pub fn connect(host: &str, port: u16) -> Result<UdpSocket> {
        if !is_up() {
            return Err(Error::Io);
        }
        let sd = check(unsafe { sys::sock_open(sys::CDK_SOCK_UDP) })?;
        let s = UdpSocket(sd);

        let hn = len_i32(host.len())?;
        check_ok(unsafe { sys::sock_connect(sd, host.as_ptr(), hn, i32::from(port)) })?;
        Ok(s)
    }

    pub fn send(&mut self, data: &[u8]) -> Result<usize> {
        let n = len_i32(data.len())?;
        let r = unsafe { sys::sock_send(self.0, data.as_ptr(), n) };
        Ok(check(r)? as usize)
    }

    /// **`Ok(0)` は「まだ来ていない」。**
    pub fn recv(&mut self, buf: &mut [u8]) -> Result<usize> {
        let n = len_i32(buf.len())?;
        let r = unsafe { sys::sock_recv(self.0, buf.as_mut_ptr(), n) };
        Ok(check(r)? as usize)
    }
}

impl Drop for UdpSocket {
    fn drop(&mut self) {
        unsafe {
            let _ = sys::sock_close(self.0);
        }
    }
}
