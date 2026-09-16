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
 * ## 踏みやすい所（実装がここに集まっている理由）
 *
 *  - **NULL が返っても失敗とは限らない。** ノンブロッキングだと
 *    `scp_send64` / `scp_recv2` は EAGAIN のとき NULL を返す。
 *    `libssh2_session_last_errno()` を見ないと、繋がる相手を
 *    「開けません」と誤診する。
 *  - **送信は終端の手続きが要る。** 中身を送り切ったら NUL を 1 個送り、
 *    相手の返事を 1 個読み、`send_eof` → `wait_eof` → `wait_closed`。
 *    省くと相手側が確定せず、**中身が切れたファイルや 0 バイトが届く。**
 *  - **受信は `st_size` ちょうどで読み終える。** 1 バイトでも余分に読むと、
 *    枠組みの終端待ちで固まる。そのあと終端の NUL を 1 個だけ読み、続きが
 *    来ないことを時計つきで確かめる（4GB 超で申告が切られていないか。
 *    ノンブロッキングなので待っても固まらない）。
 *  - **`cdk_sd_lock()` を転送の間ずっと握らない。** CoreS3 は LCD の DC と
 *    SD の MISO が同じ G35 なので、握っている間は画面が出せない。
 *    1 回の read/write ごとに取る。
 */
#include "cdk_net.h"

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

/* 相手のパス。**引用しない。** libssh2 が中で shell quote する
 * （`quote_paths` が既定で 1）ので、こちらで括ると二重になる。 */
static char s_remote[200];

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

    /* 送信は先に大きさが要る（`libssh2_scp_send64` の引数）。
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
            snprintf(why, why_cap, "認証に失敗 (ssh key で確認)");
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

    /* ここから先はメインループが刻むのでノンブロッキングにする。
     * **セッションとソケットの両方**（片方だけだと libssh2 の recv() が
     * カーネルの中で待って、メインループが止まる。実機で踏んだ）。
     * 行き詰まりは時計でこちらが見るので、期限は 0 のままにする。 */
    cdk_ssh_go_nonblocking();

    if (!upload) {
        snprintf(s_tmp, sizeof(s_tmp), "%s.tmp", local);
    }

    s_phase = CDK_SCP_PHASE_OPEN;
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

/* ---- OPEN: SCP のチャネルを開く ---- */
static void pump_open(void)
{
    LIBSSH2_SESSION *sess = cdk_ssh_session_raw();

    if (s_upload) {
        /* mtime / atime は 0 にする。**時計を送らない。** 送ると相手側で
         * `-p` 相当の往復が 1 回増えるうえ、この機械に信頼できる時計が無い。 */
        s_ch = libssh2_scp_send64(sess, s_remote, 0644,
                                  (libssh2_int64_t)s_total, 0, 0);
    } else {
        /* **先に 0 で埋める。** 途中で返ってきたときに st_mode で見分ける
         * ため（libssh2 は枠組みの途中でもチャネルを返すことがある）。 */
        libssh2_struct_stat sb;
        memset(&sb, 0, sizeof(sb));
        s_ch = libssh2_scp_recv2(sess, s_remote, &sb);
        if (s_ch != NULL) {
            if (sb.st_mode == 0) {
                /* まだ枠組みの途中。次の刻みでやり直す（内部の状態は
                 * 保たれているので、同じ引数で呼び直せば続きから進む）。 */
                s_ch = NULL;
                return;
            }
            /* 符号なしで読み直す（2GB〜4GB は負で来る）。4GB 超は上が
             * 落ちていて、ここでは分からない。FINISH の終端確認で断る。 */
            s_total = (uint32_t)sb.st_size;
        }
    }

    if (s_ch == NULL) {
        const int rc = libssh2_session_last_errno(sess);
        if (again(rc)) {
            return; /* **NULL でも失敗とは限らない。** 次の刻みで続く */
        }
        why_from_libssh2(s_upload ? "相手に書けません" : "相手のファイルが開けません");
        fail("開けません");
        return;
    }

    /* チャネルが開いてから手元のファイルを開く。**FATFS の空きハンドルは
     * 本体に 1 本しかない**（4 本は wasm 用に予約済み）ので、認証中に
     * 読む鍵ファイルと重ならないようにする。 */
    cdk_sd_lock();
    s_fp = fopen(s_upload ? s_final : s_tmp, s_upload ? "rb" : "wb");
    cdk_sd_unlock();
    if (s_fp == NULL) {
        /* パスは長いと切る。**メッセージ全体はどのみち画面幅で切られる**
         * （`cdk_editcmds.c` の保存メッセージと同じ扱い）。 */
        snprintf(s_why, sizeof(s_why), "%.80s を開けません",
                 s_upload ? s_final : s_tmp);
        fail("開けません");
        return;
    }

    s_phase = CDK_SCP_PHASE_XFER;
    s_last_progress_ms = now_ms();
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
         * 違うものが来たら、申告の大きさが 32 ビットに切られていた
         * ＝相手のファイルは 4GB 超。中身の続きを終端と取り違えている。 */
        char      c = 0;
        const int r = (int)libssh2_channel_read(s_ch, &c, 1);
        if (again(r)) {
            return;
        }
        if (r == 1 && c != 0) {
            snprintf(s_why, sizeof(s_why), "大きすぎます (4GB 超)");
            fail("大きすぎます");
            return;
        }
        /* NUL、または相手が閉じた（終端を送らない実装もある）。 */
        s_rfin = RFIN_TAIL;
        s_tail_since = now_ms();
        return;
    }
    if (!s_upload && s_rfin == RFIN_TAIL) {
        /* **NUL のあとに何も続かないことを確かめる。** 切られた申告が
         * たまたま中身の 0 バイトで終わっていると、上の確認をすり抜ける
         * （4GB 超の疎なファイルなら必ずそうなる）。正しい転送なら相手の
         * scp は終端を送ってこちらの応答を待つので、ここでは何も来ない。 */
        char      tail[16];
        const int r = (int)libssh2_channel_read(s_ch, tail, sizeof(tail));
        if (r > 0) {
            snprintf(s_why, sizeof(s_why), "大きすぎます (4GB 超)");
            fail("大きすぎます");
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
            char   msg[96];
            size_t at = 0;
            while (at + 1u < sizeof(msg)) {
                char c = 0;
                const int r = (int)libssh2_channel_read(s_ch, &c, 1);
                if (r <= 0 || c == '\n') {
                    break;
                }
                msg[at++] = c;
            }
            msg[at] = '\0';
            if (at > 0) {
                snprintf(s_why, sizeof(s_why), "相手が断りました: %.90s", msg);
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
