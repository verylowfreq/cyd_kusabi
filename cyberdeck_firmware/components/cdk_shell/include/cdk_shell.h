/* cdk_shell — 行編集つきのコマンドラインシェル
 *
 * ESP-IDF に依存しない純 C。ホスト側で全部テストする。
 *
 * 入力は cdk_key_t（キーイベント）で受ける。エスケープ列は
 * cdk_keys_decode が解いてくれるので、ここは一切見ない。
 * USB キーボード（M7）は最初からキーイベントで来るので、この境界がそのまま使える。
 *
 * 出力は端末へのバイト列。呼び出し側が cdk_term なり SSH なりへ流す。
 * ESP-IDF の console コンポーネントは端末を直接握るので使わない。
 */
#ifndef CDK_SHELL_H
#define CDK_SHELL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cdk_keys.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CDK_SHELL_LINE_MAX    256
#define CDK_SHELL_HISTORY_MAX 16
#define CDK_SHELL_ARGS_MAX    12

typedef void (*cdk_shell_out_fn)(void *user, const char *data, size_t len);

/* 戻り値は終了コード。0 が成功。 */
typedef int (*cdk_shell_cmd_fn)(int argc, char **argv, void *user);

/* 伏せ字入力の答え。**やめたときは NULL**（空行もやめた扱い）。
 * answer は呼び出しの間だけ有効で、戻ると消される。要るなら写すこと。 */
typedef void (*cdk_shell_ask_fn)(void *user, const char *answer);

typedef struct {
    const char      *name;
    const char      *usage; /* 1 行の説明。help が並べる */
    cdk_shell_cmd_fn fn;
} cdk_shell_cmd_t;

typedef struct {
    char line[CDK_SHELL_LINE_MAX];
    int  len;
    int  cursor;

    char hist[CDK_SHELL_HISTORY_MAX][CDK_SHELL_LINE_MAX];
    int  hist_count; /* 貯まっている数 */
    int  hist_pos;   /* -1 = 編集中。0 が最新 */
    char stash[CDK_SHELL_LINE_MAX]; /* 履歴を辿る前の編集内容 */

    const cdk_shell_cmd_t *cmds;
    size_t                 ncmds;
    void                  *cmd_user;

    cdk_shell_out_fn out;
    void            *out_user;

    const char *prompt;
    bool        active; /* false の間はキーを無視する（SSH に入っている等） */
    int         last_status;

    /* 画面の桁数。0 = 知らない（1 段に収まる前提で CR だけで戻る）。 */
    int cols;
    /* 前に描いた行の形（プロンプトの先頭の段から数えて何段目か）。
     * **折り返した行の頭へ戻るのに要る。** CR だけでは今の段の頭にしか
     * 戻れず、上の段を残したまま描き直してしまう。 */
    int drawn_cur_row; /* カーソルのいる段 */
    int drawn_end_row; /* 最後の段 */

    /* ---- 伏せ字入力（cdk_shell_ask）----
     *
     * 行編集の器はコマンド行と同じものを使い回す。**2 つ持たない。**
     * 分けると、伏せ字の側だけ UTF-8 の削除や桁数の数え方を間違える。 */
    bool             asking;
    const char      *ask_saved_prompt; /* 終わったら prompt をこれに戻す */
    int              ask_max;          /* 受け取る側の入れ物に入るバイト数 */
    bool             ask_over;         /* 入りきらない字が来た。切り詰めない */
    cdk_shell_ask_fn ask_cb;
    void            *ask_user;
} cdk_shell_t;

void cdk_shell_init(cdk_shell_t *sh, cdk_shell_out_fn out, void *out_user);

void cdk_shell_set_commands(cdk_shell_t *sh, const cdk_shell_cmd_t *cmds, size_t n,
                            void *cmd_user);

/* プロンプトを出して入力を受け付け始める。 */
/* 出力先の端末の桁数を教える。**教えると、桁を超えて折り返した行も
 * 正しく描き直せる。** 教えないと 1 段に収まる前提で描くので、桁を超えた
 * 行は 1 字打つたびに上の段が残っていく。0 以下は「知らない」。 */
void cdk_shell_set_cols(cdk_shell_t *sh, int cols);

void cdk_shell_begin(cdk_shell_t *sh);

/* シェルを止める（SSH に切り替えるときなど）。キーを無視するようになる。 */
void cdk_shell_end(cdk_shell_t *sh);

bool cdk_shell_active(const cdk_shell_t *sh);

/* パスワードを 1 行聞く。**画面には `*` しか出さない。**
 *
 * コマンドの中から呼んでよい（`ssh` が認証に落ちてから呼ぶ）。呼ぶと
 * コマンド行の代わりに prompt が出て、Enter が来るまで cb は呼ばれない。
 * cb は cdk_shell_key の中から呼ばれるので、**そこで SSH を張ってよい**
 * （張れたら cdk_shell_end されるだけ）。
 *
 * prompt は呼び側が生かしておくこと（写さない。**プロンプトの寿命は
 * 呼び側のもの**、という点は sh->prompt と同じ）。
 *
 * maxlen は受け取る側の入れ物に入るバイト数。**超えたら切り詰めずに
 * やめる**（cb には NULL が行く）。切り詰めた合言葉を送っても通らないし、
 * 「打ったのに違うと言われる」の原因が分からなくなる。
 *
 * 打った字は履歴に積まない。Ctrl-C / Esc / 空の Enter はやめる。 */
void cdk_shell_ask(cdk_shell_t *sh, const char *prompt, int maxlen,
                   cdk_shell_ask_fn cb, void *user);

/* 伏せ字入力の途中か。**日本語入力を素通しさせないために要る**
 * （かな入力のままパスワードを打つと変換されてしまう）。 */
bool cdk_shell_asking(const cdk_shell_t *sh);

/* キーを 1 つ食わせる。 */
void cdk_shell_key(cdk_shell_t *sh, const cdk_key_t *key);

/* コマンドから出力するときに使う。 */
void cdk_shell_write(cdk_shell_t *sh, const char *s);
void cdk_shell_printf(cdk_shell_t *sh, const char *fmt, ...);

/* 入力途中の行に割り込んで 1 行知らせる。**打ちかけの文字を消さない。**
 *
 * WiFi の接続結果のように、コマンドと無関係に後から届く知らせのため。
 * 素の printf で割り込むと、プロンプトと入力中の行の途中に文字が挟まって
 * 何を打っていたか分からなくなる。行頭へ戻して消し、知らせを出してから
 * プロンプトと行を描き直す。
 *
 * s は改行を含めないこと（末尾に CRLF を足す）。
 * シェルが止まっている（active でない）ときは何もしない。 */
void cdk_shell_notify(cdk_shell_t *sh, const char *s);

/* ---- テストしやすいように公開している内部処理 ---- */

/* line を argv に分割する。引用符 " と ' を解釈し、line を書き換える。
 * 返り値は argc。argv は max 個まで。 */
int cdk_shell_split(char *line, char **argv, int max);

/* `user@host` を分ける。`ssh alice@example.com` を受けるため。
 *
 * **区切りは最後の `@`。** Linux の ssh と同じ規則。ユーザー名に `@` は
 * 入らないが、それを言い訳に最初の `@` で切ると `a@b@c` の扱いが
 * あちらと変わる。
 *
 * どちらかが空、`@` が無い、どちらかが長すぎる場合は **false を返して
 * out には何も書かない**。半端に埋めると、呼び側が「ユーザーだけ取れた」と
 * 誤解して知らない相手へ繋ぎに行く。 */
bool cdk_shell_split_userhost(const char *s, char *user, size_t ucap, char *host,
                              size_t hcap);

/* ---- scp の引数 ---- */

/* `cdk_shell_scp_parse` の結果。OK 以外は out を触らない。 */
typedef enum {
    CDK_SCP_OK = 0,
    CDK_SCP_E_BOTH_LOCAL,  /* `:` がどちらにも無い */
    CDK_SCP_E_BOTH_REMOTE, /* `:` が両方にある */
    CDK_SCP_E_NO_USER,     /* host:path の形だが `@` が無い */
    CDK_SCP_E_USERHOST,    /* user@host として読めない */
    CDK_SCP_E_NO_REMOTE,   /* 受信元が `user@host:` で空。何を取るか決まらない */
    CDK_SCP_E_NO_LOCAL,    /* 手元側が空 */
    CDK_SCP_E_TOOLONG      /* どれかが入らない。切り詰めない */
} cdk_scp_parse_t;

typedef struct {
    char user[48];
    char host[80];
    char remote[192];
    char local[192];
    /* remote が空。送信のときだけ許して、手元のファイル名を使う。 */
    bool remote_is_empty;
    bool upload; /* true = 手元 → 向こう */
} cdk_scp_args_t;

/* `scp` の引数 2 つを分解する。
 *
 * **どちらが向こう側かは「最初の `/` より前に `:` があるか」で決める。**
 * Linux の scp と同じ規則。こうすると `/sdcard/a:b` は手元のまま扱えて、
 * `box:/tmp/a` は向こう側になる。
 *
 * **区切りは最初の `:`。** それより後ろは全部向こうのパスで、`:` を
 * 含んでよい（Linux も同じ）。`user@host` の分解は
 * `cdk_shell_split_userhost` に任せる。**規則を 2 か所に持たない。**
 *
 * 向こう側の形をしているのに `@` が無いものは **`E_NO_USER` で断る。**
 * この機械に「手元のユーザー名」は無いので補えないし、黙って設定の相手へ
 * 繋ぐと打ち間違えた宛先へファイルを送ることになる。
 *
 * **OK 以外のとき out には何も書かない。** 半端に埋めると、呼び側が
 * 「宛先は取れた」と誤解して別の相手・別のファイルを触る。 */
cdk_scp_parse_t cdk_shell_scp_parse(const char *a, const char *b,
                                    cdk_scp_args_t *out);

/* ---- uart の引数 ---- */

typedef struct {
    int  baud;
    bool flow; /* RTS/CTS のハードウェアフロー制御 */
} cdk_uart_args_t;

/* `uart [baud] [--flow|-f|--noflow]` を読む。
 *
 * 既定は 115200 8N1・フロー制御なし。速度は位置引数で 1 つだけ、
 * フロー制御は旗で指定する。順番はどちらが先でもよい。
 *
 * **ピンは引数で変えられない。**外に出せる UART は基板の D-sub / Grove に
 * 配線された 1 本きりなので、番号を選ばせても間違える余地が増えるだけ。
 *
 * **false のとき out には何も書かない。** 半端に埋めると、呼び側が
 * 速度だけ取れたと誤解して違う速度で開く。`why` には人に見せる理由が入る
 * （`why` は NULL でもよい）。 */
bool cdk_shell_uart_parse(int argc, char **argv, cdk_uart_args_t *out,
                          const char **why);

/* ---- wget の引数 ---- */

typedef enum {
    CDK_WGET_OK = 0,
    CDK_WGET_E_SCHEME,  /* http:// / https:// で始まっていない */
    CDK_WGET_E_NO_HOST, /* `http://` のあとに相手が無い */
    CDK_WGET_E_BADCHAR, /* 空白や制御文字を含む */
    CDK_WGET_E_TOOLONG  /* 入らない。切り詰めない */
} cdk_wget_parse_t;

/* URL の上限。`esp_http_client` に渡すのはこれ。 */
#define CDK_WGET_URL_MAX 384

typedef struct {
    char url[CDK_WGET_URL_MAX];
    bool https;
} cdk_wget_args_t;

/* `wget` の URL を確かめる。**http と https だけ**（大文字小文字は問わない）。
 *
 * **OK 以外のとき out には何も書かない**（scp_parse と同じ約束）。 */
cdk_wget_parse_t cdk_shell_wget_parse(const char *url, cdk_wget_args_t *out);

/* URL から保存名（ディレクトリを含まない 1 要素）を作る。
 *
 *  - パスの最後の `/` より後ろ、`?` `#` より前。`%XX` は復号する
 *  - 名前が無い（`http://host/` など）ときは `index.html`（wget と同じ）
 *  - FAT で使えない `\ / : * ? " < > |`、制御文字、UTF-8 として壊れた
 *    バイトは `_` にする。**FATFS は UTF-8 の API なので、壊れた列を
 *    渡すと開けない**
 *  - 先頭の空白と末尾の `.` / 空白は削る（FATFS が黙って削るので、
 *    削る前の名前で「既にあるか」を見ると食い違う）
 *  - 長ければ **UTF-8 の文字境界で**、拡張子を残して切る。上限は
 *    `cap - 1` と 255 バイトの小さいほう
 *
 * cap が小さすぎる（`index.html` が入らない）ときだけ false。 */
bool cdk_shell_wget_name(const char *url, char *out, size_t cap);

/* 同名があったときの逃がし先。`a.txt` + 1 → `a.txt.1`（wget と同じ）。
 * 入らなければ false。 */
bool cdk_shell_wget_numbered(const char *path, int n, char *out, size_t cap);

/* 画面上で何桁ぶんになるかを数える。
 * エスケープ列は 0 桁、全角は 2 桁。色付きプロンプトの幅計算に要る。 */
int cdk_shell_visible_width(const char *s, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* CDK_SHELL_H */
