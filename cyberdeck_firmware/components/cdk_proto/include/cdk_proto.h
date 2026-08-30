/* cdk_proto — ホストPC と デバイス の間のフレーミングプロトコル
 *
 * ESP-IDF に依存しない純 C。ホスト側テストからも同じソースを使う。
 *
 * フレーム形式:
 *   magic(4) = 0x1B 'C' 'D' 'K'
 *   type(1)
 *   len(4, little endian)   payload のバイト数
 *   payload(len)
 *   crc32(4, little endian) magic〜payload までの CRC32 (IEEE)
 *
 * 受信側は必ず再同期できること。magic を見つけても CRC が合わなければ
 * 1 バイト捨てて magic を探し直す。len を盲信して読み飛ばさない。
 */
#ifndef CDK_PROTO_H
#define CDK_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CDK_PROTO_MAGIC_0 0x1Bu
#define CDK_PROTO_MAGIC_1 0x43u /* 'C' */
#define CDK_PROTO_MAGIC_2 0x44u /* 'D' */
#define CDK_PROTO_MAGIC_3 0x4Bu /* 'K' */

#define CDK_PROTO_MAGIC_SIZE   4u
#define CDK_PROTO_HEADER_SIZE  9u /* magic + type + len */
#define CDK_PROTO_TRAILER_SIZE 4u /* crc32 */
#define CDK_PROTO_OVERHEAD     (CDK_PROTO_HEADER_SIZE + CDK_PROTO_TRAILER_SIZE)

/* 1 フレームの payload 上限。スクリーンショットはこの単位に分割して送る。 */
#define CDK_PROTO_MAX_PAYLOAD 4096u

/* 受信バッファに必要な最小サイズ */
#define CDK_PROTO_RX_BUF_MIN (CDK_PROTO_OVERHEAD + CDK_PROTO_MAX_PAYLOAD)

typedef enum {
    CDK_MSG_PING      = 0x01, /* host -> dev : payload 任意 */
    CDK_MSG_PONG      = 0x02, /* dev -> host : PING の payload をそのまま返す */
    CDK_MSG_READY     = 0x03, /* dev -> host : 起動完了。payload = バージョン文字列 */
    CDK_MSG_LOG       = 0x04, /* dev -> host : 1 チャネル縮退モードでのログ行 */

    CDK_MSG_KEY       = 0x10, /* host -> dev : cdk_wire_key_t */
    CDK_MSG_TEXT      = 0x11, /* host -> dev : UTF-8 テキストをそのまま入力扱い */
    /* host -> dev : バイト列を**端末へ直接**流す（入力ではない）。
     * 画面を確定的な状態に置くための検証用。TEXT はキー入力として
     * シェルや SSH に届くので、画面をホストと突き合わせる用途には使えない。 */
    CDK_MSG_VT        = 0x12,

    /* host -> dev : microSD へファイルを書く。
     * SD を抜き差しせずにユーザーアプリ (.wasm) を更新するための口で、
     * これが無いと自律ループが回らない（実機に置く手段が人手しかない）。
     *
     *   FILE_BEGIN  payload = u32le 全体の長さ + パス (UTF-8, NUL なし)
     *   FILE_DATA   payload = 中身をそのまま（何回でも）
     *   FILE_END    payload = u32le 中身全体の CRC32
     *   FILE_OK     dev -> host : u32le 実際に書いたバイト数
     *
     * フレームごとの CRC は transport の壊れを見るもので、**フレームが
     * まるごと落ちたことは見つけられない。** 全体の CRC を最後に突き合わせる。 */
    CDK_MSG_FILE_BEGIN = 0x30,
    CDK_MSG_FILE_DATA  = 0x31,
    CDK_MSG_FILE_END   = 0x32,
    CDK_MSG_FILE_OK    = 0x33,

    CDK_MSG_SHOT_REQ  = 0x20, /* host -> dev : payload なし */
    CDK_MSG_SHOT_INFO = 0x21, /* dev -> host : cdk_wire_shot_info_t */
    CDK_MSG_SHOT_DATA = 0x22, /* dev -> host : 先頭 u32le = バイトオフセット、以降ピクセル */
    CDK_MSG_SHOT_END  = 0x23, /* dev -> host : payload なし */

    CDK_MSG_ERROR     = 0x7Fu /* dev -> host : UTF-8 のエラーメッセージ */
} cdk_msg_type_t;

/* SHOT_INFO の payload。すべて little endian。 */
typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t  format; /* CDK_PIXFMT_* */
    /* 実機が掛けているガンマ補正（10 倍の整数、10 = 無補正）。
     * **ホストの基準レンダラに同じ値を渡すために要る。** これが無いと、
     * `set gamma` で実機を追い込んだ瞬間に `shot --verify` が
     * 「実機が壊れた」ように落ちる。落ちる理由はガンマ違いなので紛らわしい。 */
    uint8_t  gamma_x10;
    /* 文字の拡大率（1 か 2）。ガンマと同じ理由でここに載せる:
     * 基準レンダラが同じ倍率で描かないと `shot --verify` が落ち、
     * しかも「実機が壊れた」ように見える。 */
    uint8_t  scale;
    /* ステータス帯の高さ（画素）。0 なら帯なし。
     * **`shot --verify` の比較を本文だけに限るために要る。**
     * 帯の中身（IP や電池）はホスト側で再現できないので、
     * そこを比べると必ず食い違う。 */
    uint8_t  status_h;
    uint32_t byte_len;
} cdk_wire_shot_info_t;

#define CDK_PIXFMT_RGB565_LE 0u
#define CDK_PIXFMT_RGB565_BE 1u

/* KEY の payload。すべて little endian。 */
typedef struct {
    uint16_t keycode;   /* HID usage 相当。0 なら codepoint のみ */
    uint8_t  mods;      /* CDK_MOD_* */
    uint8_t  reserved;
    uint32_t codepoint; /* 文字が確定するなら Unicode、無ければ 0 */
} cdk_wire_key_t;

#define CDK_MOD_CTRL  0x01u
#define CDK_MOD_SHIFT 0x02u
#define CDK_MOD_ALT   0x04u
#define CDK_MOD_GUI   0x08u

/* ---- CRC32 (IEEE, reflected, init 0xFFFFFFFF, final xor 0xFFFFFFFF) ---- */
/* 継続して呼ぶ場合は前回の戻り値をそのまま crc に渡す。初回は 0 を渡す。 */
uint32_t cdk_crc32(uint32_t crc, const void *data, size_t len);

/* ---- 送信 ---- */

/* out に 1 フレームを書き込み、書き込んだバイト数を返す。
 * out_cap 不足または len > CDK_PROTO_MAX_PAYLOAD なら 0 を返す。 */
size_t cdk_proto_encode(uint8_t type, const void *payload, uint32_t len,
                        void *out, size_t out_cap);

/* type と len から必要な出力サイズを返す */
static inline size_t cdk_proto_encoded_size(uint32_t payload_len)
{
    return (size_t)payload_len + CDK_PROTO_OVERHEAD;
}

/* ---- 受信 ---- */

typedef void (*cdk_proto_frame_fn)(void *user, uint8_t type,
                                   const uint8_t *payload, uint32_t len);

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   used;
    cdk_proto_frame_fn on_frame;
    void    *user;

    /* 診断用カウンタ。同期外れが起きていないかを監視する。 */
    uint32_t stat_frames;   /* 正常に受理したフレーム数 */
    uint32_t stat_bad_crc;  /* magic は合ったが CRC が合わなかった回数 */
    uint32_t stat_dropped;  /* 再同期のために捨てたバイト数 */
} cdk_proto_rx_t;

/* buf は呼び出し側が保持する。cap は CDK_PROTO_RX_BUF_MIN 以上。 */
void cdk_proto_rx_init(cdk_proto_rx_t *rx, uint8_t *buf, size_t cap,
                       cdk_proto_frame_fn on_frame, void *user);

/* 受信バイト列を投入する。完全なフレームが揃うたびに on_frame が呼ばれる。 */
void cdk_proto_rx_feed(cdk_proto_rx_t *rx, const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* CDK_PROTO_H */
