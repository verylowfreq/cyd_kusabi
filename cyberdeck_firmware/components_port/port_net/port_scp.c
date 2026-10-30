/* port_scp — SCP でファイルを 1 本やり取りする
 *
 * ## 作り
 *
 * 接続と認証は `cdk_ssh_session_open()` に任せて**ブロッキングで通す**
 * （実測約 1.9 秒。既存の `ssh` と同じ扱い。状態機械を書くほどの回数では
 * ないという §8.2 の判断をそのまま使う）。
 *
 * **中身の転送はノンブロッキングで、メインループが刻む。** そうすると
 * 画面もキーも `cdk_link_poll` も生きたままなので、進捗が出せて Ctrl-C が
 * 届き、転送中でも `cdk.py shot` が撮れる。専用タスクは作らない
 * （1 周で動かす量をこちらが決められるので要らない。`wasm run` が別タスク
 * なのは、ゲストのコードが何秒回るか分からないから）。
 *
 * ## SFTP ではなく SCP
 *
 * どちらも上流のビルドに入っているが、SFTP は 1 ファイルあたり 4KB の
 * ハンドルと 256KB のパケット上限を持ち歩く。1 本ずつ運ぶだけなら
 * SCP のほうが軽い。**代わりに相手側に `scp` の実行ファイルが要る。**
 *
 * ## 相手の scp はこちらで起こす（libssh2 の scp_send / scp_recv は使わない）
 *
 * libssh2 はパスを必ず `'...'` で括って `scp -t` / `scp -f` を走らせる。
 * **Windows の sshd は cmd.exe で走らせるので、引用符がそのまま scp.exe に
 * 渡って `'C:/x': No such file or directory` で断られる**（2026-10-30、
 * Windows の OpenSSH 9.5 で踏んだ）。libssh2 は無改造の方針なので、
 * チャネルを開いて exec し、最初のやり取り（準備完了・`C` 行・返事）は
 * ここで書く。行の作り方と `C` 行の読み方は cdk_shell（ホストでテスト）。
 *
 * ついでに、相手が断った理由（「No such file or directory」など）を
 * そのまま出せるようになった。libssh2 は読んで捨てて「failed to send file」
 * だけを残していた。
 *
 * ## 踏みやすい所（実装がここに集まっている理由）
 *
 *  - **NULL が返っても失敗とは限らない。** ノンブロッキングだと
 *    `channel_open_session` は EAGAIN のとき NULL を返す。
 *    `libssh2_session_last_errno()` を見ないと、繋がる相手を
 *    「開けません」と誤診する。
 *  - **送信は終端の手続きが要る。** 中身を送り切ったら NUL を 1 個送り、
 *    相手の返事を 1 個読み、`send_eof` → `wait_eof` → `wait_closed`。
 *    省くと相手側が確定せず、**中身が切れたファイルや 0 バイトが届く。**
 *  - **受信は C 行の大きさちょうどで読み終える。** 1 バイトでも余分に
 *    読むと、枠組みの終端待ちで固まる。そのあと終端の NUL を 1 個だけ読み、
 *    続きが来ないことを時計つきで確かめる（ノンブロッキングなので待っても
 *    固まらない）。
 *  - **`cdk_sd_lock()` を転送の間ずっと握らない。** CoreS3 は LCD の DC と
 *    SD の MISO が同じ G35 なので、握っている間は画面が出せない。
 *    1 回の read/write ごとに取る。
 */
#include "cdk_net.h"

#include "cdk_shell.h"
#include "cdk_storage.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "libssh2.h"
#include "port_ssh_priv.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "cdk_scp";

/* 転送用の領域。**起動時に 1 回だけ確保する**（DESIGN.md §14）。
 * static にすると 4KB を内部 DRAM から恒久的に食う。この機械で
 * いちばん足りない資源なので PSRAM から取る。 */
static uint8_t *s_buf;

static bool            s_busy;
static bool            s_upload;
static bool            s_abort;
static cdk_scp_phase_t s_phase;
static LIBSSH2_CHANNEL *s_ch;
static FILE           *s_fp;

static uint32_t s_done;
static uint32_t s_total;
static uint32_t s_last_progress_ms; /* 最後に進んだ時刻。行き詰まりを見る */

/* 相手のパス（括る前）。括るのは cdk_shell_scp_command で s_cmd を作るとき。 */
static char s_remote[200];

/* 相手で走らせる行。**EAGAIN のあと同じ引数で呼び直す**ので static に持つ。
 * `'` を `'\''` にすると最悪 4 倍になるので、パス 200 字ぶんより大きく取る。 */
static char s_cmd[512];

/* 最初のやり取りはどこまで進んだか。EAGAIN で戻るので覚えておく。
 *   送る: チャネル → exec → 準備完了 \0 を待つ → C 行を送る → 返事を待つ
 *   取る: チャネル → exec → \0 を送る → C 行を読む → \0 で答える */
typedef enum {
    OPEN_CHANNEL = 0,
    OPEN_EXEC,
    OPEN_WAIT_READY, /* 送る: 相手の準備完了 */
    OPEN_SEND_HEADER,
    OPEN_WAIT_ACK, /* 送る: C 行への返事 */
    OPEN_SEND_GO,  /* 取る: \0 を送る（T 行のあとも） */
    OPEN_READ_HEADER,
    OPEN_SEND_ACK, /* 取る: C 行に \0 で答える */
    OPEN_READ_WHY  /* 相手が 0x01 / 0x02 で断った。続く理由の行を読む */
} open_t;
static open_t s_open;

/* 相手から来る 1 行の読みかけ（C 行・断りの理由）。送る C 行もここで作る。 */
static char   s_line[256];
static size_t s_line_len;
static size_t s_line_at; /* 送る C 行のうち何バイト送ったか */

/* 手元のパス。受信は書き上がるまで .tmp に置く（下の rename の理由）。 */
static char s_final[200];
static char s_tmp[208];

/* 送信の終端手続きはどこまで進んだか。EAGAIN で戻るので覚えておく。 */
typedef enum { FIN_NUL = 0, FIN_ACK, FIN_EOF, FIN_WAIT_EOF, FIN_WAIT_CLOSED } fin_t;
static fin_t s_fin;

/* 受信の仕上げはどこまで進んだか（終端の確認 → 続きが無いかの確認 → rename）。 */
typedef enum { RFIN_STATUS = 0, RFIN_TAIL, RFIN_DONE } rfin_t;
static rfin_t   s_rfin;
static uint32_t s_tail_since;

/* 1 チャンクの読みかけ。送信は fread した分を送り切るまで持つ。 */
static uint32_t s_pend;    /* s_buf に入っているバイト数 */
static uint32_t s_pend_at; /* そのうち何バイト送ったか */

static char s_why[128];

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* もう一度呼べば進むかもしれない、を 1 つの述語にする。
 *
 * ノンブロッキングでは EAGAIN が来る。セッション期限は
 * `cdk_ssh_shell_open` の側で 0 に戻しているが、こちらは自分で 0 のまま
 * 使うので TIMEOUT は本来出ない。**それでも見ておく**（将来 `set_timeout`
 * を入れた誰かが、ここを致命的エラーと誤判定しないように）。 */
static bool again(int rc)
{
    return rc == LIBSSH2_ERROR_EAGAIN || rc == LIBSSH2_ERROR_TIMEOUT;
}

/* libssh2 が持っている理由をそのまま借りる。
 * 「No such file or directory」のような**相手の言葉**がいちばん役に立つ。 */
static void why_from_libssh2(const char *fallback)
{
    LIBSSH2_SESSION *s = cdk_ssh_session_raw();
    char            *msg = NULL;
    if (s != NULL && libssh2_session_last_error(s, &msg, NULL, 0) != 0 &&
        msg != NULL && msg[0] != '\0') {
        snprintf(s_why, sizeof(s_why), "%s", msg);
        return;
    }
    snprintf(s_why, sizeof(s_why), "%s", fallback);
}

static void close_local(void)
{
    if (s_fp != NULL) {
        cdk_sd_lock();
        fclose(s_fp);
        cdk_sd_unlock();
        s_fp = NULL;
    }
}

/* 後片付け。**受信の書きかけは消す。**
 *
 * `edit` の保存と同じ作法。途中で切れたものを最終名で残すと、開いた人が
 * 「壊れたファイル」を掴む。`cdk.py push` は消していないが、あちらに
 * 倣わない。 */
static void cleanup(bool ok)
{
    close_local();
    if (s_ch != NULL) {
        /* free も EAGAIN を返す。ここは戻り値を捨てて構わない
         * （どうせセッションごと閉じる）。 */
        while (libssh2_channel_free(s_ch) == LIBSSH2_ERROR_EAGAIN) {
        }
        s_ch = NULL;
    }
    if (!s_upload && !ok && s_tmp[0] != '\0') {
        cdk_sd_lock();
        remove(s_tmp);
        cdk_sd_unlock();
    }
    /* **`cdk_ssh_close()` ではなく session_close。** こちらは `s_open` を
     * 立てていないので切断理由を触らないが、名前で意図を示しておく。 */
    cdk_ssh_session_close();
}

esp_err_t cdk_scp_init(void)
{
    if (s_buf != NULL) {
        return ESP_OK;
    }
    s_buf = heap_caps_malloc(CDK_SCP_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_buf == NULL) {
        ESP_LOGE(TAG, "転送用の領域を確保できません");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool cdk_scp_busy(void)
{
    return s_busy;
}

void cdk_scp_abort(void)
{
    if (s_busy) {
        s_abort = true;
    }
}

bool cdk_scp_status(cdk_scp_status_t *out)
{
    if (!s_busy || out == NULL) {
        return false;
    }
    out->phase = s_phase;
    out->upload = s_upload;
    out->done = s_done;
    out->total = s_total;
    return true;
}

esp_err_t cdk_scp_start(bool upload, const char *local, const char *remote,
                        const cdk_ssh_login_t *login, char *why, size_t why_cap)
{
    if (s_buf == NULL) {
        snprintf(why, why_cap, "転送用の領域がありません（reboot）");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_busy) {
        snprintf(why, why_cap, "もう転送中です");
        return ESP_ERR_INVALID_STATE;
    }
    if (local == NULL || remote == NULL || login == NULL) {
        snprintf(why, why_cap, "引数が足りません");
        return ESP_ERR_INVALID_ARG;
    }

    s_upload = upload;
    s_abort = false;
    s_done = 0;
    s_total = 0;
    s_pend = 0;
    s_pend_at = 0;
    s_fin = FIN_NUL;
    s_rfin = RFIN_STATUS;
    s_ch = NULL;
    s_fp = NULL;
    s_why[0] = '\0';
    s_tmp[0] = '\0';
    snprintf(s_final, sizeof(s_final), "%s", local);
    snprintf(s_remote, sizeof(s_remote), "%s", remote);

    /* 送信は先に大きさが要る（C 行に書く）。
     * **ここで見ておけば、繋ぐ前に「ありません」と言える。** */
    struct stat st;
    if (upload) {
        cdk_sd_lock();
        const int rc = stat(local, &st);
        cdk_sd_unlock();
        if (rc != 0) {
            snprintf(why, why_cap, "%s がありません", local);
            return ESP_ERR_NOT_FOUND;
        }
        if (S_ISDIR(st.st_mode)) {
            snprintf(why, why_cap, "ディレクトリは送れません（1 本だけ）");
            return ESP_ERR_INVALID_ARG;
        }
        /* **符号なしで読み直す。** off_t は 32 ビットの long なので、FAT の
         * 2GB〜4GB のファイルは負で返る（FAT の上限は 4GB なので、
         * 32 ビットに収まることは保証されている）。 */
        s_total = (uint32_t)st.st_size;
    }

    /* ここはブロッキング。約 1.9 秒。 */
    const esp_err_t err = cdk_ssh_session_open(login);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NOT_ALLOWED) {
            /* **パスワードを送ったのに `ssh key で確認` と出すと嘘になる**
             * （`ssh` と同じ言い分け。空はパスワード無し）。 */
            const bool sent_pass =
                (login->password != NULL && login->password[0] != '\0');
            snprintf(why, why_cap, "%s",
                     sent_pass ? "認証に失敗（鍵もパスワードも通りません）"
                               : "認証に失敗 (ssh key で確認)");
        } else if (err == ESP_ERR_NOT_FOUND) {
            snprintf(why, why_cap, "%s に届きません", login->host);
        } else if (err == ESP_ERR_INVALID_STATE) {
            snprintf(why, why_cap, "ホスト鍵が記録と一致しません (hostkey forget)");
        } else if (err == ESP_ERR_NOT_FINISHED) {
            snprintf(why, why_cap, "SSH を先に閉じてください (ssh close)");
        } else {
            snprintf(why, why_cap, "接続できません %s", esp_err_to_name(err));
        }
        cdk_ssh_session_close();
        return err;
    }

    /* 相手で走らせる行を作る。**括り方は相手のシェル次第**なので、繋いで
     * 相手の名乗り（`SSH-2.0-OpenSSH_for_Windows_9.5`）を見てから決める。 */
    const char *banner = libssh2_session_banner_get(cdk_ssh_session_raw());
    const bool  windows = (banner != NULL && strstr(banner, "Windows") != NULL);
    if (!cdk_shell_scp_command(upload, remote, windows, s_cmd, sizeof(s_cmd))) {
        snprintf(why, why_cap, "%s",
                 (windows && strchr(remote, '"') != NULL)
                     ? "Windows の相手には \" を含むパスを渡せません"
                     : "パスが長すぎます");
        cdk_ssh_session_close();
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "相手 %s → %s", (banner != NULL) ? banner : "(名乗りなし)", s_cmd);

    /* ここから先はメインループが刻むのでノンブロッキングにする。
     * **セッションとソケットの両方**（片方だけだと libssh2 の recv() が
     * カーネルの中で待って、メインループが止まる。実機で踏んだ）。
     * 行き詰まりは時計でこちらが見るので、期限は 0 のままにする。 */
    cdk_ssh_go_nonblocking();

    if (!upload) {
        snprintf(s_tmp, sizeof(s_tmp), "%s.tmp", local);
    }

    s_phase = CDK_SCP_PHASE_OPEN;
    s_open = OPEN_CHANNEL;
    s_line_len = 0;
    s_line_at = 0;
    s_last_progress_ms = now_ms();
    s_busy = true;
    ESP_LOGI(TAG, "%s %s %s %s", login->user, upload ? "へ送る" : "から取る",
             remote, local);
    return ESP_OK;
}

/* 失敗で終える。理由は s_why に入っているもの（無ければ fallback）。 */
static void fail(const char *fallback)
{
    if (s_why[0] == '\0') {
        snprintf(s_why, sizeof(s_why), "%s", fallback);
    }
    cleanup(false);
    s_phase = CDK_SCP_PHASE_DONE;
}

/* 相手の 1 行を s_line に溜める（改行は入れない）。
 * 1 = 揃った、0 = まだ（EAGAIN）、-1 = 切れた・長すぎる。 */
static int read_line(void)
{
    while (s_line_len + 1u < sizeof(s_line)) {
        char      c = 0;
        const int r = (int)libssh2_channel_read(s_ch, &c, 1);
        if (again(r)) {
            return 0;
        }
        if (r < 0) {
            return -1;
        }
        if (r == 0) {
            return libssh2_channel_eof(s_ch) ? -1 : 0;
        }
        if (c == '\n') {
            s_line[s_line_len] = '\0';
            return 1;
        }
        s_line[s_line_len++] = c;
    }
    return -1;
}

/* 相手の返事 1 バイト。1 = 良し (\0)、0 = まだ、-1 = 失敗（理由は s_why か、
 * 断られたなら OPEN_READ_WHY へ進めてある）。 */
static int read_ack(void)
{
    char      c = 0;
    const int r = (int)libssh2_channel_read(s_ch, &c, 1);
    if (again(r)) {
        return 0;
    }
    if (r < 0) {
        why_from_libssh2("相手の返事が読めません");
        return -1;
    }
    if (r == 0) {
        if (!libssh2_channel_eof(s_ch)) {
            return 0;
        }
        /* 何も言わずに閉じた。相手に scp が無いとこうなる（シェルが
         * 「command not found」を stderr に出して終わる）。 */
        snprintf(s_why, sizeof(s_why), "相手の scp が応答しません（scp はありますか）");
        return -1;
    }
    if (c != 0) {
        /* **理由は相手が続けて送ってくる。** 状態バイトの次に改行までの
         * メッセージが来るので、それを読んでから終える。 */
        s_line_len = 0;
        s_open = OPEN_READ_WHY;
        return -1;
    }
    return 1;
}

/* 1 行ぶん（s_line の s_line_at から s_line_len まで）を送る。1 = 送り切った。 */
static int write_line(void)
{
    while (s_line_at < s_line_len) {
        const ssize_t w =
            libssh2_channel_write(s_ch, s_line + s_line_at, s_line_len - s_line_at);
        if (w == LIBSSH2_ERROR_EAGAIN) {
            return 0;
        }
        if (w < 0) {
            why_from_libssh2("相手に送れません");
            return -1;
        }
        s_line_at += (size_t)w;
    }
    return 1;
}

/* 手元のファイルを開く。**最初のやり取りが済んでから開く。** FATFS の
 * 空きハンドルは本体に 1 本しかない（4 本は wasm 用に予約済み）ので、
 * 認証中に読む鍵ファイルと重ならないようにする。 */
static bool open_local(void)
{
    cdk_sd_lock();
    s_fp = fopen(s_upload ? s_final : s_tmp, s_upload ? "rb" : "wb");
    cdk_sd_unlock();
    if (s_fp == NULL) {
        /* パスは長いと切る。**メッセージ全体はどのみち画面幅で切られる**
         * （`cdk_editcmds.c` の保存メッセージと同じ扱い）。 */
        snprintf(s_why, sizeof(s_why), "%.80s を開けません",
                 s_upload ? s_final : s_tmp);
        return false;
    }
    return true;
}

/* ---- OPEN: 相手の scp を起こして、最初のやり取りを済ませる ---- */
static void pump_open(void)
{
    LIBSSH2_SESSION *sess = cdk_ssh_session_raw();
    int              rc = 0;

    switch (s_open) {
    case OPEN_CHANNEL:
        s_ch = libssh2_channel_open_session(sess);
        if (s_ch == NULL) {
            if (again(libssh2_session_last_errno(sess))) {
                return; /* **NULL でも失敗とは限らない。** 次の刻みで続く */
            }
            why_from_libssh2("チャネルを開けません");
            fail("開けません");
            return;
        }
        s_open = OPEN_EXEC;
        s_last_progress_ms = now_ms();
        return;

    case OPEN_EXEC:
        /* EAGAIN のときは同じ引数で呼び直す（s_cmd は static）。 */
        rc = libssh2_channel_exec(s_ch, s_cmd);
        if (again(rc)) {
            return;
        }
        if (rc != 0) {
            why_from_libssh2("相手の scp を起こせません");
            fail("開けません");
            return;
        }
        s_open = s_upload ? OPEN_WAIT_READY : OPEN_SEND_GO;
        s_last_progress_ms = now_ms();
        return;

    case OPEN_WAIT_READY:
    case OPEN_WAIT_ACK:
        rc = read_ack();
        if (rc == 0) {
            return;
        }
        if (rc < 0) {
            if (s_open != OPEN_READ_WHY) {
                fail("断られました");
            }
            return;
        }
        s_last_progress_ms = now_ms();
        if (s_open == OPEN_WAIT_READY) {
            /* C 行。名前は**手元の**ファイル名（OpenSSH の scp と同じ）。
             * 相手は宛先がディレクトリならその下にこの名前で置き、ファイル
             * ならその名前で置く。mode は 0644。**時刻は送らない**（-p 無し。
             * この機械に信頼できる時計が無い）。 */
            const char *slash = strrchr(s_final, '/');
            const char *base = (slash != NULL) ? slash + 1 : s_final;
            const int   n = snprintf(s_line, sizeof(s_line), "C0644 %lu %s\n",
                                     (unsigned long)s_total, base);
            if (n <= 0 || (size_t)n >= sizeof(s_line)) {
                snprintf(s_why, sizeof(s_why), "名前が長すぎます");
                fail("名前が長すぎます");
                return;
            }
            s_line_len = (size_t)n;
            s_line_at = 0;
            s_open = OPEN_SEND_HEADER;
            return;
        }
        /* C 行が受け入れられた。中身へ */
        if (!open_local()) {
            fail("開けません");
            return;
        }
        s_phase = CDK_SCP_PHASE_XFER;
        return;

    case OPEN_SEND_HEADER:
        rc = write_line();
        if (rc == 0) {
            return;
        }
        if (rc < 0) {
            fail("送れません");
            return;
        }
        s_open = OPEN_WAIT_ACK;
        s_last_progress_ms = now_ms();
        return;

    case OPEN_SEND_GO:
        /* 「送ってよい」の \0。T 行に答えるときもここを通る。 */
        rc = (int)libssh2_channel_write(s_ch, "\0", 1);
        if (again(rc)) {
            return;
        }
        if (rc < 0) {
            why_from_libssh2("相手に送れません");
            fail("送れません");
            return;
        }
        s_line_len = 0;
        s_open = OPEN_READ_HEADER;
        s_last_progress_ms = now_ms();
        return;

    case OPEN_READ_HEADER: {
        rc = read_line();
        if (rc == 0) {
            return;
        }
        if (rc < 0) {
            if (s_line_len == 0 && libssh2_channel_eof(s_ch)) {
                snprintf(s_why, sizeof(s_why),
                         "相手の scp が応答しません（scp はありますか）");
            } else {
                why_from_libssh2("相手の返事が読めません");
            }
            fail("開けません");
            return;
        }
        s_last_progress_ms = now_ms();
        uint64_t            size = 0;
        const cdk_scp_hdr_t h = cdk_shell_scp_header(s_line, &size);
        switch (h) {
        case CDK_SCP_HDR_FILE:
            /* **4GB 以上は FAT に置けない**（s_total も 32 ビット）。
             * 大きさは 64 ビットのまま読めているので、ここで断れる。 */
            if (size > UINT32_MAX) {
                snprintf(s_why, sizeof(s_why), "大きすぎます (4GB 超)");
                fail("大きすぎます");
                return;
            }
            s_total = (uint32_t)size;
            if (!open_local()) {
                fail("開けません");
                return;
            }
            s_open = OPEN_SEND_ACK;
            return;
        case CDK_SCP_HDR_TIME:
            s_open = OPEN_SEND_GO; /* 時刻は使わない。\0 で答えて次の行へ */
            return;
        case CDK_SCP_HDR_DIR:
            snprintf(s_why, sizeof(s_why), "ディレクトリは取れません（1 本だけ）");
            fail("ディレクトリです");
            return;
        case CDK_SCP_HDR_ERROR:
            /* 相手の言葉をそのまま。長ければ間を詰める（理由は最後に来る）。 */
            cdk_shell_elide_middle(s_line + 1, 24, s_why, sizeof(s_why));
            fail("断られました");
            return;
        default:
            snprintf(s_why, sizeof(s_why), "相手の返事が読めません: %.60s", s_line);
            fail("読めません");
            return;
        }
    }

    case OPEN_SEND_ACK:
        rc = (int)libssh2_channel_write(s_ch, "\0", 1);
        if (again(rc)) {
            return;
        }
        if (rc < 0) {
            why_from_libssh2("相手に送れません");
            fail("送れません");
            return;
        }
        s_phase = CDK_SCP_PHASE_XFER;
        s_last_progress_ms = now_ms();
        return;

    case OPEN_READ_WHY:
    default:
        rc = read_line();
        if (rc == 0) {
            return;
        }
        if (rc > 0 && s_line_len > 0) {
            /* 相手の言葉をそのまま出す（「No such file or directory」など）。
             * 長ければ間を詰める（理由は最後に来る）。 */
            cdk_shell_elide_middle(s_line, 24, s_why, sizeof(s_why));
        } else {
            snprintf(s_why, sizeof(s_why), "相手が断りました");
        }
        fail("断られました");
        return;
    }
}

/* ---- XFER: 中身を動かす ---- */
static void pump_xfer(void)
{
    if (s_upload) {
        if (s_pend == s_pend_at) {
            /* 次のひと山を読む */
            if (s_done >= s_total) {
                s_phase = CDK_SCP_PHASE_FINISH;
                s_fin = FIN_NUL;
                return;
            }
            uint32_t want = CDK_SCP_CHUNK;
            if (s_total - s_done < want) {
                want = s_total - s_done;
            }
            cdk_sd_lock();
            const size_t n = fread(s_buf, 1, want, s_fp);
            cdk_sd_unlock();
            if (n == 0) {
                /* 申告した長さより短い。**途中で止めるしかない**
                 * （相手は s_total バイト待っている）。 */
                fail("読めるバイト数が申告と合いません");
                return;
            }
            s_pend = (uint32_t)n;
            s_pend_at = 0;
        }
        const ssize_t w = libssh2_channel_write(
            s_ch, (const char *)(s_buf + s_pend_at), s_pend - s_pend_at);
        if (w == LIBSSH2_ERROR_EAGAIN) {
            return;
        }
        if (w < 0) {
            why_from_libssh2("送れません");
            fail("送れません");
            return;
        }
        s_pend_at += (uint32_t)w;
        s_done += (uint32_t)w;
        s_last_progress_ms = now_ms();
        return;
    }

    /* 受信。**`s_total` ちょうどで読み終える。** 余分に読むと、
     * 枠組みの終端待ちで固まる。 */
    if (s_done >= s_total) {
        s_phase = CDK_SCP_PHASE_FINISH;
        return;
    }
    uint32_t want = CDK_SCP_CHUNK;
    if (s_total - s_done < want) {
        want = s_total - s_done;
    }
    const ssize_t r = libssh2_channel_read(s_ch, (char *)s_buf, want);
    if (r == LIBSSH2_ERROR_EAGAIN) {
        return;
    }
    if (r < 0) {
        why_from_libssh2("受け取れません");
        fail("受け取れません");
        return;
    }
    if (r == 0) {
        if (libssh2_channel_eof(s_ch)) {
            fail("途中で切れました（相手が閉じました）");
        }
        return;
    }
    cdk_sd_lock();
    const size_t n = fwrite(s_buf, 1, (size_t)r, s_fp);
    cdk_sd_unlock();
    if (n != (size_t)r) {
        fail("書き込みに失敗しました（容量は足りていますか）");
        return;
    }
    s_done += (uint32_t)r;
    s_last_progress_ms = now_ms();
}

/* ---- FINISH ---- */
static void pump_finish(void)
{
    if (!s_upload && s_rfin == RFIN_STATUS) {
        /* **申告どおり読み終えたら、次は scp の終端（NUL 1 個）のはず。**
         * 違うものが来たら、申告より中身が長い。
         *
         * （libssh2 の scp_recv2 を使っていた頃は、申告が 32 ビットに切られて
         * 4GB 超がここで初めて分かった。いまは C 行を 64 ビットで読んで先に
         * 断るので、ここは相手の枠組みの確認として残している。） */
        char      c = 0;
        const int r = (int)libssh2_channel_read(s_ch, &c, 1);
        if (again(r)) {
            return;
        }
        if (r == 1 && c != 0) {
            snprintf(s_why, sizeof(s_why), "相手が申告より長く送ってきました");
            fail("長さが合いません");
            return;
        }
        /* NUL、または相手が閉じた（終端を送らない実装もある）。 */
        s_rfin = RFIN_TAIL;
        s_tail_since = now_ms();
        return;
    }
    if (!s_upload && s_rfin == RFIN_TAIL) {
        /* **NUL のあとに何も続かないことを確かめる。** 申告より長い中身が
         * たまたま 0 バイトで続いていると、上の確認をすり抜ける。正しい
         * 転送なら相手の scp は終端を送ってこちらの応答を待つので、
         * ここでは何も来ない。 */
        char      tail[16];
        const int r = (int)libssh2_channel_read(s_ch, tail, sizeof(tail));
        if (r > 0) {
            snprintf(s_why, sizeof(s_why), "相手が申告より長く送ってきました");
            fail("長さが合いません");
            return;
        }
        if (again(r) && now_ms() - s_tail_since < CDK_SCP_TAIL_WAIT_MS) {
            return;
        }
        s_rfin = RFIN_DONE; /* 来なかった / 相手が閉じた。仕上げへ */
    }

    if (!s_upload) {
        /* 書き上がったものだけを最終名にする。
         * **FATFS の rename は上書きしないので先に消す。**
         * ここで落ちると元が無い状態になるが、.tmp は残っている
         * （`edit` の保存と同じ約束）。 */
        close_local();
        cdk_sd_lock();
        remove(s_final);
        const int rc = rename(s_tmp, s_final);
        cdk_sd_unlock();
        if (rc != 0) {
            snprintf(s_why, sizeof(s_why), "%.80s に置けません", s_final);
            fail("置けません");
            return;
        }
        s_tmp[0] = '\0'; /* もう消させない */
        cleanup(true);
        s_phase = CDK_SCP_PHASE_DONE;
        return;
    }

    /* 送信の終端手続き。**どれも省けない。**
     * 相手の `scp` は中身を受け切ったあとに返事を待つので、
     * NUL を送らずに EOF だけ流すと「lost connection」になる。 */
    int rc = 0;
    switch (s_fin) {
    case FIN_NUL:
        rc = (int)libssh2_channel_write(s_ch, "\0", 1);
        if (again(rc)) {
            return;
        }
        if (rc < 0) {
            why_from_libssh2("終端を送れません");
            fail("終端を送れません");
            return;
        }
        s_fin = FIN_ACK;
        return;
    case FIN_ACK: {
        char ack = 0;
        rc = (int)libssh2_channel_read(s_ch, &ack, 1);
        if (again(rc)) {
            return;
        }
        /* 相手が何も返さずに閉じることもある。**そこは通す**
         * （中身は届いていて、返事だけが来ない実装がある）。 */
        if (rc > 0 && ack != 0) {
            /* **理由は相手が続けて送ってくる。** scp は状態バイトの次に
             * 改行までのメッセージを流すので、それを借りる。
             * 「断られました」だけでは何を直せばいいか分からない。 */
            /* 入れ物は s_line を借りる（もう C 行には使わない）。 */
            size_t at = 0;
            while (at + 1u < sizeof(s_line)) {
                char c = 0;
                const int r = (int)libssh2_channel_read(s_ch, &c, 1);
                if (r <= 0 || c == '\n') {
                    break;
                }
                s_line[at++] = c;
            }
            s_line[at] = '\0';
            if (at > 0) {
                /* 「相手が断りました: 」(26 バイト) の残りに、間を詰めて入れる
                 * （Windows はパスのあとに理由を書くので、頭から切ると消える）。 */
                static const char pre[] = "相手が断りました: ";
                memcpy(s_why, pre, sizeof(pre) - 1u);
                cdk_shell_elide_middle(s_line, 16, s_why + sizeof(pre) - 1u,
                                       sizeof(s_why) - (sizeof(pre) - 1u));
            } else {
                snprintf(s_why, sizeof(s_why), "相手が断りました (0x%02x)",
                         (unsigned)(unsigned char)ack);
            }
            fail("断られました");
            return;
        }
        s_fin = FIN_EOF;
        return;
    }
    case FIN_EOF:
        rc = libssh2_channel_send_eof(s_ch);
        if (again(rc)) {
            return;
        }
        s_fin = FIN_WAIT_EOF;
        return;
    case FIN_WAIT_EOF:
        rc = libssh2_channel_wait_eof(s_ch);
        if (again(rc)) {
            return;
        }
        s_fin = FIN_WAIT_CLOSED;
        return;
    case FIN_WAIT_CLOSED:
    default:
        rc = libssh2_channel_wait_closed(s_ch);
        if (again(rc)) {
            return;
        }
        cleanup(true);
        s_phase = CDK_SCP_PHASE_DONE;
        return;
    }
}

void cdk_scp_poll(bool *done, char *report, size_t report_cap)
{
    if (done != NULL) {
        *done = false;
    }
    if (!s_busy) {
        return;
    }

    if (s_phase != CDK_SCP_PHASE_DONE) {
        if (s_abort) {
            /* **送信の途中で止めると、相手に書きかけが残る。**
             * こちらでは消せないので、そう言う。 */
            snprintf(s_why, sizeof(s_why), "%s",
                     s_upload ? "止めました（相手に途中まで残っています）"
                              : "止めました");
            cleanup(false);
            s_phase = CDK_SCP_PHASE_DONE;
        } else if (now_ms() - s_last_progress_ms > CDK_SCP_STALL_MS) {
            /* 相手が黙ると EAGAIN が永久に返る。**誰かが時計を見ないと
             * 終わらない。** */
            fail("応答がありません");
        } else {
            /* **1 周で 8 回まで刻む。** SSH の受信と同じ予算
             * （`main.c` は 8×512B を 1 周で汲む）。
             *
             * 狙いは「メインループの刻み (`cdk_link_poll(5)` の 5ms) が
             * 上限にならないこと」。**ただし実測では効かなかった**:
             * 1 回でも 8 回でも 316KB が 13 秒（約 24KB/s）で、
             * この相手（国外の VPS）では網の往復が支配している。
             * LAN の相手なら効くはずだが、**確かめていない。**
             * 進まなくなった時点で抜けるので、メインループは止めない。 */
            for (int i = 0; i < 8; i++) {
                const uint32_t        before = s_done;
                const cdk_scp_phase_t was = s_phase;
                switch (s_phase) {
                case CDK_SCP_PHASE_OPEN: pump_open(); break;
                case CDK_SCP_PHASE_XFER: pump_xfer(); break;
                case CDK_SCP_PHASE_FINISH: pump_finish(); break;
                default: break;
                }
                if (s_phase != was || s_done == before) {
                    break; /* 段が変わった / 進まなかった。次の周へ */
                }
            }
        }
    }

    if (s_phase != CDK_SCP_PHASE_DONE) {
        return;
    }

    /* 終わった。1 行にまとめて返す。 */
    if (report != NULL && report_cap > 0) {
        if (s_why[0] == '\0') {
            snprintf(report, report_cap,
                     "\r\033[K\033[32mscp   : %luKB %s\033[0m\r\n",
                     (unsigned long)((s_done + 1023u) / 1024u),
                     s_upload ? "送りました" : "受け取りました");
        } else {
            snprintf(report, report_cap, "\r\033[K\033[31mscp   : %s\033[0m\r\n",
                     s_why);
        }
    }
    ESP_LOGI(TAG, "終了 %s %lu/%lu バイト %s", s_upload ? "送信" : "受信",
             (unsigned long)s_done, (unsigned long)s_total,
             (s_why[0] == '\0') ? "OK" : s_why);
    s_busy = false;
    s_phase = CDK_SCP_PHASE_IDLE;
    if (done != NULL) {
        *done = true;
    }
}
