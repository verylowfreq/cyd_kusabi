# 上流 wasm3 に当てた差分

いまのところ **無し**。`upstream/` は取り込んだままで、コミットは
`upstream/COMMIT.txt`（`2b4a5fa4c35def3552a9283c4f7aab39a93f33e9` /
2026-08-27 / `Allow non-global WASI contexts. (#331)`）。

差分を当てるときは、ここに **何を・なぜ** を書く。上流を黙って書き換えると、
入れ替えたときに理由ごと消える（libssh2 で 1 度やって壊した）。

## 検討したが当てなかったもの

### `d_m3FixedHeap` によるアリーナ化

`m3_core.c` の `d_m3FixedHeap` は **static 配列 + リセットできない bump
アロケータ**で、`m3_Free_Impl` は最後の 1 個しか返さない。`wasm run` を
繰り返すたびに食い潰して戻らないので、この用途には使えない。

いまは既定の `calloc` / `free` のまま。`m3_FreeRuntime` /
`m3_FreeEnvironment` / `m3_FreeModule` で全部返る。ESP-IDF は
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`（既定 16KB）を超える確保を自動で
PSRAM に回すので、大物（コードページ・線形メモリ）は PSRAM に落ちる。

**判定は実測でする。** `wasm run` を 10 回繰り返して `free` の internal
空きと最大ブロックが戻らなければ、そのときにアリーナ化を入れる。
測る前に上流をいじらない。
