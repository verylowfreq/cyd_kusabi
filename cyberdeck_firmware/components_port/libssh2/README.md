# libssh2 (vendored)

- 版: **1.11.1**
- 入手元: https://www.libssh2.org/download/libssh2-1.11.1.tar.gz
- ライセンス: BSD-3-Clause（`upstream/COPYING`）

`upstream/` は配布物の `src/` と `include/` を**丸ごと**置いたもの。
ビルド対象は `CMakeLists.txt` の一覧（上流 `src/Makefile.inc` の CSOURCES）だけだが、
ファイルは削らないこと。`agent.c` は `agent_win.c` を**無条件に** `#include` し、
`crypto.c` はバックエンドの `.c` を `#include` する。中身側で `#ifdef` 分岐している。

## なぜ wolfSSH ではないのか

wolfSSH のクライアント側 PTY 要求は
`#if defined(WOLFSSH_TERM) && !defined(NO_FILESYSTEM)` の中にあり、
ベンダー純正の ESP32 用 `user_settings.h` が `NO_FILESYSTEM` を定義しているため
既定ではコンパイル対象外になる。有効化しても `GetTerminalInfo()` が
非 POSIX ターゲットでは 80x24 と "xterm" をハードコードする。

libssh2 は `libssh2_channel_request_pty_ex()` が端末名とサイズを引数で取るので、
実機の 53x20 をそのまま渡せる。詳細は DESIGN.md §8.2。

## 設定

`libssh2_config.h` がこのプロジェクト向けの設定。
上流の autotools / CMake が生成するものを手書きで置き換えている。
バックエンドは mbedTLS（ESP-IDF 同梱、ESP32-S3 のハード暗号が効く）。

## 更新のしかた

```
curl -LO https://www.libssh2.org/download/libssh2-<ver>.tar.gz
tar xzf libssh2-<ver>.tar.gz
cp libssh2-<ver>/src/*.c libssh2-<ver>/src/*.h upstream/src/
cp libssh2-<ver>/include/*.h upstream/include/
cp libssh2-<ver>/COPYING upstream/
```

そのあと `src/Makefile.inc` の CSOURCES と `CMakeLists.txt` の一覧を突き合わせること。
