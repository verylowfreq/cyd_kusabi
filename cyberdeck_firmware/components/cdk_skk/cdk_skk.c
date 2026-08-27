/* SKK の状態機械。仕様は cdk_skk.h の頭。 */
#include "cdk_skk.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* 小道具                                                              */
/* ------------------------------------------------------------------ */

static void str_append(char *dst, size_t cap, const char *src, size_t len)
{
    const size_t used = strlen(dst);
    if (used + len >= cap) {
        return; /* 入らないものは捨てる。切れた UTF-8 を作らない */
    }
    memcpy(dst + used, src, len);
    dst[used + len] = '\0';
}

/* UTF-8 の末尾 1 文字を消す。 */
static void str_chop(char *s)
{
    size_t n = strlen(s);
    if (n == 0) {
        return;
    }
    n--;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) {
        n--;
    }
    s[n] = '\0';
}

static void emit(cdk_skk_t *s, const char *text, size_t len)
{
    if (len > 0 && s->commit != NULL) {
        s->commit(s->ctx, text, len);
    }
}

/* かなをいまのモードの文字種に直して確定する。 */
static void emit_kana(cdk_skk_t *s, const char *kana, size_t len)
{
    if (len == 0) {
        return;
    }
    char buf[CDK_SKK_PREEDIT_MAX];
    switch (s->mode) {
    case CDK_SKK_KATAKANA:
        len = cdk_skk_hira_to_kata(kana, len, buf, sizeof(buf));
        emit(s, buf, len);
        return;
    case CDK_SKK_HANKAKU:
        len = cdk_skk_hira_to_hankaku(kana, len, buf, sizeof(buf));
        emit(s, buf, len);
        return;
    default:
        emit(s, kana, len);
        return;
    }
}

/* キーを ASCII 1 文字に落とす。修飾つきや特殊キーは -1。 */
static int key_char(const cdk_key_t *k)
{
    if (k->keycode == CDK_K_SPACE && k->mods == 0) {
        return ' ';
    }
    if (k->keycode != 0) {
        return -1;
    }
    if ((k->mods & (CDK_MOD_CTRL_BIT | CDK_MOD_ALT_BIT | CDK_MOD_GUI_BIT)) != 0) {
        return -1;
    }
    if (k->codepoint >= 0x21 && k->codepoint < 0x7F) {
        return (int)k->codepoint;
    }
    return -1;
}

static bool is_ctrl(const cdk_key_t *k, char c)
{
    return k->keycode == 0 && (k->mods & CDK_MOD_CTRL_BIT) != 0 &&
           k->codepoint == (uint32_t)c;
}

/* ------------------------------------------------------------------ */
/* 表示                                                                */
/* ------------------------------------------------------------------ */

static void rebuild_preedit(cdk_skk_t *s)
{
    s->preedit[0] = '\0';
    const char *pend = cdk_skk_romkana_pending(&s->rk);

    switch (s->state) {
    case CDK_SKK_ST_DIRECT:
        /* 直接入力では未確定のローマ字だけが見える。
         * これを出さないと「k を押したのに何も起きない」ように見える。 */
        str_append(s->preedit, sizeof(s->preedit), pend, strlen(pend));
        break;

    case CDK_SKK_ST_MIDASHI:
        str_append(s->preedit, sizeof(s->preedit), "▽", strlen("▽"));
        str_append(s->preedit, sizeof(s->preedit), s->midashi, strlen(s->midashi));
        str_append(s->preedit, sizeof(s->preedit), pend, strlen(pend));
        break;

    case CDK_SKK_ST_OKURI:
        str_append(s->preedit, sizeof(s->preedit), "▽", strlen("▽"));
        str_append(s->preedit, sizeof(s->preedit), s->midashi, strlen(s->midashi));
        str_append(s->preedit, sizeof(s->preedit), "*", 1);
        str_append(s->preedit, sizeof(s->preedit), s->okuri, strlen(s->okuri));
        str_append(s->preedit, sizeof(s->preedit), pend, strlen(pend));
        break;

    case CDK_SKK_ST_SELECT: {
        char cand[CDK_SKK_CAND_MAX];
        if (!cdk_skk_cand_at(s->cands, s->cand_index, cand, sizeof(cand), NULL, 0)) {
            cand[0] = '\0';
        }
        str_append(s->preedit, sizeof(s->preedit), "▼", strlen("▼"));
        str_append(s->preedit, sizeof(s->preedit), cand, strlen(cand));
        str_append(s->preedit, sizeof(s->preedit), s->okuri, strlen(s->okuri));
        break;
    }
    }
}

const char *cdk_skk_preedit(const cdk_skk_t *s)
{
    return s->preedit;
}

const char *cdk_skk_mode_label(const cdk_skk_t *s)
{
    switch (s->mode) {
    case CDK_SKK_HIRAGANA: return "あ";
    case CDK_SKK_KATAKANA: return "ア";
    case CDK_SKK_HANKAKU:  return "ｱ";
    case CDK_SKK_ZENEI:    return "Ａ";
    default:               return "";
    }
}

/* ------------------------------------------------------------------ */
/* 生成・初期化                                                        */
/* ------------------------------------------------------------------ */

void cdk_skk_init(cdk_skk_t *s, cdk_skk_commit_fn commit, void *ctx)
{
    memset(s, 0, sizeof(*s));
    s->commit = commit;
    s->ctx = ctx;
    s->mode = CDK_SKK_ASCII;
    s->state = CDK_SKK_ST_DIRECT;
    cdk_skk_romkana_reset(&s->rk);
}

void cdk_skk_set_dict(cdk_skk_t *s, const cdk_skk_dict_t *dict)
{
    s->dict = dict;
}

void cdk_skk_set_user(cdk_skk_t *s, cdk_skk_user_t *user)
{
    s->user = user;
}

void cdk_skk_set_mode(cdk_skk_t *s, cdk_skk_mode_t m)
{
    cdk_skk_reset(s);
    s->mode = m;
}

cdk_skk_mode_t cdk_skk_get_mode(const cdk_skk_t *s)
{
    return s->mode;
}

void cdk_skk_reset(cdk_skk_t *s)
{
    s->state = CDK_SKK_ST_DIRECT;
    s->midashi[0] = '\0';
    s->okuri[0] = '\0';
    s->okuri_head = '\0';
    s->abbrev = false;
    s->cands[0] = '\0';
    s->cand_index = 0;
    s->cand_count = 0;
    s->preedit[0] = '\0';
    s->lookup_key[0] = '\0';
    s->lookup_okuri = false;
    cdk_skk_romkana_reset(&s->rk);
}

/* ------------------------------------------------------------------ */
/* 変換                                                                */
/* ------------------------------------------------------------------ */

/* ▽ の中身を辞書で引いて ▼ に入る。引けなければ false。 */
static bool start_select(cdk_skk_t *s)
{
    s->last_lookup_failed = false;
    if (s->midashi[0] == '\0') {
        s->last_lookup_failed = true;
        return false;
    }

    char key[CDK_SKK_KEY_MAX];
    key[0] = '\0';
    str_append(key, sizeof(key), s->midashi, strlen(s->midashi));
    if (s->okuri_head != '\0') {
        /* 送りありの見出しは「かな + 送りの頭の子音」。
         * ここで子音を足し忘れると送りなしの索引に無い語を探すことになり、
         * 「変換できない」ようにしか見えない。 */
        str_append(key, sizeof(key), &s->okuri_head, 1);
    }
    /* 学習は確定のときに行うので、引いた見出しを覚えておく。 */
    snprintf(s->lookup_key, sizeof(s->lookup_key), "%s", key);
    s->lookup_okuri = (s->okuri_head != '\0');

    const char *user_cands =
        (s->user != NULL) ? cdk_skk_user_lookup(s->user, key, s->lookup_okuri) : NULL;

    static char sys_cands[CDK_SKK_CAND_MAX];
    sys_cands[0] = '\0';
    const bool got_sys =
        (s->dict != NULL) &&
        cdk_skk_dict_lookup(s->dict, key, s->lookup_okuri, sys_cands, sizeof(sys_cands));

    if (user_cands == NULL && !got_sys) {
        s->last_lookup_failed = true;
        return false;
    }
    /* 個人辞書が先。同じ候補は本辞書側から落とす。 */
    cdk_skk_merge_cands(user_cands, got_sys ? sys_cands : NULL, s->cands,
                        sizeof(s->cands));
    s->cand_count = cdk_skk_cand_count(s->cands);
    if (s->cand_count == 0) {
        s->last_lookup_failed = true;
        return false;
    }
    s->cand_index = 0;
    s->state = CDK_SKK_ST_SELECT;
    return true;
}

/* いま選んでいる候補（+ 送り仮名）を確定する。 */
static void commit_candidate(cdk_skk_t *s)
{
    char cand[CDK_SKK_CAND_MAX];
    if (cdk_skk_cand_at(s->cands, s->cand_index, cand, sizeof(cand), NULL, 0)) {
        emit(s, cand, strlen(cand));
        /* 選んだものを次から先頭に出す。ここでしか学習しない。 */
        if (s->user != NULL && s->lookup_key[0] != '\0') {
            cdk_skk_user_learn(s->user, s->lookup_key, s->lookup_okuri, cand);
        }
    }
    if (s->okuri[0] != '\0') {
        emit_kana(s, s->okuri, strlen(s->okuri));
    }
    cdk_skk_reset(s);
}

/* ▽ の中身をそのまま（かなのまま）確定する。 */
static void commit_midashi(cdk_skk_t *s)
{
    char out[32];
    const size_t n = cdk_skk_romkana_flush(&s->rk, out, sizeof(out));
    if (n > 0) {
        str_append(s->midashi, sizeof(s->midashi), out, n);
    }
    if (s->abbrev) {
        emit(s, s->midashi, strlen(s->midashi));
    } else {
        emit_kana(s, s->midashi, strlen(s->midashi));
    }
    if (s->okuri[0] != '\0') {
        emit_kana(s, s->okuri, strlen(s->okuri));
    }
    cdk_skk_reset(s);
}

/* ------------------------------------------------------------------ */
/* 各状態のキー処理                                                    */
/* ------------------------------------------------------------------ */

/* 未確定のローマ字を 1 文字消す。消すものが無ければ false。
 *
 * ローマ字の途中は「かな」ではないので、消したあとの状態は
 * 残りを頭から入れ直して作る。"kya" の a を消したら "ky" に戻す、
 * という戻し方を状態機械の外に書かなくて済む。 */
static bool backspace_pending(cdk_skk_t *s)
{
    if (cdk_skk_romkana_pending(&s->rk)[0] == '\0') {
        return false;
    }
    char pend[CDK_SKK_ROM_MAX + 1];
    strcpy(pend, cdk_skk_romkana_pending(&s->rk));
    pend[strlen(pend) - 1] = '\0';

    cdk_skk_romkana_reset(&s->rk);
    for (const char *p = pend; *p != '\0'; p++) {
        char tmp[32];
        bool consumed = false;
        (void)cdk_skk_romkana_feed(&s->rk, *p, tmp, sizeof(tmp), &consumed);
    }
    return true;
}

static bool key_direct(cdk_skk_t *s, const cdk_key_t *k)
{
    /* Ctrl-J は未確定のローマ字を吐き出す。全英からは かな に戻る。 */
    if (is_ctrl(k, 'j')) {
        if (s->mode == CDK_SKK_ZENEI) {
            s->mode = CDK_SKK_HIRAGANA;
            return true;
        }
        char out[32];
        const size_t n = cdk_skk_romkana_flush(&s->rk, out, sizeof(out));
        emit_kana(s, out, n);
        return true;
    }

    if (k->keycode == CDK_K_BS) {
        /* 未確定のローマ字があるときだけ食べる。無ければ端末に流す。 */
        if (backspace_pending(s)) {
            return true;
        }
        return false;
    }

    const int c = key_char(k);
    if (c < 0) {
        return false; /* 特殊キーは端末へ */
    }

    if (s->mode == CDK_SKK_ZENEI) {
        char in = (char)c;
        char out[8];
        const size_t n = cdk_skk_ascii_to_zenkaku(&in, 1, out, sizeof(out));
        emit(s, out, n);
        return true;
    }

    /* モード切り替え。未確定のローマ字が無いときだけ効く。
     * "l" は "らりるれろ" の途中にも出るので、pending があるなら文字扱い。 */
    if (cdk_skk_romkana_pending(&s->rk)[0] == '\0') {
        if (c == 'q') {
            s->mode = (s->mode == CDK_SKK_KATAKANA) ? CDK_SKK_HIRAGANA
                                                    : CDK_SKK_KATAKANA;
            return true;
        }
        if (c == 'l') {
            s->mode = CDK_SKK_ASCII;
            return true;
        }
        if (c == 'L') {
            s->mode = CDK_SKK_ZENEI;
            return true;
        }
        if (c == '/') {
            s->state = CDK_SKK_ST_MIDASHI;
            s->abbrev = true;
            return true;
        }
    }

    if (c >= 'A' && c <= 'Z') {
        /* 大文字で ▽ に入る。その文字自身も見出しの一部。 */
        s->state = CDK_SKK_ST_MIDASHI;
        s->abbrev = false;
        char out[32];
        bool consumed = false;
        const size_t n =
            cdk_skk_romkana_feed(&s->rk, (char)c, out, sizeof(out), &consumed);
        str_append(s->midashi, sizeof(s->midashi), out, n);
        return true;
    }

    char out[32];
    bool consumed = false;
    const size_t n = cdk_skk_romkana_feed(&s->rk, (char)c, out, sizeof(out), &consumed);
    if (!consumed) {
        return false;
    }
    emit_kana(s, out, n);
    return true;
}

static bool key_midashi(cdk_skk_t *s, const cdk_key_t *k)
{
    if (k->keycode == CDK_K_ESC) {
        cdk_skk_reset(s); /* 捨てる */
        return true;
    }
    if (k->keycode == CDK_K_ENTER || is_ctrl(k, 'j')) {
        commit_midashi(s);
        return true;
    }
    if (k->keycode == CDK_K_BS) {
        if (backspace_pending(s)) {
            return true;
        }
        if (s->midashi[0] != '\0') {
            str_chop(s->midashi);
            return true;
        }
        /* ▽ だけ残っている状態。抜ける。 */
        cdk_skk_reset(s);
        return true;
    }

    const int c = key_char(k);
    if (c < 0) {
        return true; /* ▽ の間は他のキーを外へ漏らさない */
    }

    if (c == ' ') {
        char out[32];
        const size_t n = cdk_skk_romkana_flush(&s->rk, out, sizeof(out));
        str_append(s->midashi, sizeof(s->midashi), out, n);
        if (!start_select(s)) {
            /* 辞書に無い。いまは かな のまま確定する。
             * 本来ここは辞書登録モードに入るところ（DESIGN.md §9.2）。 */
            commit_midashi(s);
        }
        return true;
    }

    if (s->abbrev) {
        /* abbrev はローマ字変換しない。英字のまま見出しにする。 */
        const char ch = (char)c;
        str_append(s->midashi, sizeof(s->midashi), &ch, 1);
        return true;
    }

    if (c == 'q') {
        /* ▽ の中身をカタカナにして確定する。 */
        char out[32];
        const size_t n = cdk_skk_romkana_flush(&s->rk, out, sizeof(out));
        str_append(s->midashi, sizeof(s->midashi), out, n);
        char kata[CDK_SKK_MIDASHI_MAX * 2];
        const size_t m =
            cdk_skk_hira_to_kata(s->midashi, strlen(s->midashi), kata, sizeof(kata));
        emit(s, kata, m);
        cdk_skk_reset(s);
        return true;
    }

    if (c >= 'A' && c <= 'Z') {
        /* 送り仮名の始まり。大文字の子音が送りの頭になる。 */
        const char lower = (char)(c - 'A' + 'a');
        char       out[32];
        const size_t n = cdk_skk_romkana_flush(&s->rk, out, sizeof(out));
        str_append(s->midashi, sizeof(s->midashi), out, n);
        if (s->midashi[0] == '\0') {
            /* ▽ の直後にいきなり大文字。送りではなく見出しの続きとみなす。 */
            bool consumed = false;
            const size_t m =
                cdk_skk_romkana_feed(&s->rk, lower, out, sizeof(out), &consumed);
            str_append(s->midashi, sizeof(s->midashi), out, m);
            return true;
        }
        s->okuri_head = lower;
        s->state = CDK_SKK_ST_OKURI;
        bool consumed = false;
        const size_t m =
            cdk_skk_romkana_feed(&s->rk, lower, out, sizeof(out), &consumed);
        if (m > 0) {
            /* 母音を伴わずに かな が出た（"nn" など）。そのまま送りとする。 */
            str_append(s->okuri, sizeof(s->okuri), out, m);
            (void)start_select(s);
            if (s->state != CDK_SKK_ST_SELECT) {
                commit_midashi(s);
            }
        }
        return true;
    }

    char out[32];
    bool consumed = false;
    const size_t n = cdk_skk_romkana_feed(&s->rk, (char)c, out, sizeof(out), &consumed);
    str_append(s->midashi, sizeof(s->midashi), out, n);
    return true;
}

static bool key_okuri(cdk_skk_t *s, const cdk_key_t *k)
{
    if (k->keycode == CDK_K_ESC) {
        /* 送りだけ捨てて ▽ に戻る。 */
        s->okuri[0] = '\0';
        s->okuri_head = '\0';
        s->state = CDK_SKK_ST_MIDASHI;
        cdk_skk_romkana_reset(&s->rk);
        return true;
    }
    if (k->keycode == CDK_K_ENTER || is_ctrl(k, 'j')) {
        commit_midashi(s);
        return true;
    }
    if (k->keycode == CDK_K_BS) {
        /* ローマ字がまだ残るなら 1 文字消すだけ。 */
        if (backspace_pending(s) && cdk_skk_romkana_pending(&s->rk)[0] != '\0') {
            return true;
        }
        /* 送りの頭まで消えた。▽ に戻す。
         * ここで OKURI のまま残すと "▽おく*" という、
         * 母音を打つまで抜けられない見た目になる。 */
        s->okuri[0] = '\0';
        s->okuri_head = '\0';
        s->state = CDK_SKK_ST_MIDASHI;
        cdk_skk_romkana_reset(&s->rk);
        return true;
    }

    const int c = key_char(k);
    if (c < 0) {
        return true;
    }

    char       out[32];
    bool       consumed = false;
    const size_t n = cdk_skk_romkana_feed(&s->rk, (char)c, out, sizeof(out), &consumed);
    if (n == 0) {
        return true; /* まだ かな にならない */
    }
    str_append(s->okuri, sizeof(s->okuri), out, n);

    /* 送り仮名が 1 文字出たら引く。SKK はここで自動的に ▼ に入る。 */
    if (!start_select(s)) {
        commit_midashi(s);
    }
    return true;
}

static bool key_select(cdk_skk_t *s, const cdk_key_t *k)
{
    if (k->keycode == CDK_K_ESC) {
        /* ▽ に戻す。候補を捨てるだけ。 */
        s->state = CDK_SKK_ST_MIDASHI;
        s->cands[0] = '\0';
        s->cand_count = 0;
        s->cand_index = 0;
        return true;
    }
    if (k->keycode == CDK_K_ENTER || is_ctrl(k, 'j')) {
        commit_candidate(s);
        return true;
    }
    if (k->keycode == CDK_K_BS) {
        s->state = CDK_SKK_ST_MIDASHI;
        s->cands[0] = '\0';
        s->cand_count = 0;
        s->cand_index = 0;
        return true;
    }

    const int c = key_char(k);
    if (c < 0) {
        return true;
    }

    if (c == ' ') {
        if (s->cand_index + 1 < s->cand_count) {
            s->cand_index++;
        }
        /* 最後の候補で Space を押しても止まる。本来は辞書登録に入る。 */
        return true;
    }
    if (c == 'x') {
        if (s->cand_index > 0) {
            s->cand_index--;
        } else {
            s->state = CDK_SKK_ST_MIDASHI;
            s->cands[0] = '\0';
            s->cand_count = 0;
        }
        return true;
    }

    /* それ以外の文字は「確定してから、その文字を打ち直す」。
     * SKK の既定の振る舞い。句読点で確定できるので打鍵が減る。 */
    commit_candidate(s);
    return key_direct(s, k);
}

/* ------------------------------------------------------------------ */

bool cdk_skk_key(cdk_skk_t *s, const cdk_key_t *k)
{
    bool used;

    if (s->mode == CDK_SKK_ASCII) {
        /* SKK は止まっている。Ctrl-J だけ見て、かな入力を始める。 */
        if (is_ctrl(k, 'j')) {
            s->mode = CDK_SKK_HIRAGANA;
            rebuild_preedit(s);
            return true;
        }
        return false;
    }

    switch (s->state) {
    case CDK_SKK_ST_MIDASHI: used = key_midashi(s, k); break;
    case CDK_SKK_ST_OKURI:   used = key_okuri(s, k);   break;
    case CDK_SKK_ST_SELECT:  used = key_select(s, k);  break;
    default:                 used = key_direct(s, k);  break;
    }

    rebuild_preedit(s);
    return used;
}
