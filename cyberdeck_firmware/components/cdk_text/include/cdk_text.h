/* cdk_text — UTF-8 デコードと東アジア文字幅
 *
 * ESP-IDF に依存しない純 C。
 *
 * 端末として一番大事なのは「桁がずれないこと」。表示できるグリフがあるかと、
 * その文字が何セル占めるかは完全に別問題として扱う。グリフが無い文字でも
 * 幅だけは Unicode East Asian Width に従って数える。
 */
#ifndef CDK_TEXT_H
#define CDK_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 不正なバイト列に当たったときに使う置換文字 */
#define CDK_REPLACEMENT 0xFFFDu

/* 表示できないグリフの代わりに出す文字（げた）。幅は 2。 */
#define CDK_TOFU 0x3013u

typedef enum {
    CDK_UTF8_OK,       /* 1 文字取れた */
    CDK_UTF8_NEED,     /* 途中。もっとバイトが要る */
    CDK_UTF8_INVALID   /* 不正。1 バイト読み飛ばして U+FFFD にする */
} cdk_utf8_status_t;

/* ストリーム用の UTF-8 デコーダ。バイトを 1 つずつ食わせる。
 * 分割された入力（シリアルや SSH）で途中まで来た状態を保持できる。 */
typedef struct {
    uint32_t acc;   /* 組み立て中のコードポイント */
    uint8_t  need;  /* あと何バイト要るか */
    uint8_t  seen;  /* 何バイト読んだか */
    uint32_t min;   /* 最短形式チェック用の下限 */
} cdk_utf8_t;

void cdk_utf8_reset(cdk_utf8_t *d);

/* 1 バイト食わせる。CDK_UTF8_OK のとき *cp に結果が入る。
 * CDK_UTF8_INVALID のときも *cp に U+FFFD が入る（呼び側はそのまま描いてよい）。
 * 不正バイトが新しい文字の先頭でもある場合は *retry に true が入るので、
 * 同じバイトをもう一度食わせること。 */
cdk_utf8_status_t cdk_utf8_feed(cdk_utf8_t *d, uint8_t byte, uint32_t *cp, bool *retry);

/* バッファ全体を一度にデコードする版。消費バイト数を返す。
 * 末尾が途中で切れている場合はそのぶんを消費せずに返す。 */
size_t cdk_utf8_decode(const uint8_t *p, size_t len, uint32_t *cp, bool *ok);

/* コードポイントを UTF-8 に符号化する。書いたバイト数 (1-4) を返す。
 * out は 4 バイト以上。 */
size_t cdk_utf8_encode(uint32_t cp, uint8_t *out);

/* 端末セルでの幅。
 *   0 = 結合文字・ゼロ幅（直前のセルに付く。M1 では捨てる）
 *   1 = 半角
 *   2 = 全角
 * 制御文字は 0 を返す。端末側で別途処理すること。 */
int cdk_char_width(uint32_t cp);

/* East Asian Width が Ambiguous かどうか。
 * 端末では通常「半角」として扱うが、日本語環境では全角にしたいことがある。
 * cdk_char_width は常に半角として返すので、必要ならこれで上書きする。 */
bool cdk_char_ambiguous(uint32_t cp);

#ifdef __cplusplus
}
#endif

#endif /* CDK_TEXT_H */
