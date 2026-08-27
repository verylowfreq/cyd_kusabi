/* cdk_keys_decode — 端末の入力バイト列を cdk_key_t に戻す
 *
 * cdk_keys_encode の逆。ホスト側テストで往復（encode → decode = 元の値）を
 * 検証できるので、対応表がずれたら必ず落ちる。
 *
 * これがあるおかげでシェルはエスケープ列を一切見ない。
 * 矢印キーが ESC [ A で来ることも、その 3 バイトが feed の呼び出しを
 * またいで分割されることも、ここだけが知っていればよい。
 *
 * ESC 単独か、エスケープ列の始まりかは、次のバイトが来るまで確定しない。
 * 入力が途切れたところで cdk_keys_decode_flush() を呼ぶと保留を吐き出す。
 */
#include "cdk_keys.h"

#include "cdk_text.h"

#include <string.h>

#define MAX_PARAMS 4

void cdk_keys_decode_init(cdk_keys_decoder_t *d, cdk_key_fn on_key, void *user)
{
    memset(d, 0, sizeof(*d));
    d->on_key = on_key;
    d->user = user;
    cdk_utf8_reset(&d->utf8);
}

static void emit(cdk_keys_decoder_t *d, uint16_t keycode, uint8_t mods, uint32_t cp)
{
    /* ESC 前置で保留していた Alt をここで合成する。
     * UTF-8 が複数バイトに割れていても、文字が確定した時点で付く。 */
    mods |= d->pending_mods;
    d->pending_mods = 0;

    if (d->on_key != NULL) {
        const cdk_key_t k = {keycode, mods, cp};
        d->on_key(d->user, &k);
    }
}

/* xterm の修飾子コード (1 + shift + alt*2 + ctrl*4) をビットに戻す */
static uint8_t mods_from_code(int code)
{
    if (code <= 1) {
        return 0;
    }
    const int m = code - 1;
    uint8_t mods = 0;
    if (m & 1) {
        mods |= CDK_MOD_SHIFT_BIT;
    }
    if (m & 2) {
        mods |= CDK_MOD_ALT_BIT;
    }
    if (m & 4) {
        mods |= CDK_MOD_CTRL_BIT;
    }
    return mods;
}

/* CSI / SS3 の終端文字からキーを決める */
static void dispatch_final(cdk_keys_decoder_t *d, uint8_t final, bool ss3)
{
    const uint8_t mods = mods_from_code(d->nparams >= 2 ? d->params[1] : 0);
    const int first = (d->nparams >= 1) ? d->params[0] : 0;

    switch (final) {
    case 'A': emit(d, CDK_K_UP, mods, 0); return;
    case 'B': emit(d, CDK_K_DOWN, mods, 0); return;
    case 'C': emit(d, CDK_K_RIGHT, mods, 0); return;
    case 'D': emit(d, CDK_K_LEFT, mods, 0); return;
    case 'H': emit(d, CDK_K_HOME, mods, 0); return;
    case 'F': emit(d, CDK_K_END, mods, 0); return;
    case 'Z': emit(d, CDK_K_TAB, CDK_MOD_SHIFT_BIT, 0); return;
    default: break;
    }

    if (ss3 && final >= 'P' && final <= 'S') {
        emit(d, (uint16_t)(CDK_K_F1 + (final - 'P')), mods, 0);
        return;
    }

    if (final == '~') {
        switch (first) {
        case 2:  emit(d, CDK_K_INSERT, mods, 0); return;
        case 3:  emit(d, CDK_K_DELETE, mods, 0); return;
        case 5:  emit(d, CDK_K_PGUP, mods, 0); return;
        case 6:  emit(d, CDK_K_PGDN, mods, 0); return;
        /* F1-F12 の番号は歴史的に飛んでいる。encode 側と同じ表。 */
        case 11: emit(d, CDK_K_F1 + 0, mods, 0); return;
        case 12: emit(d, CDK_K_F1 + 1, mods, 0); return;
        case 13: emit(d, CDK_K_F1 + 2, mods, 0); return;
        case 14: emit(d, CDK_K_F1 + 3, mods, 0); return;
        case 15: emit(d, CDK_K_F1 + 4, mods, 0); return;
        case 17: emit(d, CDK_K_F1 + 5, mods, 0); return;
        case 18: emit(d, CDK_K_F1 + 6, mods, 0); return;
        case 19: emit(d, CDK_K_F1 + 7, mods, 0); return;
        case 20: emit(d, CDK_K_F1 + 8, mods, 0); return;
        case 21: emit(d, CDK_K_F1 + 9, mods, 0); return;
        case 23: emit(d, CDK_K_F1 + 10, mods, 0); return;
        case 24: emit(d, CDK_K_F1 + 11, mods, 0); return;
        default: break;
        }
    }
    d->stat_unknown++;
}

/* Ctrl 付き制御コードを文字に戻す。cdk_keys_encode の ctrl_of() の逆。 */
static bool ctrl_to_key(cdk_keys_decoder_t *d, uint8_t b)
{
    const uint8_t mods = CDK_MOD_CTRL_BIT;

    if (b >= 1 && b <= 26) {
        emit(d, CDK_K_NONE, mods, (uint32_t)('a' + b - 1));
        return true;
    }
    switch (b) {
    case 0x00: emit(d, CDK_K_SPACE, mods, 0); return true;
    case 0x1C: emit(d, CDK_K_NONE, mods, '\\'); return true;
    case 0x1D: emit(d, CDK_K_NONE, mods, ']'); return true;
    case 0x1E: emit(d, CDK_K_NONE, mods, '^'); return true;
    case 0x1F: emit(d, CDK_K_NONE, mods, '_'); return true;
    default: return false;
    }
}

/* 通常文字（制御文字でないもの）を処理する。UTF-8 を跨いで組み立てる。 */
static void ground_byte(cdk_keys_decoder_t *d, uint8_t b)
{
    switch (b) {
    case '\r':
    case '\n':
        emit(d, CDK_K_ENTER, 0, 0);
        return;
    case '\t':
        emit(d, CDK_K_TAB, 0, 0);
        return;
    case 0x7F:
        emit(d, CDK_K_BS, 0, 0);
        return;
    case ' ':
        emit(d, CDK_K_SPACE, 0, 0);
        return;
    default:
        break;
    }

    if (b < 0x20) {
        if (!ctrl_to_key(d, b)) {
            d->stat_unknown++;
        }
        return;
    }

    uint32_t cp = 0;
    bool retry = false;
    const cdk_utf8_status_t st = cdk_utf8_feed(&d->utf8, b, &cp, &retry);
    if (st == CDK_UTF8_OK) {
        emit(d, CDK_K_NONE, 0, cp);
    } else if (st == CDK_UTF8_INVALID) {
        emit(d, CDK_K_NONE, 0, CDK_REPLACEMENT);
        if (retry) {
            bool r2 = false;
            if (cdk_utf8_feed(&d->utf8, b, &cp, &r2) == CDK_UTF8_OK) {
                emit(d, CDK_K_NONE, 0, cp);
            }
        }
    }
    /* CDK_UTF8_NEED は続きを待つ */
}

void cdk_keys_decode_feed(cdk_keys_decoder_t *d, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    for (size_t i = 0; i < len; i++) {
        const uint8_t b = p[i];

        switch (d->state) {
        case CDK_KD_GROUND:
            if (b == 0x1B) {
                d->state = CDK_KD_ESC;
            } else {
                ground_byte(d, b);
            }
            break;

        case CDK_KD_ESC:
            if (b == '[') {
                d->state = CDK_KD_CSI;
                d->nparams = 0;
                d->params[0] = 0;
                d->param_seen = false;
            } else if (b == 'O') {
                d->state = CDK_KD_SS3;
                d->nparams = 0;
                d->params[0] = 0;
                d->param_seen = false;
            } else if (b == 0x1B) {
                /* ESC ESC。1 つ目を Esc キーとして確定させ、2 つ目を保留する。 */
                emit(d, CDK_K_ESC, 0, 0);
            } else {
                /* Alt + 何か。encode 側の metaSendsEscape の逆。
                 * 文字が UTF-8 で複数バイトにまたがることがあるので、
                 * ここでは付けずに保留し、キーが確定した時点で合成する。 */
                d->state = CDK_KD_GROUND;
                d->pending_mods = CDK_MOD_ALT_BIT;
                ground_byte(d, b);
            }
            break;

        case CDK_KD_CSI:
        case CDK_KD_SS3:
            if (b >= '0' && b <= '9') {
                if (d->nparams < MAX_PARAMS) {
                    d->params[d->nparams] = d->params[d->nparams] * 10 + (b - '0');
                    d->param_seen = true;
                }
            } else if (b == ';') {
                if (d->nparams + 1 < MAX_PARAMS) {
                    d->nparams++;
                    d->params[d->nparams] = 0;
                }
            } else if (b >= 0x40 && b <= 0x7E) {
                if (d->param_seen) {
                    d->nparams++;
                }
                dispatch_final(d, b, d->state == CDK_KD_SS3);
                d->state = CDK_KD_GROUND;
                d->nparams = 0;
                d->param_seen = false;
                memset(d->params, 0, sizeof(d->params));
            } else if (b == 0x1B) {
                /* 途中で新しいシーケンスが始まった。今までを捨てる。 */
                d->stat_unknown++;
                d->state = CDK_KD_ESC;
            }
            /* それ以外（中間文字など）は読み飛ばす */
            break;

        default:
            d->state = CDK_KD_GROUND;
            break;
        }
    }
}

void cdk_keys_decode_flush(cdk_keys_decoder_t *d)
{
    /* 入力が途切れた時点で ESC 単独だったなら、それは Esc キー。 */
    if (d->state == CDK_KD_ESC) {
        emit(d, CDK_K_ESC, 0, 0);
    }
    d->pending_mods = 0;
    d->state = CDK_KD_GROUND;
    d->nparams = 0;
    d->param_seen = false;
    memset(d->params, 0, sizeof(d->params));
    cdk_utf8_reset(&d->utf8);
}
