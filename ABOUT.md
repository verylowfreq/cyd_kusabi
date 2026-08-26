## 概要

ESP32-S3でサイバーデックを作るプロジェクト

## 利用シチュエーション

- 簡易なSSH端末に
- SSH接続先のホストで実行されるAIエージェントの指示端末
- マイコンであることを活かして、ローレベルな通信やピン操作が可能な端末
- 日本語ワープロ

## 機能

- WiFiでインターネット接続
- USBキーボードから入力を受け付ける
- ディスプレイにコンソールを表示
- SSH端末
- SKK日本語入力
- コマンドラインの簡易なシェル風環境
- コマンドでGPIO、I2C、ADC、UARTなどが操作できる
- microSDをファイルシステムとして利用

## ハードウェア

- ESP32-S3 + PSRAM
- USBキーボード
- microSD
- SPI接続の液晶ディスプレイ。320x240もしくは480x320

## 開発環境

ESP-IDF v5.5.5 ( C:\DATA-0\Espressif\esp-idf-v5.5.5 )

## ターミナル機能

目標はCodexやClaude CodeのCLIが動くこと。下記で省略可能なものは削って良い。

CR、LF、BS、TAB
カーソル移動
画面消去
行消去
SGR色
カーソル位置設定
保存・復元
スクロール
UTF-8。ただし実際に扱うのはShiftJISの範囲でも良い。
全角幅

## SKK日本語入力

辞書はとりあえずSKK-JISYO.Lを利用する。あとで差し替えられるように。
実機では辞書ファイルを直接扱わず、パソコン上でいったん変換を挟んでも良い。

## 開発進行

まずはM5Stack CoreS3で試作をする。この時点ではUSBキーボード対応は不要。AIエージェントが自律的にコーディング、実行、デバッグできるようにする。ディスプレイ表示も検証できるように、ディスプレイの表示画像をファイルに書き出してパソコンへ転送する仕組みを入れる。
ESP32-S3実機ではなくパソコン上で検証できるところは積極的にパソコン上で検証する。

## テスト用WiFiネットワーク

SSID: TEST_WIFI_SSID
PSK: TEST_WIIF_PSK

## テスト用SSHサーバー

Host: TEST_SSHSERVER_HOST
UserName: TEST_SSHSERVER_USERNAME
Password: TEST_SSHSERVER_PASSWORD

## ホストのコンパイラ・インタプリタ

Rust
clang ( C:\Program Files\LLVM\bin )
Visual Studio 2022 ( C:\Program Files\Microsoft Visual Studio\2022\Community )
Python

## 決めてほしいこと

- 機能の開発順序
- 機能ごとの詳細
- 実装困難・不能な箇所についての指摘と代替案