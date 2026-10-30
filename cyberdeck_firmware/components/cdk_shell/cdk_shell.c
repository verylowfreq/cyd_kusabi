#include "cdk_shell.h"

#include "cdk_text.h"

#include <stdint.h>
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

/* s[i] から 1 文字ぶん進める。戻り値は次の位置（0 なら途中で切れている）。
 * *w にその文字の桁数が入る。
 *
 * エスケープ列は幅を持たない。プロンプトに色を付けると SGR が入るので、
 * ここを素通しにするとカーソルがその文字数だけ右にずれる。
 * 全角は 2 桁として数える。 */
static size_t next_glyph(const char *s, size_t len, size_t i, int *w)
{
    *w = 0;
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
        return i;
    }

    uint32_t cp = 0;
    bool ok = false;
    const size_t used = cdk_utf8_decode((const uint8_t *)s + i, len - i, &cp, &ok);
    if (used == 0) {
        return 0; /* 途中で切れている */
    }
    *w = ok ? cdk_char_width(cp) : 1;
    return i + used;
}

/* 画面上で何桁ぶんになるかを数える。 */
int cdk_shell_visible_width(const char *s, size_t len)
{
    int width = 0;
    size_t i = 0;

    while (i < len && s[i] != '\0') {
        int w = 0;
        const size_t next = next_glyph(s, len, i, &w);
        if (next == 0) {
            break;
        }
        width += w;
        i = next;
    }
    return width;
}

/* ---- 折り返した行のどこに何があるか ----
 *
 * **cdk_term と同じ規則で数える。** ずれると、描き直すたびに行が 1 段ずつ
 * 上か下へ滑っていく。
 *   - 遅延折返し: 右端に書いた時点では折り返さず、次の字が来たら改行する
 *   - 全角が右端 1 桁に入らないときは、その桁を空けて次の段へ送る */
typedef struct {
    int  row;     /* 行頭（プロンプトの先頭）から何段目か */
    int  col;
    bool pending; /* 右端に書いた直後（次の字で折り返す） */
} layout_t;

static void layout_put(layout_t *p, int cols, int w)
{
    if (w <= 0) {
        return; /* 結合文字は cdk_term も捨てる */
    }
    if (p->pending) {
        p->row++;
        p->col = 0;
        p->pending = false;
    }
    if (w == 2 && p->col + 1 > cols - 1) {
        p->row++;
        p->col = 0;
    }
    p->col += w;
    if (p->col > cols - 1) {
        p->col = cols - 1;
        p->pending = true;
    }
}

static void layout_feed(layout_t *p, int cols, const char *s, size_t len)
{
    size_t i = 0;
    while (i < len && s[i] != '\0') {
        int w = 0;
        const size_t next = next_glyph(s, len, i, &w);
        if (next == 0) {
            break;
        }
        layout_put(p, cols, w);
        i = next;
    }
}

/* 前に描いた行を消して、カーソルを行頭（プロンプトの先頭）に置く。
 *
 * **折り返していたら上の段から消す。** CR だけでは今いる段の頭にしか
 * 戻らないので、上の段が残ったまま下に描き直すことになる（40 桁を
 * 超えたコマンドが 1 字打つたびに 1 段ずつ増えていった）。 */
static void erase_drawn(cdk_shell_t *sh)
{
    if (sh->cols <= 0) {
        emit(sh, "\r\033[K", 4);
        return;
    }
    if (sh->drawn_cur_row > 0) {
        char buf[16];
        const int n = snprintf(buf, sizeof(buf), "\033[%dA", sh->drawn_cur_row);
        emit(sh, buf, (size_t)n);
    }
    /* 折り返していたら下を全部消す（段の数だけ EL を打つより確実。
     * シェルの行はいつも出力の最後にある）。1 段なら EL だけにして、
     * 下にあるものには触らない。 */
    emit(sh, (sh->drawn_end_row > 0) ? "\r\033[J" : "\r\033[K", 4);
    sh->drawn_cur_row = 0;
    sh->drawn_end_row = 0;
}

/* 行を丸ごと描き直す。
 *
 * 差分更新はしない。消して書き直す方が、遅くても常に正しい。
 * 1 行 256 文字なので実用上も問題にならない。
 *
 * 桁数 (cols) を知っているときは折り返しを追う。知らないときは 1 段に
 * 収まる前提で CR だけで戻る（cdk_shell_set_cols を呼ばない使い方）。
 *
 * 行が画面の段数より高くなると上が流れて戻れなくなる。256 字は 40 桁で
 * 7 段なので、今の画面では起きない。 */
static void redraw(cdk_shell_t *sh)
{
    const char *prompt = (sh->prompt != NULL) ? sh->prompt : "$ ";

    erase_drawn(sh);
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

    if (sh->cols > 0) {
        /* 書き終えた位置（end）と、カーソルを置く位置（cur）を数える。
         * 伏せ字のときカーソルは常に末尾。 */
        layout_t end = {0, 0, false};
        layout_feed(&end, sh->cols, prompt, strlen(prompt));
        layout_t cur = end;
        if (sh->asking) {
            for (int i = 0; i < stars; i++) {
                layout_put(&end, sh->cols, 1);
            }
            cur = end;
        } else {
            layout_feed(&cur, sh->cols, sh->line, (size_t)sh->cursor);
            end = cur;
            layout_feed(&end, sh->cols, sh->line + sh->cursor,
                        (size_t)(sh->len - sh->cursor));
        }

        /* カーソルは「次の字が置かれる桁」に置く。右端に書いた直後なら
         * 次の段の頭。カーソル下の字が全角で右端に入らないときも次の段。 */
        int trow = cur.row;
        int tcol = cur.col;
        if (cur.pending) {
            trow++;
            tcol = 0;
        } else if (!sh->asking && sh->cursor < sh->len) {
            int w = 0;
            (void)next_glyph(sh->line, (size_t)sh->len, (size_t)sh->cursor, &w);
            if (w == 2 && tcol + 1 > sh->cols - 1) {
                trow++;
                tcol = 0;
            }
        }

        if (trow > end.row) {
            /* 末尾がちょうど右端で終わった。次の段はまだ無いので作る。 */
            emit(sh, "\r\n", 2);
        } else {
            if (end.row > trow) {
                char buf[16];
                const int n = snprintf(buf, sizeof(buf), "\033[%dA", end.row - trow);
                emit(sh, buf, (size_t)n);
            }
            emit(sh, "\r", 1);
            if (tcol > 0) {
                char buf[16];
                const int n = snprintf(buf, sizeof(buf), "\033[%dC", tcol);
                emit(sh, buf, (size_t)n);
            }
        }
        sh->drawn_cur_row = trow;
        sh->drawn_end_row = (trow > end.row) ? trow : end.row;
        return;
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

/* 行を離れる（Enter・Ctrl-C・候補一覧）前に、カーソルを行の末尾へ置く。
 *
 * 折り返しの途中の段で改行すると、続く出力が行の残りの段に上書きされる。
 * `^C` も途中に書くと行の字を潰す。末尾に置いて描き直せば、どちらも
 * 行の後ろから始まる。 */
static void leave_drawn(cdk_shell_t *sh)
{
    if (sh->cols > 0 && sh->cursor != sh->len) {
        const int saved = sh->cursor;
        sh->cursor = sh->len;
        redraw(sh);
        sh->cursor = saved;
    }
    sh->drawn_cur_row = 0;
    sh->drawn_end_row = 0;
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
        leave_drawn(sh);
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

void cdk_shell_set_cols(cdk_shell_t *sh, int cols)
{
    sh->cols = (cols > 0) ? cols : 0;
    sh->drawn_cur_row = 0;
    sh->drawn_end_row = 0;
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
    /* 止まっている間に別の誰か（SSH・エディタ）が画面を使った。前に描いた
     * 行の形はもう当てにならないので、今いる段から新しく描く。 */
    sh->drawn_cur_row = 0;
    sh->drawn_end_row = 0;
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

/* ---- uart の引数 ---- */

/* 10 進の数だけを読む。`atoi` は "9600x" を 9600 として通してしまい、
 * 打ち間違いが黙って別の速度になる。溢れも見る。 */
static bool parse_baud(const char *s, int *out)
{
    if (s == NULL || s[0] == '\0') return false;
    long v = 0;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + (*p - '0');
        if (v > 3000000) return false; /* 上限を超えたら即やめる */
    }
    if (v < 300) return false;
    *out = (int)v;
    return true;
}

bool cdk_shell_uart_parse(int argc, char **argv, cdk_uart_args_t *out,
                          const char **why)
{
    const char *reason = NULL;
    cdk_uart_args_t tmp;
    tmp.baud = 115200;
    tmp.flow = false;
    bool got_baud = false;

    if (out == NULL || argv == NULL) {
        reason = "引数がありません";
        goto fail;
    }

    /* argv[0] はコマンド名。1 から見る。 */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a == NULL) {
            reason = "引数がありません";
            goto fail;
        }
        if (strcmp(a, "--flow") == 0 || strcmp(a, "-f") == 0) {
            tmp.flow = true;
            continue;
        }
        if (strcmp(a, "--noflow") == 0) {
            tmp.flow = false;
            continue;
        }
        if (a[0] == '-') {
            reason = "知らない指定です";
            goto fail;
        }
        if (got_baud) {
            reason = "速度は 1 つだけです";
            goto fail;
        }
        if (!parse_baud(a, &tmp.baud)) {
            reason = "速度は 300〜3000000 の数で指定してください";
            goto fail;
        }
        got_baud = true;
    }

    *out = tmp;
    if (why != NULL) *why = NULL;
    return true;

fail:
    if (why != NULL) *why = reason;
    return false;
}

/* ---- wget の引数 ---- */

/* `http://` / `https://` を大文字小文字を問わず剥がす。
 * 当たらなければ NULL。 */
static const char *skip_http_scheme(const char *url, bool *https)
{
    static const char *const names[] = {"http://", "https://"};
    for (int k = 0; k < 2; k++) {
        const size_t n = strlen(names[k]);
        size_t       i = 0;
        for (; i < n; i++) {
            char c = url[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != names[k][i]) break;
        }
        if (i == n) {
            if (https != NULL) *https = (k == 1);
            return url + n;
        }
    }
    return NULL;
}

cdk_wget_parse_t cdk_shell_wget_parse(const char *url, cdk_wget_args_t *out)
{
    if (url == NULL || out == NULL) return CDK_WGET_E_SCHEME;

    bool        https = false;
    const char *auth = skip_http_scheme(url, &https);
    if (auth == NULL) return CDK_WGET_E_SCHEME;

    for (const char *p = url; *p != '\0'; p++) {
        const unsigned char c = (unsigned char)*p;
        if (c <= 0x20 || c == 0x7F) return CDK_WGET_E_BADCHAR;
    }

    /* 相手は `/ ? #` の手前まで。`user@` と `:port` を除いて空なら断る。 */
    const size_t alen = strcspn(auth, "/?#");
    const char  *host = auth;
    for (size_t i = 0; i < alen; i++) {
        if (auth[i] == '@') host = auth + i + 1;
    }
    const size_t hlen = (size_t)(auth + alen - host);
    if (hlen == 0 || host[0] == ':') return CDK_WGET_E_NO_HOST;

    if (strlen(url) >= sizeof(out->url)) return CDK_WGET_E_TOOLONG;

    snprintf(out->url, sizeof(out->url), "%s", url);
    out->https = https;
    return CDK_WGET_OK;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* s[i] から始まる UTF-8 の 1 文字の長さ。壊れていれば 0。
 * 冗長な表現とサロゲートも壊れているとみなす（FATFS が受け付けない）。
 *
 * `cdk_utf8_decode` を使わないのは、あれが描画向けで「不正バイトを消費
 * しない（0 を返す）」ことがあり、1 バイトずつ `_` に置き換える用途に
 * 合わないため。 */
static size_t utf8_len_at(const unsigned char *s, size_t n, size_t i)
{
    const unsigned char c = s[i];
    if (c < 0x80) return 1;
    size_t   len;
    uint32_t cp;
    if (c >= 0xC2 && c <= 0xDF) {
        len = 2;
        cp = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
        len = 3;
        cp = c & 0x0F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4;
        cp = c & 0x07;
    } else {
        return 0;
    }
    if (i + len > n) return 0;
    for (size_t k = 1; k < len; k++) {
        if ((s[i + k] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (s[i + k] & 0x3F);
    }
    if (len == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) return 0;
    if (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) return 0;
    return len;
}

/* 保存名の上限（バイト）。FAT の LFN は 255 文字なので、バイトで
 * 255 に収めれば必ず入る。 */
#define WGET_NAME_MAX 255

bool cdk_shell_wget_name(const char *url, char *out, size_t cap)
{
    static const char fallback[] = "index.html";
    if (out == NULL || cap < sizeof(fallback)) return false;
    out[0] = '\0';
    if (url == NULL) url = "";

    /* 名前の部分を取り出す: 相手の後ろのパスの、最後の `/` より後ろ。 */
    const char *p = skip_http_scheme(url, NULL);
    if (p == NULL) p = url;
    const size_t end = strcspn(p, "?#");
    const char  *slash = NULL;
    for (size_t i = 0; i < end; i++) {
        if (p[i] == '/') slash = p + i;
    }
    const char  *seg = (slash != NULL) ? slash + 1 : p + end; /* パスが無い */
    const size_t seglen = (size_t)(p + end - seg);

    /* `%XX` を復号しながら、使えない文字を `_` に。 */
    unsigned char buf[WGET_NAME_MAX * 3 + 1];
    size_t        n = 0;
    for (size_t i = 0; i < seglen && n < sizeof(buf) - 1; i++) {
        unsigned char c = (unsigned char)seg[i];
        if (c == '%' && i + 2 < seglen) {
            const int hi = hex_val(seg[i + 1]);
            const int lo = hex_val(seg[i + 2]);
            if (hi >= 0 && lo >= 0) {
                c = (unsigned char)(hi * 16 + lo);
                i += 2;
            }
        }
        if (c < 0x20 || c == 0x7F || strchr("\\/:*?\"<>|", (int)c) != NULL) {
            c = '_';
        }
        buf[n++] = c;
    }

    /* 壊れた UTF-8 は 1 バイトずつ `_` にする。 */
    for (size_t i = 0; i < n;) {
        const size_t l = utf8_len_at(buf, n, i);
        if (l == 0) {
            buf[i++] = '_';
        } else {
            i += l;
        }
    }

    /* 先頭の空白、末尾の `.` と空白を削る。 */
    size_t head = 0;
    while (head < n && buf[head] == ' ') head++;
    while (n > head && (buf[n - 1] == '.' || buf[n - 1] == ' ')) n--;
    const unsigned char *name = buf + head;
    size_t               len = n - head;

    if (len == 0) {
        snprintf(out, cap, "%s", fallback);
        return true;
    }

    size_t max = cap - 1;
    if (max > WGET_NAME_MAX) max = WGET_NAME_MAX;

    if (len <= max) {
        memcpy(out, name, len);
        out[len] = '\0';
        return true;
    }

    /* 長い。拡張子（先頭以外の最後の `.` から、16 バイトまで）を残す。 */
    size_t ext = 0;
    for (size_t i = len; i > 1; i--) {
        if (name[i - 1] == '.') {
            if (len - (i - 1) <= 16) ext = len - (i - 1);
            break;
        }
    }
    if (ext >= max) ext = 0;

    /* 前半を文字境界で切る。 */
    const size_t room = max - ext;
    size_t       cut = 0;
    while (cut < len - ext) {
        size_t l = utf8_len_at(name, len, cut);
        if (l == 0) l = 1; /* 上で直してあるので来ないが、止まらないように */
        if (cut + l > room) break;
        cut += l;
    }
    memcpy(out, name, cut);
    memcpy(out + cut, name + len - ext, ext);
    out[cut + ext] = '\0';
    return true;
}

bool cdk_shell_wget_numbered(const char *path, int n, char *out, size_t cap)
{
    if (path == NULL || out == NULL || cap == 0 || n < 0) return false;
    const int w = snprintf(out, cap, "%s.%d", path, n);
    return w > 0 && (size_t)w < cap;
}

void cdk_shell_notify(cdk_shell_t *sh, const char *s)
{
    if (!sh->active || s == NULL) {
        return;
    }
    cdk_shell_hide_line(sh);
    emit(sh, s, strlen(s));
    emit(sh, "\r\n", 2);
    cdk_shell_show_line(sh);
}

void cdk_shell_hide_line(cdk_shell_t *sh)
{
    if (!sh->active) {
        return;
    }
    /* 折り返した行なら全段を消す（上の段が知らせの上に残る）。 */
    erase_drawn(sh);
}

void cdk_shell_show_line(cdk_shell_t *sh)
{
    if (!sh->active) {
        return;
    }
    /* 外から書かれたぶんカーソルは動いている。前の形はもう当てにならない
     * （hide で消してあるので、今いる段から新しく描けばよい）。 */
    sh->drawn_cur_row = 0;
    sh->drawn_end_row = 0;
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

        leave_drawn(sh);
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
        /* カーソルが折り返しの途中の段にいても、出力は行の下から始める。 */
        leave_drawn(sh);
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
            leave_drawn(sh);
            cdk_shell_write(sh, "^C\r\n");
            clear_line(sh);
            sh->hist_pos = -1;
            redraw(sh);
            return;
        case 'l': /* 画面消去 */
            cdk_shell_write(sh, "\033[2J\033[H");
            sh->drawn_cur_row = 0; /* 前の行はもう画面に無い */
            sh->drawn_end_row = 0;
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
