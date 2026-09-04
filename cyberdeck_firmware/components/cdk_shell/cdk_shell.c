#include "cdk_shell.h"

#include "cdk_text.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* 出力                                                                */
/* ------------------------------------------------------------------ */

static void emit(cdk_shell_t *sh, const char *s, size_t n)
{
    if (sh->out != NULL && n > 0) {
        sh->out(sh->out_user, s, n);
    }
}

void cdk_shell_write(cdk_shell_t *sh, const char *s)
{
    if (s != NULL) {
        emit(sh, s, strlen(s));
    }
}

void cdk_shell_printf(cdk_shell_t *sh, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if (n >= (int)sizeof(buf)) {
        n = (int)sizeof(buf) - 1;
    }
    emit(sh, buf, (size_t)n);
}

/* 画面上で何桁ぶんになるかを数える。
 *
 * エスケープ列は幅を持たない。プロンプトに色を付けると SGR が入るので、
 * ここを素通しにするとカーソルがその文字数だけ右にずれる。
 * 全角は 2 桁として数える。 */
int cdk_shell_visible_width(const char *s, size_t len)
{
    int width = 0;
    size_t i = 0;

    while (i < len && s[i] != '\0') {
        if ((unsigned char)s[i] == 0x1B) {
            i++;
            if (i < len && (s[i] == '[' || s[i] == ']')) {
                const char kind = s[i];
                i++;
                if (kind == '[') {
                    /* CSI: 終端は 0x40-0x7E */
                    while (i < len && ((unsigned char)s[i] < 0x40 ||
                                       (unsigned char)s[i] > 0x7E)) {
                        i++;
                    }
                } else {
                    /* OSC: BEL か ESC で終わる */
                    while (i < len && (unsigned char)s[i] != 0x07 &&
                           (unsigned char)s[i] != 0x1B) {
                        i++;
                    }
                }
            }
            if (i < len) {
                i++; /* 終端文字を飛ばす */
            }
            continue;
        }

        uint32_t cp = 0;
        bool ok = false;
        const size_t used = cdk_utf8_decode((const uint8_t *)s + i, len - i, &cp, &ok);
        if (used == 0) {
            break; /* 途中で切れている */
        }
        width += ok ? cdk_char_width(cp) : 1;
        i += used;
    }
    return width;
}

/* 行を丸ごと描き直す。
 *
 * 差分更新はしない。端末の桁数を知らないので、折り返した行のどこに
 * カーソルがあるかを正しく追えないため。CR で行頭に戻して消して書き直す方が、
 * 遅くても常に正しい。1 行 256 文字なので実用上も問題にならない。 */
static void redraw(cdk_shell_t *sh)
{
    const char *prompt = (sh->prompt != NULL) ? sh->prompt : "$ ";

    emit(sh, "\r\033[K", 4);
    emit(sh, prompt, strlen(prompt));

    /* **伏せ字のときは行の中身を出さない。** ここが唯一の出口なので、
     * cdk_shell_notify（WiFi の知らせ）が割り込んで描き直しても漏れない。
     *
     * 字数は出す。何文字打てたか分からないと打ち直しの判断ができない
     * （`set` がパスワードの文字数だけ出すのと同じ考え）。 */
    int stars = 0;
    if (sh->asking) {
        for (int i = 0; i < sh->len; i++) {
            /* UTF-8 の続きのバイトは数えない。1 文字 = `*` 1 つ。 */
            if (((unsigned char)sh->line[i] & 0xC0u) != 0x80u) {
                emit(sh, "*", 1);
                stars++;
            }
        }
    } else {
        emit(sh, sh->line, (size_t)sh->len);
    }

    /* カーソルを行頭に戻してから、プロンプト＋カーソル位置まで右へ動かす。
     * 伏せ字のときカーソルは常に末尾なので、`*` の数がそのまま桁になる。 */
    const int width =
        cdk_shell_visible_width(prompt, strlen(prompt)) +
        (sh->asking ? stars
                    : cdk_shell_visible_width(sh->line, (size_t)sh->cursor));

    emit(sh, "\r", 1);
    if (width > 0) {
        char buf[16];
        const int n = snprintf(buf, sizeof(buf), "\033[%dC", width);
        emit(sh, buf, (size_t)n);
    }
}

/* ------------------------------------------------------------------ */
/* 行編集                                                              */
/* ------------------------------------------------------------------ */

/* cursor の直前にある 1 文字の先頭バイト位置を返す（UTF-8 を跨がない） */
static int prev_char_start(const cdk_shell_t *sh)
{
    int i = sh->cursor - 1;
    while (i > 0 && ((unsigned char)sh->line[i] & 0xC0u) == 0x80u) {
        i--;
    }
    return (i < 0) ? 0 : i;
}

/* cursor にある 1 文字のバイト数 */
static int cur_char_len(const cdk_shell_t *sh)
{
    if (sh->cursor >= sh->len) {
        return 0;
    }
    int n = 1;
    while (sh->cursor + n < sh->len &&
           ((unsigned char)sh->line[sh->cursor + n] & 0xC0u) == 0x80u) {
        n++;
    }
    return n;
}

static void insert_bytes(cdk_shell_t *sh, const char *bytes, int n)
{
    if (sh->len + n >= CDK_SHELL_LINE_MAX) {
        return; /* 入りきらない。黙って捨てる */
    }
    memmove(sh->line + sh->cursor + n, sh->line + sh->cursor,
            (size_t)(sh->len - sh->cursor));
    memcpy(sh->line + sh->cursor, bytes, (size_t)n);
    sh->len += n;
    sh->cursor += n;
    sh->line[sh->len] = '\0';
}

static void delete_range(cdk_shell_t *sh, int from, int to)
{
    if (from < 0) {
        from = 0;
    }
    if (to > sh->len) {
        to = sh->len;
    }
    if (from >= to) {
        return;
    }
    memmove(sh->line + from, sh->line + to, (size_t)(sh->len - to));
    sh->len -= (to - from);
    sh->cursor = from;
    sh->line[sh->len] = '\0';
}

static void clear_line(cdk_shell_t *sh)
{
    sh->len = 0;
    sh->cursor = 0;
    sh->line[0] = '\0';
}

/* ------------------------------------------------------------------ */
/* 履歴                                                                */
/* ------------------------------------------------------------------ */

static void history_push(cdk_shell_t *sh, const char *line)
{
    if (line[0] == '\0') {
        return;
    }
    /* 直前と同じなら積まない */
    if (sh->hist_count > 0 && strcmp(sh->hist[0], line) == 0) {
        return;
    }
    for (int i = CDK_SHELL_HISTORY_MAX - 1; i > 0; i--) {
        memcpy(sh->hist[i], sh->hist[i - 1], CDK_SHELL_LINE_MAX);
    }
    strncpy(sh->hist[0], line, CDK_SHELL_LINE_MAX - 1);
    sh->hist[0][CDK_SHELL_LINE_MAX - 1] = '\0';
    if (sh->hist_count < CDK_SHELL_HISTORY_MAX) {
        sh->hist_count++;
    }
}

static void history_load(cdk_shell_t *sh, int pos)
{
    if (pos < 0) {
        /* 編集中の内容に戻る */
        strncpy(sh->line, sh->stash, CDK_SHELL_LINE_MAX - 1);
        sh->line[CDK_SHELL_LINE_MAX - 1] = '\0';
    } else {
        strncpy(sh->line, sh->hist[pos], CDK_SHELL_LINE_MAX - 1);
        sh->line[CDK_SHELL_LINE_MAX - 1] = '\0';
    }
    sh->len = (int)strlen(sh->line);
    sh->cursor = sh->len;
    sh->hist_pos = pos;
}

/* ------------------------------------------------------------------ */
/* 分割                                                                */
/* ------------------------------------------------------------------ */

int cdk_shell_split(char *line, char **argv, int max)
{
    int argc = 0;
    char *w = line; /* 書き込み位置。引用符を取り除くので読みと分ける */
    const char *r = line;

    while (*r != '\0' && argc < max) {
        while (*r == ' ' || *r == '\t') {
            r++;
        }
        if (*r == '\0') {
            break;
        }
        argv[argc++] = w;

        char quote = '\0';
        while (*r != '\0') {
            const char c = *r;
            if (quote != '\0') {
                if (c == quote) {
                    quote = '\0';
                    r++;
                    continue;
                }
            } else if (c == '"' || c == '\'') {
                quote = c;
                r++;
                continue;
            } else if (c == ' ' || c == '\t') {
                break;
            }
            *w++ = *r++;
        }
        /* 区切り文字を読む前に終端を書くと、w が r に追いついているとき
         * （引用符が無い普通の入力）に区切り文字自体を NUL で潰してしまう。
         * 「まだ続きがあるか」を先に控えてから終端を書くこと。 */
        const bool more = (*r != '\0');
        *w++ = '\0';
        if (more) {
            r++;
        }
    }
    return argc;
}

/* ------------------------------------------------------------------ */
/* 実行                                                                */
/* ------------------------------------------------------------------ */

/* line は呼び出し側が退避したもの。sh->line は先に空にしておくこと。
 * コマンドがシェルを止めたり、自分で出力したりするので、
 * 実行中に編集中の行が残っていると状態がもつれる。 */
static void execute(cdk_shell_t *sh, const char *line)
{
    char work[CDK_SHELL_LINE_MAX];
    strncpy(work, line, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';

    char *argv[CDK_SHELL_ARGS_MAX];
    const int argc = cdk_shell_split(work, argv, CDK_SHELL_ARGS_MAX);
    if (argc == 0) {
        return;
    }

    for (size_t i = 0; i < sh->ncmds; i++) {
        if (strcmp(sh->cmds[i].name, argv[0]) == 0) {
            sh->last_status = sh->cmds[i].fn(argc, argv, sh->cmd_user);
            return;
        }
    }
    cdk_shell_printf(sh, "\033[31m%s: そんなコマンドはありません\033[0m\r\n", argv[0]);
    sh->last_status = 127;
}

/* コマンド名を補完する。候補が 1 つなら確定、複数なら一覧を出す。 */
static void complete(cdk_shell_t *sh)
{
    /* 1 語目だけ補完する。引数の補完はコマンド側の知識が要るので今はやらない。 */
    for (int i = 0; i < sh->cursor; i++) {
        if (sh->line[i] == ' ') {
            return;
        }
    }

    const int n = sh->cursor;
    const cdk_shell_cmd_t *match = NULL;
    int matches = 0;
    for (size_t i = 0; i < sh->ncmds; i++) {
        if (strncmp(sh->cmds[i].name, sh->line, (size_t)n) == 0) {
            match = &sh->cmds[i];
            matches++;
        }
    }

    if (matches == 1) {
        const int full = (int)strlen(match->name);
        if (full > n) {
            clear_line(sh);
            insert_bytes(sh, match->name, full);
        }
        insert_bytes(sh, " ", 1);
        redraw(sh);
        return;
    }
    if (matches > 1) {
        cdk_shell_write(sh, "\r\n");
        for (size_t i = 0; i < sh->ncmds; i++) {
            if (strncmp(sh->cmds[i].name, sh->line, (size_t)n) == 0) {
                cdk_shell_printf(sh, "%s  ", sh->cmds[i].name);
            }
        }
        cdk_shell_write(sh, "\r\n");
        redraw(sh);
    }
}

/* ------------------------------------------------------------------ */
/* 公開 API                                                            */
/* ------------------------------------------------------------------ */

void cdk_shell_init(cdk_shell_t *sh, cdk_shell_out_fn out, void *out_user)
{
    memset(sh, 0, sizeof(*sh));
    sh->out = out;
    sh->out_user = out_user;
    sh->prompt = "\033[33m$\033[0m ";
    sh->hist_pos = -1;
}

void cdk_shell_set_commands(cdk_shell_t *sh, const cdk_shell_cmd_t *cmds, size_t n,
                            void *cmd_user)
{
    sh->cmds = cmds;
    sh->ncmds = n;
    sh->cmd_user = cmd_user;
}

/* 伏せ字入力をやめて普通の行に戻す。**cb は呼ばない。**
 *
 * 打ちかけの中身も消す。SSH が落ちて enter_shell() まで戻ってきたときに
 * 打ちかけのパスワードが残っていると、次のコマンド行に混ざる。 */
static void ask_reset(cdk_shell_t *sh)
{
    if (!sh->asking) {
        return;
    }
    sh->asking = false;
    sh->prompt = sh->ask_saved_prompt;
    sh->ask_cb = NULL;
    sh->ask_user = NULL;
    sh->ask_over = false;
    memset(sh->line, 0, sizeof(sh->line));
    clear_line(sh);
}

void cdk_shell_begin(cdk_shell_t *sh)
{
    /* **聞いている途中で持ち主が戻ってきたら、聞くのをやめる。**
     * 残すと、SSH が切れて戻ったシェルが伏せ字のまま待ち続ける。 */
    ask_reset(sh);
    sh->active = true;
    clear_line(sh);
    sh->hist_pos = -1;
    redraw(sh);
}

void cdk_shell_end(cdk_shell_t *sh)
{
    ask_reset(sh);
    sh->active = false;
}

void cdk_shell_ask(cdk_shell_t *sh, const char *prompt, int maxlen,
                   cdk_shell_ask_fn cb, void *user)
{
    if (sh == NULL || cb == NULL || maxlen <= 0) {
        return;
    }
    if (maxlen > CDK_SHELL_LINE_MAX - 1) {
        maxlen = CDK_SHELL_LINE_MAX - 1;
    }
    /* 二重に聞かれたら前のは捨てる（前の cb は呼ばない）。 */
    ask_reset(sh);

    sh->ask_saved_prompt = sh->prompt;
    sh->prompt = prompt;
    sh->ask_max = maxlen;
    sh->ask_over = false;
    sh->ask_cb = cb;
    sh->ask_user = user;
    sh->asking = true;
    sh->active = true;
    clear_line(sh);
    redraw(sh);
}

bool cdk_shell_asking(const cdk_shell_t *sh)
{
    return (sh != NULL) && sh->asking;
}

bool cdk_shell_split_userhost(const char *s, char *user, size_t ucap, char *host,
                              size_t hcap)
{
    if (s == NULL || user == NULL || host == NULL || ucap == 0 || hcap == 0) {
        return false;
    }
    const char *at = strrchr(s, '@');
    if (at == NULL || at == s || at[1] == '\0') {
        return false; /* `@` が無い / ユーザーが空 / ホストが空 */
    }
    const size_t ulen = (size_t)(at - s);
    const size_t hlen = strlen(at + 1);
    if (ulen + 1u > ucap || hlen + 1u > hcap) {
        return false; /* 切り詰めない。別の相手になる */
    }
    memcpy(user, s, ulen);
    user[ulen] = '\0';
    memcpy(host, at + 1, hlen);
    host[hlen] = '\0';
    return true;
}

/* 向こう側を指す `:` を返す。無ければ NULL。
 *
 * **`/` より前にある `:` だけを数える。** これが Linux の scp の規則で、
 * `/sdcard/a:b` を手元のパスとして扱うために要る。逆にしてしまうと、
 * コロンを含む名前のファイルが送れなくなる。 */
static const char *remote_colon(const char *s)
{
    for (const char *p = s; *p != '\0'; p++) {
        if (*p == '/') {
            return NULL; /* パスが始まった。ここから先の `:` は名前の一部 */
        }
        if (*p == ':') {
            return p;
        }
    }
    return NULL;
}

/* 収まるなら写して true。**入らないなら何もしないで false。** */
static bool fit(char *dst, size_t cap, const char *src, size_t len)
{
    if (len + 1u > cap) {
        return false;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
    return true;
}

cdk_scp_parse_t cdk_shell_scp_parse(const char *a, const char *b,
                                    cdk_scp_args_t *out)
{
    if (a == NULL || b == NULL || out == NULL) {
        return CDK_SCP_E_BOTH_LOCAL;
    }
    const char *ca = remote_colon(a);
    const char *cb = remote_colon(b);

    if (ca != NULL && cb != NULL) {
        return CDK_SCP_E_BOTH_REMOTE;
    }
    if (ca == NULL && cb == NULL) {
        return CDK_SCP_E_BOTH_LOCAL;
    }

    const bool upload = (cb != NULL); /* 向こう側が後ろなら送信 */
    const char *rspec = upload ? b : a;      /* user@host:path */
    const char *rcolon = upload ? cb : ca;
    const char *local = upload ? a : b;

    if (local[0] == '\0') {
        return CDK_SCP_E_NO_LOCAL;
    }

    /* `user@host` の部分。`@` が無ければ断る。 */
    const size_t uhlen = (size_t)(rcolon - rspec);
    bool has_at = false;
    for (size_t i = 0; i < uhlen; i++) {
        if (rspec[i] == '@') {
            has_at = true;
            break;
        }
    }
    if (!has_at) {
        return CDK_SCP_E_NO_USER;
    }

    /* 分解のために一度切り出す。**out へは最後にまとめて書く。** */
    char uh[128];
    if (!fit(uh, sizeof(uh), rspec, uhlen)) {
        return CDK_SCP_E_TOOLONG;
    }

    cdk_scp_args_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    if (!cdk_shell_split_userhost(uh, tmp.user, sizeof(tmp.user), tmp.host,
                                  sizeof(tmp.host))) {
        return CDK_SCP_E_USERHOST;
    }

    const char *rpath = rcolon + 1;
    if (rpath[0] == '\0') {
        /* 送信なら手元の名前を使えるが、**受信では何を取るか決まらない。** */
        if (!upload) {
            return CDK_SCP_E_NO_REMOTE;
        }
        tmp.remote_is_empty = true;
    }
    if (!fit(tmp.remote, sizeof(tmp.remote), rpath, strlen(rpath))) {
        return CDK_SCP_E_TOOLONG;
    }
    if (!fit(tmp.local, sizeof(tmp.local), local, strlen(local))) {
        return CDK_SCP_E_TOOLONG;
    }
    tmp.upload = upload;

    *out = tmp;
    return CDK_SCP_OK;
}

void cdk_shell_notify(cdk_shell_t *sh, const char *s)
{
    if (!sh->active || s == NULL) {
        return;
    }
    emit(sh, "\r\033[K", 4);
    emit(sh, s, strlen(s));
    emit(sh, "\r\n", 2);
    redraw(sh);
}

bool cdk_shell_active(const cdk_shell_t *sh)
{
    return sh->active;
}

/* 伏せ字入力のキー。**普通の行編集の表を通さない。**
 *
 * 履歴（Ctrl-P も ↑ も）と補完を通すと、打った合言葉が履歴や候補一覧に
 * 出る。カーソル移動を許さないのは、`*` の下のどこに居るのかを見せられない
 * から（見せるとその位置の字が何かを一つずつ当てられる）。
 * 通すのは「打つ・1 文字消す・全部消す・出す・やめる」だけ。 */
static void ask_key(cdk_shell_t *sh, const cdk_key_t *k)
{
    const bool ctrl = (k->mods & CDK_MOD_CTRL_BIT) != 0;
    const bool cancel_key =
        (k->keycode == CDK_K_ESC) ||
        (ctrl && (k->codepoint == 'c' || k->codepoint == 'C'));

    if (k->keycode == CDK_K_ENTER || cancel_key) {
        /* cb と「長すぎた」は ask_reset で消えるので先に写す。 */
        const cdk_shell_ask_fn cb = sh->ask_cb;
        void                  *user = sh->ask_user;
        const bool             over = sh->ask_over;
        const int              cap = sh->ask_max;

        char answer[CDK_SHELL_LINE_MAX];
        memcpy(answer, sh->line, (size_t)sh->len);
        answer[sh->len] = '\0';

        cdk_shell_write(sh, (cancel_key && k->keycode != CDK_K_ESC) ? "^C\r\n"
                                                                    : "\r\n");
        ask_reset(sh); /* ここで sh->line が消える */

        if (over && !cancel_key) { /* やめた人に長さの文句は要らない */
            /* **切り詰めて送らない。** 途中まで合っている合言葉は
             * 「打ったのに違うと言われる」になって原因が分からない。 */
            cdk_shell_printf(sh, "\033[31m長すぎます（%d バイトまで）\033[0m\r\n",
                             cap);
        }
        if (cb != NULL) {
            /* やめたときは**打ちかけを渡さない**。空の Enter も「やめた」
             * 扱い（port 側は空文字を「パスワード無し」と見るので、渡しても
             * 認証を叩かずに落ちるだけになる）。 */
            const bool give = !cancel_key && !over && answer[0] != '\0';
            cb(user, give ? answer : NULL);
        }
        memset(answer, 0, sizeof(answer));

        /* cb が SSH を張ったならシェルは止まっている（描き直さない）。 */
        if (sh->active) {
            redraw(sh);
        }
        return;
    }

    if (k->keycode == CDK_K_BS) {
        if (sh->cursor > 0) {
            delete_range(sh, prev_char_start(sh), sh->cursor);
            redraw(sh);
        }
        return;
    }

    if (ctrl) {
        if (k->codepoint == 'u') { /* 打ち直す */
            clear_line(sh);
            sh->ask_over = false;
            redraw(sh);
        }
        return; /* 他の Ctrl 組み合わせは伏せ字では使わない */
    }

    if (k->keycode == CDK_K_SPACE) {
        /* 空白も合言葉に入りうる。**黙って落とさない。** */
        if (sh->len + 1 > sh->ask_max) {
            sh->ask_over = true;
            return;
        }
        insert_bytes(sh, " ", 1);
        redraw(sh);
        return;
    }

    if (k->keycode != CDK_K_NONE || k->codepoint == 0) {
        return; /* Tab・矢印・Home などは無視 */
    }

    uint8_t      buf[4];
    const size_t n = cdk_utf8_encode(k->codepoint, buf);
    if (sh->len + (int)n > sh->ask_max) {
        sh->ask_over = true;
        return;
    }
    insert_bytes(sh, (const char *)buf, (int)n);
    redraw(sh);
}

void cdk_shell_key(cdk_shell_t *sh, const cdk_key_t *k)
{
    if (!sh->active || k == NULL) {
        return;
    }

    if (sh->asking) {
        ask_key(sh, k);
        return;
    }

    const bool ctrl = (k->mods & CDK_MOD_CTRL_BIT) != 0;

    switch (k->keycode) {
    case CDK_K_ENTER:
        cdk_shell_write(sh, "\r\n");
        history_push(sh, sh->line);
        {
            /* 実行前に行を空にする。コマンドがシェルを止めた場合でも、
             * 送信済みの行が残っていると次に戻ってきたとき二重に見える。 */
            char submitted[CDK_SHELL_LINE_MAX];
            strncpy(submitted, sh->line, sizeof(submitted) - 1);
            submitted[sizeof(submitted) - 1] = '\0';
            clear_line(sh);
            sh->hist_pos = -1;
            execute(sh, submitted);
        }
        if (sh->active) { /* コマンドがシェルを止めたなら描き直さない */
            redraw(sh);
        }
        return;

    case CDK_K_BS:
        if (sh->cursor > 0) {
            delete_range(sh, prev_char_start(sh), sh->cursor);
            redraw(sh);
        }
        return;

    case CDK_K_DELETE: {
        const int n = cur_char_len(sh);
        if (n > 0) {
            delete_range(sh, sh->cursor, sh->cursor + n);
            redraw(sh);
        }
        return;
    }

    case CDK_K_LEFT:
        if (sh->cursor > 0) {
            sh->cursor = prev_char_start(sh);
            redraw(sh);
        }
        return;

    case CDK_K_RIGHT:
        if (sh->cursor < sh->len) {
            sh->cursor += cur_char_len(sh);
            redraw(sh);
        }
        return;

    case CDK_K_HOME:
        sh->cursor = 0;
        redraw(sh);
        return;

    case CDK_K_END:
        sh->cursor = sh->len;
        redraw(sh);
        return;

    case CDK_K_UP:
        if (sh->hist_count > 0 && sh->hist_pos + 1 < sh->hist_count) {
            if (sh->hist_pos < 0) {
                strncpy(sh->stash, sh->line, CDK_SHELL_LINE_MAX - 1);
                sh->stash[CDK_SHELL_LINE_MAX - 1] = '\0';
            }
            history_load(sh, sh->hist_pos + 1);
            redraw(sh);
        }
        return;

    case CDK_K_DOWN:
        if (sh->hist_pos >= 0) {
            history_load(sh, sh->hist_pos - 1);
            redraw(sh);
        }
        return;

    case CDK_K_TAB:
        complete(sh);
        return;

    case CDK_K_SPACE:
        if (ctrl) {
            return; /* Ctrl+Space は使わない */
        }
        insert_bytes(sh, " ", 1);
        redraw(sh);
        return;

    case CDK_K_ESC:
        return;

    default:
        break;
    }

    if (k->codepoint == 0) {
        return;
    }

    if (ctrl) {
        switch (k->codepoint) {
        case 'a': sh->cursor = 0; redraw(sh); return;
        case 'e': sh->cursor = sh->len; redraw(sh); return;
        case 'b':
            if (sh->cursor > 0) {
                sh->cursor = prev_char_start(sh);
                redraw(sh);
            }
            return;
        case 'f':
            if (sh->cursor < sh->len) {
                sh->cursor += cur_char_len(sh);
                redraw(sh);
            }
            return;
        case 'u': /* カーソルより前を消す */
            delete_range(sh, 0, sh->cursor);
            redraw(sh);
            return;
        case 'k': /* カーソルより後ろを消す */
            sh->len = sh->cursor;
            sh->line[sh->len] = '\0';
            redraw(sh);
            return;
        case 'w': { /* 直前の単語を消す */
            int i = sh->cursor;
            while (i > 0 && sh->line[i - 1] == ' ') {
                i--;
            }
            while (i > 0 && sh->line[i - 1] != ' ') {
                i--;
            }
            delete_range(sh, i, sh->cursor);
            redraw(sh);
            return;
        }
        case 'c': /* 入力を捨てて新しい行 */
            cdk_shell_write(sh, "^C\r\n");
            clear_line(sh);
            sh->hist_pos = -1;
            redraw(sh);
            return;
        case 'l': /* 画面消去 */
            cdk_shell_write(sh, "\033[2J\033[H");
            redraw(sh);
            return;
        case 'd':
            if (sh->len == 0) {
                return; /* シェルからは抜けない */
            }
            {
                const int n = cur_char_len(sh);
                if (n > 0) {
                    delete_range(sh, sh->cursor, sh->cursor + n);
                    redraw(sh);
                }
            }
            return;
        default:
            return; /* 知らない Ctrl 組み合わせは無視 */
        }
    }

    /* 普通の文字 */
    uint8_t buf[4];
    const size_t n = cdk_utf8_encode(k->codepoint, buf);
    insert_bytes(sh, (const char *)buf, (int)n);
    redraw(sh);
}
