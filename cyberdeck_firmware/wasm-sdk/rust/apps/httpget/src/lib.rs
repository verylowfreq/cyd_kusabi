//! 素の TCP で HTTP GET する。`wasm run apps/rs_httpget.wasm <host> [path]`
//!
//! TLS は無いので http のみ。C 版は SDK に snprintf が無く、リクエスト行を
//! 1 バイトずつ組み立てていた。こちらは `format!` で済む。
#![no_std]

extern crate alloc;

use alloc::string::String;
use cdk_app::prelude::*;

cdk_app::main!(run);

fn run() -> Result<()> {
    let Ok(host) = args::get(1) else {
        println!("使い方: rs_httpget.wasm <host> [path]");
        return Ok(());
    };
    let path = args::get(2).unwrap_or_else(|_| String::from("/"));

    if !net::is_up() {
        println!("WiFi が繋がっていません");
        return Err(Error::Io);
    }

    println!("{host} へ接続中...");
    let mut sock = net::TcpStream::connect(&host, 80)?;

    let req = alloc::format!(
        "GET {path} HTTP/1.0\r\nHost: {host}\r\nConnection: close\r\n\r\n"
    );
    sock.write_all(req.as_bytes())?;

    // 相手が閉じるまで読み、届いたそばから画面へ出す。
    let mut buf = [0u8; 256];
    let mut total = 0usize;
    loop {
        match sock.read(&mut buf) {
            Ok(0) => time::delay(20), // まだ来ていない
            Ok(n) => {
                con::write(&buf[..n])?;
                total += n;
            }
            Err(Error::Io) => break, // 相手が閉じた
            Err(e) => return Err(e),
        }
    }
    println!();
    println!("--- {total} バイト ---");
    Ok(())
}
