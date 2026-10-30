/* Cyberdeck ファームウェア
 *
 * M0: ビルド基盤 / ホスト側テスト / LCD 表示 / スクショ転送 / 有限時間のログ取得
 * M1: VT パーサ + 東雲 12px フォント + テキストコンソール
 * M2: WiFi + libssh2 による PTY 付き SSH
 * M3: 行編集つきシェル + GPIO / ADC / I2C
 * M4: microSD (FATFS)
 * M5: SKK 日本語入力
 * M6: テキストエディタ  ← いまここ
 *
 * キーの行き先は 1 か所（route_key）だけで決める。
 * 条件分岐をあちこちに散らすと「このキーは誰が受け取るのか」が
 * 実装の副作用になって追えなくなる。
 */
#include "app.h"

#include "cdk_board.h"
#include "cdk_canvas.h"
#include "cdk_cfg.h"
#include "cdk_display.h"
#include "cdk_font.h"
#include "cdk_keys.h"
#include "cdk_link.h"
#include "cdk_net.h"
#include "cdk_periph.h" /* cdk_uart_* — 外部 UART の端末 */
#include "cdk_proto.h"
#include "cdk_screen.h"
#include "cdk_shell.h"
#include "cdk_skk.h"
#include "cdk_skkdict_src.h"
#include "cdk_retry.h"
#include "cdk_status.h"
#include "cdk_storage.h"
#include "cdk_wasm.h"     /* cdk_wasm_path_resolve — SD の外へ出さない唯一の判定 */
#include "cdk_wasm_run.h"
#include "cdk_usbhid.h"
#include "esp_timer.h"
#include "cdk_term.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "secrets.h"

static const char *TAG = "cyberdeck";

/* tools/cdk.py がこの行を見て「起動完了」と判断する。 */
#define CDK_READY_SENTINEL "#CDK-READY"
#define CDK_VERSION        "KUSABI v1"

/* フォントはフラッシュに焼き込む。SD もファイルシステムも要らない。 */
extern const uint8_t font_start[] asm("_binary_shinonome12_cdkfont_start");
extern const uint8_t font_end[] asm("_binary_shinonome12_cdkfont_end");

/* 起動バナー。host/fixtures/vt/banner.vt と同一のバイト列。
 * ホスト側の cdk_render vt が同じファイルを描くので、
 * 実機のスクリーンショットと 1 バイト違わず比較できる。 */
extern const uint8_t banner_start[] asm("_binary_banner_vt_start");
extern const uint8_t banner_end[] asm("_binary_banner_vt_end");

/* 2 倍のときは 40 桁 x 13 行しかないので、いつものバナーは枠がはみ出し、
 * 全体（約 20 行）も画面に収まらない。展示用の短いものに差し替える。 */
extern const uint8_t banner40_start[] asm("_binary_banner40_vt_start");
extern const uint8_t banner40_end[] asm("_binary_banner40_vt_end");

static cdk_display_info_t s_disp;
static cdk_canvas_t       s_canvas;
static uint16_t          *s_pixels;
static cdk_font_t         s_font;
static cdk_screen_t       s_screen;
static cdk_term_t         s_term;
static void              *s_termmem;
static bool               s_dirty;

/* ---- ステータス帯 ----
 * 帯を出すか。**再起動が要る**（端末の行数が変わるため）。 */
static bool     s_statusbar;
/* 帯を描く行。端末グリッドの 1 つ下（グリッドの外）。 */
static int      s_status_row;
static char     s_status_last[160];
/* SD の容量。マウント時に 1 回だけ取る（帯の描画で毎回問い合わせない）。 */
static uint32_t s_sd_mb;

static cdk_shell_t        s_shell;
static cdk_keys_decoder_t s_keydec;
static app_ctx_t          s_ctx;

static cdk_skk_t          s_skk;
static cdk_skk_dict_t     s_skkdict;
/* 個人辞書。SD が無いと消えるが、RAM 上でだけ働かせておく。
 * 学習しないより、その場かぎりでも先頭に来るほうが打鍵が減る。 */
static cdk_skk_user_entry_t s_skkuser_store[192];
static cdk_skk_user_t       s_skkuser;

/* いまの時刻 (ms)。再接続の間隔と USB のキーリピートで使う。 */
static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---- WiFi ----
 * 直前に見た状態。**変わった瞬間だけシェルへ 1 行返す**ために覚えておく。
 * 起動時の接続は待たない（DESIGN.md §8.1）ので、結果はここを見て気づく。 */
static cdk_wifi_state_t s_wifi_state = CDK_WIFI_IDLE;
/* いま繋ぎに行っている SSID。**設定値 (s_wifi_ssid) とは別物。**
 * 設定を書き換えても、走っている試行の相手は変わらないため。 */
static char s_wifi_trying[64];

/* ---- 再接続 ----
 * WiFi が切れたり相手が落ちたりしたときに繋ぎ直す。
 * **リモートで `exit` したときは繋ぎ直さない**（抜けられなくなる）。
 * どのキーでも中止できる。展示中に勝手に繋ぎ直し続けるのを止められないと困る。 */
static cdk_retry_t s_retry;
static bool        s_reconnect_enabled = true;

#define RECONNECT_BASE_MS 1000u
#define RECONNECT_CAP_MS  15000u
#define RECONNECT_TRIES   8u

static cdk_edit_t s_edit;
static char      *s_edit_arena;
#define EDIT_ARENA_BYTES (128 * 1024)

/* スクロールバックの行数。1 行 = 桁数 x 16 バイトなので、
 * 80 桁なら 256KB、40 桁（2 倍表示）なら 128KB。PSRAM から取る。
 * 200 行はいまの画面 8 枚ぶんで、あふれた出力を遡るには足りる。 */
#define SCROLLBACK_LINES 200

/* ------------------------------------------------------------------ */
/* セッション                                                          */
/* ------------------------------------------------------------------ */

typedef enum {
    SESSION_SHELL = 0,
    SESSION_SSH,
    SESSION_EDIT,
    SESSION_WASM,
    SESSION_SCP,
    SESSION_WGET,
#if CDK_BOARD_HAS_EXT_UART
    SESSION_UART
#endif
} session_t;

static session_t s_session = SESSION_SHELL;

#if CDK_BOARD_HAS_EXT_UART
/* いま開いている UART の設定。帯に出すので main が持つ。 */
static int  s_uart_baud;
static bool s_uart_flow;

/* `Ctrl-]` を受けた直後か。次の 1 打で抜けるかどうかが決まる。
 *
 * **前置キーにしたのは、`Ctrl-]` そのものを相手に送る道を残すため**
 * （DESIGN.md の「見送ったもの」がここを決めろと書いていた）。
 * `Ctrl-] q` で抜け、それ以外は 0x1D を送ってからそのキーを送る。 */
static bool s_uart_prefix;

/* 詰まっていることを既に知らせたか。**詰まるたびに出すと画面が埋まる。**
 * 1 バイトずつ弾かれると貼り付けの長さぶん行が出て、
 * 「詰まりを見せる」どころか本文が流れて消える。 */
static bool s_uart_stalled;
#endif

/* 端末へ書く。実体はこの下の方にある。 */
static void term_printf(const char *fmt, ...);

/* 画面の持ち主が変わるときは入力中のものを捨てる。
 * 残すと、次の持ち主の画面に前の ▽ が出たままになる。 */
static void drop_preedit(void)
{
    cdk_skk_reset(&s_skk);
    cdk_screen_set_preedit(&s_screen, "");
    s_dirty = true;
}

static void enter_shell(void)
{
    s_session = SESSION_SHELL;
    drop_preedit();
    cdk_shell_begin(&s_shell);
    s_dirty = true;
}

static void enter_ssh(void)
{
    s_session = SESSION_SSH;
    drop_preedit();
    cdk_shell_end(&s_shell);
}

/* 日本語入力のモードはセッションごとに覚える。
 *
 * 1 つにすると、エディタでかな入力にしたまま抜けたときにシェルの
 * コマンドが変換され、`ls` が「ｌｓ」になって何も打てなくなる。
 * 逆にエディタへ入るたびに Ctrl-J を押させるのも煩わしい。 */
static cdk_skk_mode_t s_mode_shell = CDK_SKK_ASCII;
static cdk_skk_mode_t s_mode_edit = CDK_SKK_ASCII;

/* エディタは代替画面で開く。抜けたときにシェルの画面がそのまま戻る。 */
static void enter_edit(void)
{
    s_mode_shell = cdk_skk_get_mode(&s_skk);
    s_session = SESSION_EDIT;
    drop_preedit();
    cdk_skk_set_mode(&s_skk, s_mode_edit);
    cdk_shell_end(&s_shell);
    term_printf("\033[?1049h");
    cdk_edit_set_size(&s_edit, s_term.cols, s_term.rows);
    s_edit.mode_label = cdk_skk_mode_label(&s_skk);
    cdk_edit_draw(&s_edit);
    s_dirty = true;
}

/* ユーザーアプリ。**edit と同じく、切り替えて即 return する。**
 * アプリ自体は専用タスクで走っていて、ここは画面とキーの持ち主を
 * 移すだけ。走っている間シェルを止めるので、ロックを持たない
 * port_periph / port_cfg をシェルとアプリが同時に叩くことがない。 */
static void enter_wasm(void)
{
    s_session = SESSION_WASM;
    drop_preedit();
    cdk_shell_end(&s_shell);
    s_dirty = true;
}

static void leave_wasm(void)
{
    enter_shell();
}

/* ファイル転送。**wasm と同じく、切り替えて即 return する。**
 *
 * 転送そのものはメインループが `cdk_scp_poll()` で刻むので、画面もキーも
 * 制御チャネルも生きたまま。**それでもシェルは止める。**
 * 止めないと、転送中に `ssh` を叩かれて `cdk_ssh_connect()` が
 * `cdk_ssh_close()` を呼び、scp のセッションを黙って壊す。FATFS の空き
 * ハンドルも本体には 1 本しか無いので `edit` / `cat` とも喧嘩する。
 * 止めておけば、その事故が「起きにくい」ではなく**起きない**。 */
static void enter_scp(void)
{
    s_session = SESSION_SCP;
    drop_preedit();
    cdk_shell_end(&s_shell);
    s_dirty = true;
}

static void leave_scp(void)
{
    enter_shell();
}

/* `wget`。**scp と同じ理由でシェルを止める**（FATFS の空きハンドルは
 * 本体に 1 本しか無く、受け取り中の `.tmp` がそれを握っている）。 */
static void enter_wget(void)
{
    s_session = SESSION_WGET;
    drop_preedit();
    cdk_shell_end(&s_shell);
    s_dirty = true;
}

static void leave_wget(void)
{
    enter_shell();
}

#if CDK_BOARD_HAS_EXT_UART
/* 外部 UART の端末。**wasm / scp と同じく、切り替えて即 return する。**
 * 汲むのはメインループで、ここは画面とキーの持ち主を移すだけ。 */
static void enter_uart(void)
{
    s_session = SESSION_UART;
    s_uart_prefix = false;
    /* **詰まりの通知を出し直せるようにする。** 詰まったまま抜けて
     * 開き直したとき、立ったままだと「送信が詰まっています」が
     * 二度と出ない（通知は書き込みが通ったときにしか降りない）。
     * 配線を直そうとしている最中こそ出てほしい。 */
    s_uart_stalled = false;
    drop_preedit();
    cdk_shell_end(&s_shell);
    s_dirty = true;
}

static void leave_uart(void)
{
    /* **必ず閉じる。** 閉じ忘れると UART2 が掴まれたままになり、
     * 以後 `wasm run uartecho` が開けなくなる（貸し出し口は 1 本きり）。 */
    (void)cdk_uart_close();
    s_uart_baud = 0;
    s_uart_flow = false;
    s_uart_prefix = false;
    /* **知らせるのは enter_shell の前。** 逆にするとプロンプトを出した
     * あとに行が挟まって、打ちかけの行が上へ流れる（ssh / scp と同じ順）。 */
    term_printf("\r\n\033[90muart  : 戻りました\033[0m\r\n");
    enter_shell();
}

/* `uart` コマンドから頼まれて開く。`app_ssh_open` と同じ形。
 *
 * **開けるのも閉じるのもここ。** コマンド側に開けさせると、閉じるのが
 * main.c で持ち主が 2 つに割れるし、帯に出す速度とフローの値も要る。 */
static bool app_uart_open(int baud, bool flow, const char **why)
{
    /* wasm アプリが掴んでいたら断る。外に出せる UART は 1 本きり。 */
    if (cdk_uart_is_open()) {
        if (why != NULL) *why = "UART2 は使用中です（アプリが開いています）";
        return false;
    }
    const esp_err_t err = cdk_uart_open(CDK_BOARD_EXT_UART_TX,
                                        CDK_BOARD_EXT_UART_RX, baud, flow,
                                        CDK_BOARD_EXT_UART_RTS,
                                        CDK_BOARD_EXT_UART_CTS);
    if (err != ESP_OK) {
        if (why != NULL) *why = esp_err_to_name(err);
        return false;
    }
    s_uart_baud = baud;
    s_uart_flow = flow;
    enter_uart();
    return true;
}

/* 相手へ送る。**詰まったら捨てて、そのことを人に見せる。**
 *
 * 詰まる理由は 2 つあって、どちらかは中から見分けられない:
 *  - フロー制御を入れて相手の CTS が浮いている（永久に捌けない）
 *  - 単に速度が足りない。制御チャネルは 921600 で来るので、
 *    `uart 9600` に 1KB 以上貼り付けると TX リング (1KB) が溢れる
 * どちらにしても黙って消えると「打っても何も起きない」になるので、
 * 原因を決めつけない文言で知らせる。 */
static void uart_send(const uint8_t *p, size_t n)
{
    if (n == 0) return;
    if (cdk_uart_write(p, (int)n) < 0) {
        if (!s_uart_stalled) { /* 詰まりに入った 1 回だけ知らせる */
            s_uart_stalled = true;
            term_printf("\r\n\033[33muart  : 送信が詰まっています"
                        "（相手が受け取っていないか、速度が"
                        "追いついていません）\033[0m\r\n");
            s_dirty = true;
        }
        return;
    }
    s_uart_stalled = false; /* 通ったので、次の詰まりはまた知らせる */
}
#endif /* CDK_BOARD_HAS_EXT_UART */

static void leave_edit(void)
{
    s_mode_edit = cdk_skk_get_mode(&s_skk);
    term_printf("\033[?1049l");
    enter_shell();
    cdk_skk_set_mode(&s_skk, s_mode_shell);
}

/* SKK が確定した文字列。シェルにも SSH にも同じ道を通す。 */
static void skk_commit(void *ctx, const char *utf8, size_t len)
{
    (void)ctx;
    if (s_session == SESSION_SSH && cdk_ssh_is_open()) {
        (void)cdk_ssh_write((const uint8_t *)utf8, len);
        return;
    }
#if CDK_BOARD_HAS_EXT_UART
    /* **ここを忘れると確定した日本語がシェルへ漏れる。**
     * 打った本人は相手に送ったつもりなので、戻ったときに
     * コマンド行に化けて残るのが一番分かりにくい。 */
    if (s_session == SESSION_UART) {
        uart_send((const uint8_t *)utf8, len);
        return;
    }
#endif
    if (s_session == SESSION_EDIT) {
        cdk_edit_insert(&s_edit, utf8, len);
        return;
    }
    /* シェルは キーイベント しか見ない。確定した文字列を
     * コードポイントに割ってから渡す。 */
    cdk_utf8_t dec = {0};
    for (size_t i = 0; i < len; i++) {
        uint32_t cp = 0;
        bool     retry = false;
        if (cdk_utf8_feed(&dec, (uint8_t)utf8[i], &cp, &retry) == CDK_UTF8_OK) {
            const cdk_key_t k = {0, 0, cp};
            cdk_shell_key(&s_shell, &k);
        }
    }
}

/* キーの行き先を決める唯一の場所。 */
static void route_key(void *user, const cdk_key_t *k)
{
    (void)user;

    /* **どのキーでも再接続を中止する。** 展示中に勝手に繋ぎ直し続けるのを
     * 止められないと困る。特定のキーにすると、そのキーを覚えていないと
     * 止められない。 */
    if (cdk_retry_active(&s_retry)) {
        cdk_retry_stop(&s_retry);
        /* シェルの行を避けて書く（プロンプトの後ろに続けない）。 */
        cdk_shell_hide_line(&s_shell);
        term_printf("\033[33mssh   : 再接続をやめました\033[0m\r\n");
        cdk_shell_show_line(&s_shell);
        s_dirty = true;
        return;
    }

    /* ---- スクロールバック ----
     *
     * **位置は動かせない。**
     *  - これより下（SKK の下）だと、`▽` 変換中に SKK が文字にならない
     *    キーを外へ漏らさないので、変換中だけ効かなくなる。
     *  - さらに下（EDIT / WASM / SSH の分岐）はどれも無条件に return するので、
     *    シェル以外では一生届かない。
     *  - これより上（再接続の中止）は「どのキーでも中止する」契約を壊す。
     *    そのぶん、再接続を待っている間はスクロールが効かない。
     *
     * Shift+PgUp / PgDn は端末の慣例で、SSH 先へは送らないのが普通なので
     * `vi` や `less` と衝突しない。 */
    if ((k->mods & CDK_MOD_SHIFT_BIT) != 0u &&
        (k->keycode == CDK_K_PGUP || k->keycode == CDK_K_PGDN)) {
        /* 1 画面ぶん。1 行重ねると読みつなぎやすい。 */
        int step = s_term.rows - 1;
        if (step < 1) {
            step = 1;
        }
        const int moved =
            cdk_term_view_scroll(&s_term, (k->keycode == CDK_K_PGUP) ? step : -step);
        if (moved != 0) {
            s_dirty = true;
            return; /* 動けたときだけ消費する */
        }
        /* 動けないなら消費しない。代替画面（エディタや vi）では履歴が
         * 無効なので、そのまま下へ落ちて PgUp として扱われる。 */
    } else if (cdk_term_view_offset(&s_term) > 0) {
        /* **何か打ったら最下部へ戻す。** 遡ったまま入力すると、
         * 打った字が見えないところへ出ていって混乱する。
         * キーは消費せずそのまま下へ流す。 */
        cdk_term_view_reset(&s_term);
        s_dirty = true;
    }

    /* **パスワードを聞いている間は日本語入力を通さない。**
     * かな入力のままだと `pass` が「ぱっｓ」になって送られるし、変換中の
     * `▽` に打った字がそのまま出る（伏せ字にならない）。
     * ここだけは SKK より先に判断する必要がある。 */
    if (s_session == SESSION_SHELL && cdk_shell_asking(&s_shell)) {
        cdk_shell_key(&s_shell, k);
        return;
    }

#if CDK_BOARD_HAS_EXT_UART
    /* **抜けるキーの判定は SKK より前。**
     *
     * かなモードの SKK は素の `q` を食う（カタカナ切り替え）。ここを
     * SKK の後ろに置くと、日本語を打つためにかなにした時点で `Ctrl-] q`
     * が届かず、**画面を占有したまま抜ける手段が無くなる**。 */
    if (s_session == SESSION_UART) {
        if (!s_uart_prefix) {
            if ((k->mods & CDK_MOD_CTRL_BIT) && k->codepoint == ']') {
                s_uart_prefix = true;
                return;
            }
        } else {
            s_uart_prefix = false;
            if (k->codepoint == 'q' || k->codepoint == 'Q') {
                leave_uart();
                return;
            }
            /* 抜けないなら `Ctrl-]` そのものを相手へ送り、**この打鍵は
             * 下へ落とす**（2 打目は SKK を通す。日本語がそのまま打てる）。 */
            static const uint8_t gs = 0x1D;
            uart_send(&gs, 1);
        }
    }
#endif

    /* 日本語入力はシェル / SSH の分岐より**前**に置く。ここを分岐の
     * 3 本目にすると「このキーは誰が受け取るのか」が追えなくなる。
     * 消費されなかったキーだけが下へ落ちる。 */
    const bool skk_used = cdk_skk_key(&s_skk, k);
    cdk_screen_set_preedit(&s_screen, cdk_skk_preedit(&s_skk));
    if (skk_used) {
        s_dirty = true;
        if (s_session == SESSION_EDIT) {
            cdk_edit_draw(&s_edit); /* 確定した文字を反映する */
        }
        return;
    }

    if (s_session == SESSION_EDIT) {
        (void)cdk_edit_key(&s_edit, k);
        if (s_edit.want_save) {
            s_edit.want_save = false;
            (void)cdk_edit_file_save(&s_edit);
        }
        if (s_edit.want_quit) {
            s_edit.want_quit = false;
            leave_edit();
            return;
        }
        s_edit.mode_label = cdk_skk_mode_label(&s_skk);
        cdk_edit_draw(&s_edit);
        s_dirty = true;
        return;
    }

    if (s_session == SESSION_WASM) {
        /* Ctrl-C は止める合図。アプリには渡さない。
         * 渡してしまうと、読まないアプリを止める手段が無くなる。 */
        if ((k->mods & CDK_MOD_CTRL_BIT) && (k->codepoint == 'c' || k->codepoint == 'C')) {
            cdk_wasm_run_stop();
            term_printf("\r\n\033[33mwasm  : 止めています...\033[0m\r\n");
            s_dirty = true;
            return;
        }
        /* それ以外は**端末が出すのと同じバイト列**にしてアプリへ。
         * ゲストからは普通の端末に見える（新しいキーの表現を作らない）。 */
        uint8_t out[16];
        const size_t n = cdk_keys_encode(k, s_term.app_cursor_keys, out, sizeof(out));
        if (n > 0) cdk_wasm_run_feed(out, n);
        return;
    }

    if (s_session == SESSION_SCP) {
        /* Ctrl-C だけ拾って、**残りは捨てる。**
         * シェルは止めてあるので、ここで通してもプロンプトは無い。
         * 捨てないと転送中に打った文字が終わったあとのシェルへ流れ込む。 */
        if ((k->mods & CDK_MOD_CTRL_BIT) &&
            (k->codepoint == 'c' || k->codepoint == 'C')) {
            cdk_scp_abort();
            term_printf("\r\n\033[33mscp   : 止めています...\033[0m\r\n");
            s_dirty = true;
        }
        return;
    }

    if (s_session == SESSION_WGET) {
        /* scp と同じ。Ctrl-C だけ拾って残りは捨てる。 */
        if ((k->mods & CDK_MOD_CTRL_BIT) &&
            (k->codepoint == 'c' || k->codepoint == 'C')) {
            cdk_wget_abort();
            term_printf("\r\n\033[33mwget  : 止めています...\033[0m\r\n");
            s_dirty = true;
        }
        return;
    }

    if (s_session == SESSION_SSH && cdk_ssh_is_open()) {
        uint8_t out[16];
        const size_t n = cdk_keys_encode(k, s_term.app_cursor_keys, out, sizeof(out));
        if (n > 0) {
            (void)cdk_ssh_write(out, n);
        }
        return;
    }

#if CDK_BOARD_HAS_EXT_UART
    if (s_session == SESSION_UART) {
        /* **Ctrl-C は横取りしない**（wasm / scp とは逆）。相手の CLI を
         * 止める合図なので、こちらで食うと相手を止められなくなる。
         * 抜けるのは `Ctrl-] q` だけ。 */
        uint8_t out[16];
        const size_t n = cdk_keys_encode(k, s_term.app_cursor_keys, out, sizeof(out));
        uart_send(out, n);
        return;
    }
#endif
    cdk_shell_key(&s_shell, k);
}

/* 端末の入力バイト列（cdk.py type や将来の他の入力源）を流し込む。
 * SSH に入っているときは解釈せずそのまま送る。リモートの CLI が
 * 自分でエスケープ列を解釈するので、こちらで触ると壊れる。 */
static void route_bytes(const uint8_t *data, size_t len)
{
    if (s_session == SESSION_SSH && cdk_ssh_is_open()) {
        (void)cdk_ssh_write(data, len);
        return;
    }
#if CDK_BOARD_HAS_EXT_UART
    /* **前置キーの状態機械をここにも置く。** scp の Ctrl-C と同じ理由で、
     * 入力の出どころが 2 つあるため。`cdk.py key ctrl+]` は MSG_KEY で
     * route_key に来るが、**`cdk.py type` は MSG_TEXT でここに来る**ので、
     * 片方だけに入れると貼り付けた 0x1D の扱いが食い違う。
     * 抜けるのは 0x1D のあとの `q`。それ以外は 0x1D ごと相手へ流す。 */
    if (s_session == SESSION_UART) {
        size_t i = 0;
        while (i < len) {
            if (s_uart_prefix) {
                const uint8_t c = data[i++];
                s_uart_prefix = false;
                if (c == 'q' || c == 'Q') {
                    leave_uart();
                    /* **残りは捨てる。** シェルへ戻ったあとのプロンプトに
                     * 流し込むと、打った覚えのない行が出来る。 */
                    return;
                }
                const uint8_t esc[2] = {0x1D, c};
                uart_send(esc, 2);
                continue;
            }
            /* **0x1D までをまとめて送る。** 1 バイトずつ渡すと、詰まった
             * ときに弾かれる回数が貼り付けの長さぶんになる。 */
            size_t run = 0;
            while (i + run < len && data[i + run] != 0x1D) run++;
            if (run > 0) {
                uart_send(data + i, run);
                i += run;
                continue;
            }
            i++; /* data[i] == 0x1D */
            s_uart_prefix = true;
        }
        return;
    }
#endif
    /* ユーザーアプリも SSH と同じ扱い。こちらでエスケープを解かない。
     * ただし Ctrl-C だけは横取りする（route_key と揃える）。 */
    if (s_session == SESSION_WASM) {
        for (size_t i = 0; i < len; i++) {
            if (data[i] == 0x03) {
                cdk_wasm_run_stop();
                term_printf("\r\n\033[33mwasm  : 止めています...\033[0m\r\n");
                s_dirty = true;
                return;
            }
        }
        cdk_wasm_run_feed(data, len);
        return;
    }
    /* 転送中も同じ扱い。**ここを忘れると `cdk.py key ctrl+c` が黙って
     * 効かない**（あちらは制御チャネルのバイト列で来るので route_key を
     * 通らない）。wasm が 2 か所に入れてある理由がそのまま当てはまる。 */
    if (s_session == SESSION_SCP) {
        for (size_t i = 0; i < len; i++) {
            if (data[i] == 0x03) {
                cdk_scp_abort();
                term_printf("\r\n\033[33mscp   : 止めています...\033[0m\r\n");
                s_dirty = true;
                return;
            }
        }
        return; /* 残りは捨てる */
    }
    /* wget も同じ。**ここを忘れると `cdk.py key ctrl+c` が効かない。** */
    if (s_session == SESSION_WGET) {
        for (size_t i = 0; i < len; i++) {
            if (data[i] == 0x03) {
                cdk_wget_abort();
                term_printf("\r\n\033[33mwget  : 止めています...\033[0m\r\n");
                s_dirty = true;
                return;
            }
        }
        return; /* 残りは捨てる */
    }
    /* エディタはキーイベントで受ける。SSH と違い、こちらでエスケープを
     * 解いてよい（相手が解釈するわけではないので）。 */
    cdk_keys_decode_feed(&s_keydec, data, len);
    /* 1 回の送信で切れた ESC は「Esc キー」として確定させる。 */
    cdk_keys_decode_flush(&s_keydec);
}

/* シェルの出力は端末へ。 */
static void shell_out(void *user, const char *data, size_t len)
{
    (void)user;
    cdk_term_feed(&s_term, data, len);
    s_dirty = true;
}

/* 端末が返す応答 (DA / DSR)。
 *
 * SSH に返さないと、CSI 6n でカーソル位置を問い合わせて待つ CLI が
 * 永久に固まる。しかも「端末のバグ」ではなく「CLI が謎のハング」に見える。 */
static void on_term_reply(void *user, const uint8_t *data, size_t len)
{
    (void)user;
    if (s_session == SESSION_SSH && cdk_ssh_is_open()) {
        (void)cdk_ssh_write(data, len);
    }
#if CDK_BOARD_HAS_EXT_UART
    /* UART の相手も CLI なので同じ。返さないと `CSI 6n` を出す相手が
     * 永久に待つ。
     *
     * **ここだけは `uart_send` を通さない。** この関数は
     * `cdk_term_feed` の途中（エスケープ列を解いている最中）から呼ばれる。
     * `uart_send` は詰まったときに `term_printf` で知らせ、それが
     * `cdk_term_feed` を呼ぶので、解きかけの状態で再入して壊れる。
     * 応答を 1 つ落とすのは構わないが、パーサを壊すのは困る。 */
    if (s_session == SESSION_UART) {
        (void)cdk_uart_write(data, (int)len);
    }
#endif
    /* シェルは問い合わせないので、それ以外では捨てる。 */
}

/* ------------------------------------------------------------------ */
/* 制御チャネル                                                        */
/* ------------------------------------------------------------------ */

static void send_screenshot(void)
{
    const cdk_wire_shot_info_t info = {
        .width = (uint16_t)s_canvas.w,
        .height = (uint16_t)s_canvas.h,
        .format = CDK_PIXFMT_RGB565_LE,
        .gamma_x10 = s_screen.gamma_x10,
        .scale = s_screen.scale,
        /* **本文が終わる位置から下ぜんぶ。** 帯 1 行だけでなく、
         * 端数の 8px も含める（480x320 は 12行x24px = 288 で、
         * 残り 32px が帯 24 + 端数 8）。ここを帯の高さだけにすると
         * 端数の 8px が比較に残り、本文だけを比べているつもりで
         * 帯の一部を比べてしまう。 */
        .status_h = (uint8_t)(s_statusbar
                                  ? (s_canvas.h - s_term.rows * cdk_screen_cell_h(&s_screen))
                                  : 0),
        .byte_len = (uint32_t)cdk_canvas_bytes(s_canvas.w, s_canvas.h),
    };

    if (cdk_link_send(CDK_MSG_SHOT_INFO, &info, sizeof(info)) != ESP_OK) {
        ESP_LOGW(TAG, "SHOT_INFO を送れませんでした");
        return;
    }
    if (cdk_link_send_chunked(CDK_MSG_SHOT_DATA, s_canvas.pix, info.byte_len) != ESP_OK) {
        ESP_LOGW(TAG, "SHOT_DATA が途中で切れました");
        return;
    }
    (void)cdk_link_send(CDK_MSG_SHOT_END, NULL, 0);
    ESP_LOGI(TAG, "スクリーンショット送信 %ux%u", info.width, info.height);
}

static void on_frame(void *user, uint8_t type, const uint8_t *payload, uint32_t len)
{
    (void)user;

    switch (type) {
    case CDK_MSG_PING:
        (void)cdk_link_send(CDK_MSG_PONG, payload, len);
        break;

    case CDK_MSG_SHOT_REQ:
        send_screenshot();
        break;

    case CDK_MSG_TEXT:
        route_bytes(payload, len);
        break;

    /* 画面をホストの基準画像と突き合わせるための入口。
     * 端末を初期化してから流すので、起動時の WiFi/SD の状態表示が
     * 残らない。TEXT では入力として扱われてシェルに届いてしまうので、
     * 画面の検証には使えない。 */
    case CDK_MSG_VT:
        if (len >= 1) {
            /* 先頭 1 バイトがフラグ。bit0 で端末を初期化する。
             * 分割して送るときに毎回初期化すると前半が消えるので、
             * 「どのフレームが先頭か」を送り側に言わせる。 */
            if ((payload[0] & 0x01u) != 0u) {
                drop_preedit();
                cdk_term_reset(&s_term);
            }
            cdk_term_feed(&s_term, payload + 1, len - 1);
            s_dirty = true;
        }
        break;

    case CDK_MSG_KEY:
        if (len >= sizeof(cdk_wire_key_t)) {
            cdk_wire_key_t w;
            memcpy(&w, payload, sizeof(w));
            const cdk_key_t k = {w.keycode, w.mods, w.codepoint};
            route_key(NULL, &k);
        }
        break;

    /* ホストから microSD へファイルを書く。中身は main/cdk_pushfile.c。 */
    case CDK_MSG_FILE_BEGIN:
        cdk_push_begin(payload, len);
        break;
    case CDK_MSG_FILE_DATA:
        cdk_push_data(payload, len);
        break;
    case CDK_MSG_FILE_END:
        cdk_push_end(payload, len);
        break;

    default:
        ESP_LOGW(TAG, "未知のフレーム type=0x%02X len=%u", type, (unsigned)len);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* 起動時の表示                                                        */
/* ------------------------------------------------------------------ */

/* 端末に 1 行出して即座に画面へ反映する。起動中の進捗を実機で見せるため。
 *
 * **入れ物は static。** メインタスクのスタックは公開鍵認証で一度溢れて
 * いる（#40）ので積まない。呼ぶのはメインタスクだけ（コマンドも
 * メインループの中で走る）で、**自分の中から呼び直されることも無い**
 * （cdk_term_feed の途中から来る応答の口では使わない約束。DESIGN.md §10
 * 「応答を返す口では term_printf を使えない」）。この 2 つが崩れたら
 * static はやめること。
 *
 * **入りきらないときも行末は守る。** 192 バイトだった頃、長いパスの
 * `scp : 送ります ...（鍵のみ）...\r\n` が切れて改行ごと落ち、次の
 * 知らせが同じ行に続いた。色の戻し (`\033[0m`) が落ちると、以降の字が
 * ずっと赤くなる。切るなら UTF-8 の字の切れ目で切り、戻しと改行を足す。 */
static void term_printf(const char *fmt, ...)
{
    static char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if (n >= (int)sizeof(buf)) {
        static const char tail[] = "\033[0m\r\n";
        const size_t      flen = strlen(fmt);
        const bool        nl = (flen >= 2 && strcmp(fmt + flen - 2, "\r\n") == 0);
        const size_t      tlen = nl ? sizeof(tail) - 1 : sizeof(tail) - 3;
        size_t            cut = sizeof(buf) - 1 - tlen;
        /* UTF-8 の続きのバイトの途中なら、字の頭まで戻る。エスケープ列の
         * 途中で切れても、後ろの `\033[0m` の ESC で打ち切られる。 */
        while (cut > 0 && ((unsigned char)buf[cut] & 0xC0u) == 0x80u) {
            cut--;
        }
        memcpy(buf + cut, tail, tlen);
        n = (int)(cut + tlen);
    }
    cdk_term_feed(&s_term, buf, (size_t)n);
    cdk_screen_draw(&s_screen, &s_canvas, &s_term, false);
    (void)cdk_display_flush(&s_canvas);
}

/* ------------------------------------------------------------------ */
/* ステータス帯                                                        */
/* ------------------------------------------------------------------ */

/* 端末グリッドの外に、main が直接描く。
 *
 * **DECSTBM で最下行を確保する手は使えない。** vi も less も top も自分で
 * スクロール領域を設定するか ESC[r で解除して全画面を使うので、SSH 先で
 * それらを動かした瞬間に上書きされる。グリッドの外に置けば触られない。 */
/* 電池電圧。**帯のために毎周は読まない。** メインループは速く回るので、
 * 毎周 ADC を 8 回読むと無駄が大きいし、読むたびに数 mV 揺れて
 * 帯の描き直しが止まらない。電池はそんなに速く減らない。 */
#define BATT_PERIOD_MS 10000u
static int      s_batt_mv;
static uint32_t s_batt_at;
static bool     s_batt_read_once;

static int batt_mv_cached(void)
{
#if CDK_BOARD_HAS_BATTERY_SENSE
    const uint32_t now = now_ms();
    if (!s_batt_read_once || now - s_batt_at >= BATT_PERIOD_MS) {
        int mv = 0;
        s_batt_mv = (cdk_batt_read_mv(&mv) == ESP_OK) ? mv : 0;
        s_batt_at = now;
        s_batt_read_once = true;
    }
    return s_batt_mv;
#else
    return 0;
#endif
}

static void draw_status(bool force)
{
    if (!s_statusbar) {
        return;
    }

    char ip[20];
    cdk_wifi_ip(ip, sizeof(ip));

    cdk_status_t st;
    memset(&st, 0, sizeof(st));
    st.wifi_up = cdk_wifi_is_connected();
    st.ip = ip;
    st.wifi_connecting = (s_wifi_state == CDK_WIFI_CONNECTING);
    st.sd_ok = cdk_sd_mounted();
    st.sd_mb = s_sd_mb;
    st.ime = cdk_skk_mode_label(&s_skk);
    st.kbd = cdk_usbhid_connected();
    st.ssh_up = cdk_ssh_is_open();
#if CDK_BOARD_HAS_EXT_UART
    /* UART セッションの間だけ出す。開いていても別のセッションに居るなら
     * 出さない（帯は「いま何を触っているか」を出す場所）。 */
    if (s_session == SESSION_UART) {
        st.uart_baud = s_uart_baud;
        st.uart_flow = s_uart_flow;
    }
#endif
    st.scroll_back = cdk_term_view_offset(&s_term);
    st.batt_mv = batt_mv_cached();

    char line[160];
    cdk_status_format(&st, s_term.cols, line, sizeof(line));

    /* 変わっていなければ描き直さない。毎周描くと帯のぶんだけ
     * 転送が増え続ける（ダーティ行に入りっぱなしになる）。 */
    if (!force && strcmp(line, s_status_last) == 0) {
        return;
    }
    strncpy(s_status_last, line, sizeof(s_status_last) - 1);
    s_status_last[sizeof(s_status_last) - 1] = '\0';

    cdk_cell_t cell;
    memset(&cell, 0, sizeof(cell));
    cell.fg = CDK_COLOR_IDX(15); /* 白 */
    cell.bg = CDK_COLOR_IDX(4);  /* 青 */

    const size_t n = strlen(line);
    size_t       i = 0;
    int          col = 0;
    while (col < s_term.cols) {
        uint32_t cp = (uint32_t)' ';
        int      w = 1;
        if (i < n) {
            bool         ok = false;
            const size_t used = cdk_utf8_decode((const uint8_t *)line + i, n - i, &cp,
                                                &ok);
            if (used == 0) {
                break;
            }
            i += used;
            if (!ok) {
                cp = (uint32_t)' ';
            }
            w = cdk_char_width(cp);
            if (w <= 0) {
                continue;
            }
        }
        if (col + w > s_term.cols) {
            break; /* 全角が右端をまたぐ。空白のままにする */
        }
        cell.cp = (cp == (uint32_t)' ') ? 0u : cp;
        cell.attr = (w == 2) ? CDK_ATTR_WIDE_LEAD : 0u;
        cdk_screen_draw_cell(&s_screen, &s_canvas, &cell, col, s_status_row, false);
        col += w;
    }
    s_dirty = true;
}

/* 転送の進み具合を 1 行で出す。
 *
 * **行を増やさない。** `\r` で同じ行を書き直す（40 桁 12 行しかないので、
 * 毎回改行すると本文が流れて消える）。`cdk_shell_notify` は使えない
 * ——あれはシェルが動いているときだけ書く作りで、転送中は止めてある。
 *
 * 矢印は ASCII。`↑` `↓` は東雲 12px の収録範囲を当てにできないし、
 * 幅と字形が食い違うものを出さないのが §5.3 の約束。 */
static void scp_progress_line(void)
{
    cdk_scp_status_t st;
    if (!cdk_scp_status(&st)) {
        return;
    }

    /* 毎周書くと転送より描画が重くなる。**変わったときだけ、間隔を空けて。**
     * `draw_status` が「変わったときだけ描く」のと同じ理屈。 */
    static uint32_t last_ms;
    static uint32_t last_pct = 0xFFFFFFFFu;
    /* 64 ビットで掛ける。上限を外したので、32 ビットだと 43MB で溢れる。 */
    const uint32_t  pct =
        (st.total > 0) ? (uint32_t)((uint64_t)st.done * 100u / st.total) : 0u;
    const uint32_t  now = now_ms();
    if (pct == last_pct && now - last_ms < 300u) {
        return;
    }
    last_ms = now;
    last_pct = pct;

    const char *arrow = st.upload ? "->" : "<-";
    if (st.phase == CDK_SCP_PHASE_OPEN) {
        term_printf("\r\033[K\033[33mscp   : %s 開いています\033[0m", arrow);
    } else if (st.total > 0) {
        if (s_term.cols < 60) {
            term_printf("\r\033[K\033[33mscp   : %s %luKB %lu%%\033[0m", arrow,
                        (unsigned long)((st.done + 1023u) / 1024u),
                        (unsigned long)pct);
        } else {
            term_printf("\r\033[K\033[33mscp   : %s %luKB / %luKB  %lu%%\033[0m",
                        arrow, (unsigned long)((st.done + 1023u) / 1024u),
                        (unsigned long)((st.total + 1023u) / 1024u),
                        (unsigned long)pct);
        }
    } else {
        term_printf("\r\033[K\033[33mscp   : %s %luKB\033[0m", arrow,
                    (unsigned long)((st.done + 1023u) / 1024u));
    }
}

/* `wget` の進み具合。`scp_progress_line` と同じ作り（1 行を書き直す、
 * 変わったときだけ 300ms 空けて）。chunked だと全体が分からないので KB だけ。 */
static void wget_progress_line(void)
{
    cdk_wget_status_t st;
    if (!cdk_wget_status(&st)) {
        return;
    }

    static uint32_t last_ms;
    static uint32_t last_kb = 0xFFFFFFFFu;
    const uint32_t  kb = (st.done + 1023u) / 1024u;
    const uint32_t  now = now_ms();
    if (kb == last_kb || now - last_ms < 300u) {
        return;
    }
    last_ms = now;
    last_kb = kb;

    if (st.total > 0) {
        const uint32_t pct = (uint32_t)((uint64_t)st.done * 100u / st.total);
        if (s_term.cols < 60) {
            term_printf("\r\033[K\033[33mwget  : <- %luKB %lu%%\033[0m",
                        (unsigned long)kb, (unsigned long)pct);
        } else {
            term_printf("\r\033[K\033[33mwget  : <- %luKB / %luKB  %lu%%\033[0m",
                        (unsigned long)kb,
                        (unsigned long)((st.total + 1023u) / 1024u),
                        (unsigned long)pct);
        }
    } else {
        term_printf("\r\033[K\033[33mwget  : <- %luKB\033[0m", (unsigned long)kb);
    }
}

static void draw_banner(void)
{
    /* 桁数で選ぶ（拡大率ではなく）。パネルが変わって桁数が減っても
     * 勝手に短いほうへ倒れる。 */
    const bool narrow = s_term.cols < 60;
    const uint8_t *p = narrow ? banner40_start : banner_start;
    const size_t   n = narrow ? (size_t)(banner40_end - banner40_start)
                              : (size_t)(banner_end - banner_start);
    cdk_term_feed(&s_term, p, n);
    s_dirty = true;
}

/* ------------------------------------------------------------------ */
/* 設定と接続                                                          */
/* ------------------------------------------------------------------ */

static char s_wifi_ssid[64];
static char s_wifi_psk[80];
static char s_ssh_host[80];
static char s_ssh_user[48];
static char s_ssh_pass[80];

/* `ssh alice@box` で渡された相手。空なら NVS の設定を使う。
 * **NVS には書かない**（その場かぎり。人間の指示）。
 *
 * **切断や失敗では消さない。消すのは引数なしの `ssh` だけ。**
 * 切れたときの繋ぎ直しはここを見るので、閉じるたびに消すと
 * `ssh alice@box` のセッションが落ちたあと NVS の既定へ繋ぎ直して
 * しまう。別の機械に入るので、いちばんやってはいけない間違い。 */
static char s_ssh_arg_host[80];
static char s_ssh_arg_user[48];

/* その場で打ってもらったパスワードと、**それが誰のものか**。
 *
 * NVS には書かない。電源を切れば消える（DESIGN.md §8.5）。
 *
 * **相手を一緒に覚えるのが肝。** パスワードだけ持つと、次に別の機械へ
 * 繋ぐときに黙って送ってしまう。`pick_password()` はこの 3 つが揃って
 * 初めて使う。 */
static char s_ask_host[80];
static char s_ask_user[48];
static char s_ask_pass[80];

/* パスワードを聞いている間の相手。cb は誰へ繋ぐのかを覚えていないので
 * ここに置く（聞いている最中に別の相手へは行けない。シェルは 1 本）。 */
static char s_ask_target_host[80];
static char s_ask_target_user[48];
/* cdk_shell_ask は写さないので生かしておく。**いちばん長い相手が入る
 * 大きさにする**（user 47 + `@` + host 79 + 「 のパスワード: 」21 = 148）。
 * 切り詰めると宛先が途中で切れて、打ち間違いに気づく手だてが無くなる。 */
static char s_ask_prompt[176];

static void ssh_forget_asked(void)
{
    memset(s_ask_host, 0, sizeof(s_ask_host));
    memset(s_ask_user, 0, sizeof(s_ask_user));
    memset(s_ask_pass, 0, sizeof(s_ask_pass));
}

/* 設定値を NVS から読む。無ければ secrets.h（ビルド時に焼いた値）で補い、
 * その場で NVS に書いて次回以降は NVS だけで動くようにする。
 *
 * 目的は認証情報をファームウェアのバイナリから外すこと。
 * secrets.h は「初回の種」としてだけ使う。 */
static void cfg_or_seed(const char *key, const char *seed, char *buf, size_t cap)
{
    if (cdk_cfg_get(key, buf, cap) == ESP_OK && buf[0] != '\0') {
        return;
    }
    if (seed == NULL || seed[0] == '\0') {
        buf[0] = '\0';
        return;
    }
    const esp_err_t err = cdk_cfg_set(key, seed);
    ESP_LOGI(TAG, "%s を secrets.h から NVS へ書きました (%s)", key,
             esp_err_to_name(err));
    strncpy(buf, seed, cap - 1);
    buf[cap - 1] = '\0';
}

/* NVS から読むだけ。無ければ空にする。 */
static void cfg_load(const char *key, char *buf, size_t cap)
{
    if (cdk_cfg_get(key, buf, cap) != ESP_OK) {
        buf[0] = '\0';
    }
}

/* 一度でも設定されたことがあるか。
 *
 * **`set ssh.pass ""` が効かない原因がここだった。** 空文字を渡すと
 * NVS のキーは消える。すると次の load_config が「未設定」と見なして
 * secrets.h の種を書き戻し、**消したはずのパスワードが復活する**。
 * つまり認証情報を消す手段が無かった。
 *
 * 種を撒くのは本当に初回だけにする。判定は「設定キーが 1 つでもあるか」。
 * 一度でも使われた機械なら、いまの中身がその人の意思。 */
static bool cfg_configured(void)
{
    char mark[8];
    if (cdk_cfg_get(CDK_CFG_SEEDED, mark, sizeof(mark)) == ESP_OK) {
        return true;
    }
    const char *const *keys = cdk_cfg_keys();
    char               buf[128];
    for (size_t i = 0; keys[i] != NULL; i++) {
        if (cdk_cfg_get(keys[i], buf, sizeof(buf)) == ESP_OK) {
            return true;
        }
    }
    return false;
}

void cdk_apply_gamma_setting(void)
{
    char buf[16];
    long g = CDK_BOARD_LCD_GAMMA_X10;

    /* **ここで NVS を開けておく。** 起動時はまだ load_config() が呼ばれて
     * いないので、これが無いと毎回「未設定」に見えて基板の既定値に落ちる。
     * `set gamma` で書いた値が再起動で消えたように見えるのがこの症状。
     * cdk_cfg_init() は何度呼んでもよい。 */
    if (cdk_cfg_init() != ESP_OK) {
        ESP_LOGW(TAG, "設定を開けないのでガンマは既定値 %ld を使います", g);
    } else if (cdk_cfg_get(CDK_CFG_GAMMA, buf, sizeof(buf)) == ESP_OK && buf[0] != '\0') {
        char *end = NULL;
        const long v = strtol(buf, &end, 10);
        /* 壊れた値で画面を読めなくしない。範囲外なら基板の既定値のまま。 */
        if (end != buf && v >= 10 && v <= 40) {
            g = v;
        } else {
            ESP_LOGW(TAG, "gamma=%s は範囲外 (10..40)。既定値 %ld を使います", buf, g);
        }
    }

    cdk_screen_set_gamma(&s_screen, (uint8_t)g);

    /* パレットではなくセルの色に掛けているので、描き直さないと変わらない。 */
    cdk_term_mark_all_dirty(&s_term);
    cdk_canvas_mark_all_dirty(&s_canvas);
}

void cdk_apply_keymap_setting(void)
{
#if CDK_BOARD_HAS_USB_HOST
    char buf[16];
    bool jis = (CDK_BOARD_KEYMAP_JIS_DEFAULT != 0);

    /* ガンマと同じ理由でここでも NVS を開ける（不具合 #38）。 */
    if (cdk_cfg_init() == ESP_OK &&
        cdk_cfg_get(CDK_CFG_KEYMAP, buf, sizeof(buf)) == ESP_OK && buf[0] != '\0') {
        jis = (strcmp(buf, "jis") == 0);
        if (!jis && strcmp(buf, "us") != 0) {
            ESP_LOGW(TAG, "keymap=%s は us / jis のどちらでもありません。us を使います",
                     buf);
        }
    }
    cdk_usbhid_set_layout(jis);
    ESP_LOGI(TAG, "キー配列 %s", jis ? "JIS" : "US");
#endif
}

void cdk_apply_reconnect_setting(void)
{
    char buf[8];
    s_reconnect_enabled = true; /* 既定は入り。持ち歩く機械なので切れやすい */
    if (cdk_cfg_init() == ESP_OK &&
        cdk_cfg_get(CDK_CFG_SSH_RECONNECT, buf, sizeof(buf)) == ESP_OK &&
        buf[0] != '\0') {
        s_reconnect_enabled = (strcmp(buf, "off") != 0);
    }
    if (!s_reconnect_enabled) {
        cdk_retry_stop(&s_retry);
    }
    ESP_LOGI(TAG, "SSH の再接続 %s", s_reconnect_enabled ? "する" : "しない");
}

/* SSH の秘密鍵のパス。未設定なら SD の既定の場所。
 * ここを 1 か所にしておかないと、`ssh key` が見る場所と実際に使う場所が
 * ずれる（設定したのに効かない、という一番分かりにくい壊れ方になる）。 */
const char *cdk_ssh_key_path(void)
{
    static char path[96];
    if (cdk_cfg_init() == ESP_OK &&
        cdk_cfg_get(CDK_CFG_SSH_KEY, path, sizeof(path)) == ESP_OK &&
        path[0] != '\0') {
        return path;
    }
    strncpy(path, "/sdcard/id_ecdsa", sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    return path;
}

static void load_config(void)
{
    if (cdk_cfg_init() != ESP_OK) {
        ESP_LOGE(TAG, "設定を読めません");
        return;
    }
    if (!cfg_configured()) {
        /* 本当の初回だけ secrets.h の種を撒く。 */
        cfg_or_seed(CDK_CFG_WIFI_SSID, CDK_WIFI_SSID, s_wifi_ssid, sizeof(s_wifi_ssid));
        cfg_or_seed(CDK_CFG_WIFI_PSK, CDK_WIFI_PSK, s_wifi_psk, sizeof(s_wifi_psk));
        cfg_or_seed(CDK_CFG_SSH_HOST, CDK_SSH_HOST, s_ssh_host, sizeof(s_ssh_host));
        cfg_or_seed(CDK_CFG_SSH_USER, CDK_SSH_USER, s_ssh_user, sizeof(s_ssh_user));
        cfg_or_seed(CDK_CFG_SSH_PASS, CDK_SSH_PASS, s_ssh_pass, sizeof(s_ssh_pass));
        (void)cdk_cfg_set(CDK_CFG_SEEDED, "1");
        ESP_LOGI(TAG, "secrets.h の値を NVS へ書きました（初回のみ）");
        return;
    }

    /* 2 回目以降は読むだけ。**空にしたものは空のまま。**
     * ここで種を撒き直すと `set ssh.pass ""` が効かない。 */
    cfg_load(CDK_CFG_WIFI_SSID, s_wifi_ssid, sizeof(s_wifi_ssid));
    cfg_load(CDK_CFG_WIFI_PSK, s_wifi_psk, sizeof(s_wifi_psk));
    cfg_load(CDK_CFG_SSH_HOST, s_ssh_host, sizeof(s_ssh_host));
    cfg_load(CDK_CFG_SSH_USER, s_ssh_user, sizeof(s_ssh_user));
    cfg_load(CDK_CFG_SSH_PASS, s_ssh_pass, sizeof(s_ssh_pass));
}

/* 接続を始めるだけ。**待たない。**
 *
 * 待つと、AP が居ない場所で起動が 20 秒止まる。その間キーも効かないので、
 * 設定を直すことも `wifi connect` をやり直すこともできない。
 * 途中経過はステータス帯（「WiFi接続中」→ IP）に出し、結果は
 * wifi_pump() がシェルへ 1 行返す。 */
static void app_wifi_connect(void)
{
    load_config();

    if (s_wifi_ssid[0] == '\0') {
        term_printf("\033[33mwifi  : 未設定。set wifi.ssid <名前> で設定\033[0m\r\n");
        return;
    }
    if (cdk_wifi_init() != ESP_OK) {
        term_printf("\033[31mwifi  : 初期化に失敗\033[0m\r\n");
        return;
    }

    /* **同じ SSID を繋ぎに行っている最中なら重ねない。** 待たなくなったぶん
     * `wifi connect` を続けて叩けてしまう。叩くたびに今の試行を捨てて
     * 最初からやり直すので、いつまでも繋がらない。
     *
     * **SSID を変えたときは通す。** 確立後の切断は期限なしで繋ぎ直し続ける
     * ので、AP が消えた場所では CONNECTING のまま留まる。そこで
     * `set wifi.ssid <別の AP>` → `wifi connect` を弾くと、再起動しないと
     * 別の AP へ移れない（取扱説明書 9 章の直し方がそのまま効かなくなる）。 */
    if (s_wifi_state == CDK_WIFI_CONNECTING &&
        strcmp(s_wifi_ssid, s_wifi_trying) == 0) {
        term_printf("wifi  : %s に接続中です\r\n", s_wifi_ssid);
        return;
    }

    /* すでに繋がっているなら「接続中...」を出さない。
     * 出してから即「接続」と続くと、繋ぎ直したように見える。 */
    if (cdk_wifi_ssid_is(s_wifi_ssid)) {
        char now[20];
        cdk_wifi_ip(now, sizeof(now));
        term_printf("wifi  : \033[32m既に %s に接続しています (%s)\033[0m\r\n",
                    s_wifi_ssid, now);
        return;
    }

    const esp_err_t err = cdk_wifi_connect_start(s_wifi_ssid, s_wifi_psk, 20000);
    if (err != ESP_OK) {
        term_printf("\033[31mwifi  : 開始できません %s\033[0m\r\n",
                    esp_err_to_name(err));
        return;
    }
    s_wifi_state = cdk_wifi_poll();
    strncpy(s_wifi_trying, s_wifi_ssid, sizeof(s_wifi_trying) - 1);
    s_wifi_trying[sizeof(s_wifi_trying) - 1] = '\0';
    term_printf("wifi  : %s に接続中...\r\n", s_wifi_ssid);
}

/* WiFi の状態が変わったらシェルへ 1 行返す。メインループから毎周呼ぶ。
 *
 * **シェルに居るときだけ書く。** SSH の向こうやエディタ、ユーザーアプリの
 * 画面に「wifi : 接続 ...」が割り込むと、そちらの表示が壊れる。
 * 出さなくても状態はステータス帯に出ている。 */
static void wifi_pump(void)
{
    const cdk_wifi_state_t now = cdk_wifi_poll();
    if (now == s_wifi_state) {
        return;
    }
    const cdk_wifi_state_t was = s_wifi_state;
    s_wifi_state = now;

    if (s_session != SESSION_SHELL) {
        return;
    }

    /* 切り詰められると UTF-8 が途中で切れる。いちばん長い「失敗」の
     * 行で 80 バイトほどなので、余裕を持たせておく。 */
    char msg[128];
    if (now == CDK_WIFI_CONNECTED) {
        char ip[20];
        cdk_wifi_ip(ip, sizeof(ip));
        snprintf(msg, sizeof(msg), "wifi  : \033[32m接続 %s\033[0m", ip);
    } else if (now == CDK_WIFI_FAILED) {
        /* 桁が狭いときは短く出す。**折り返すと 12 行しかない画面で
         * 1 行余計に食う。** skk の起動行と同じ理由・同じ分け方。
         * やり直し方は取扱説明書 9 章にある。 */
        if (s_term.cols < 60) {
            snprintf(msg, sizeof(msg), "\033[31mwifi  : 失敗 reason=%d\033[0m",
                     cdk_wifi_last_disconnect_reason());
        } else {
            snprintf(msg, sizeof(msg),
                     "\033[31mwifi  : 失敗 reason=%d（wifi connect でやり直せます）"
                     "\033[0m",
                     cdk_wifi_last_disconnect_reason());
        }
    } else if (now == CDK_WIFI_CONNECTING && was == CDK_WIFI_CONNECTED) {
        /* 繋がっていたのが切れた。放っておいても繋ぎ直すが、
         * 黙って落ちると「なぜか通らない」になる。 */
        snprintf(msg, sizeof(msg),
                 "\033[33mwifi  : 切れました。繋ぎ直しています\033[0m");
    } else {
        return;
    }
    /* シェルが止まっていれば何も出ない。s_dirty は shell_out が立てる。 */
    cdk_shell_notify(&s_shell, msg);
}

static void app_wifi_status(void)
{
    /* いまは cmd_wifi が直接 cdk_wifi_* を読むので何もしない。
     * 表示の体裁をコマンド側に閉じ込めておきたいのでフックだけ残す。 */
}

/* パスワードの出どころ。**どれを送ったかで、失敗したときの言い方も
 * 次にすることも変わる**ので、呼び側に返す。 */
typedef enum {
    PASS_NONE = 0, /* 無い。鍵だけで行く */
    PASS_NVS,      /* set ssh.pass で溜めたもの */
    PASS_ASKED     /* その場で打ってもらったもの */
} pass_src_t;

/* 溜めたパスワードを使ってよい相手かを決める（DESIGN.md §8.3）。
 *
 * **`ssh` と `scp` で 1 か所。** 分けると、片方だけが知らない機械へ平文の
 * パスワードを送るようになる。送ってしまったものは取り消せない。
 *
 * 呼ぶ前に load_config() すること。src には出どころが入る（PASS_NONE が
 * 「鍵だけで行く」。画面に出して、入れなかったときの誤診を防ぐため）。 */
static const char *pick_password(const char *user, const char *host,
                                 pass_src_t *src)
{
    /* **空は「無い」と同じ扱い。** 空を持っていることにすると「（鍵のみ）」
     * が出ず、`set ssh.pass ""` の相手でパスワードを聞く道にも入らない。 */
    if (s_ssh_pass[0] != '\0' && strcmp(user, s_ssh_user) == 0 &&
        strcmp(host, s_ssh_host) == 0) {
        *src = PASS_NVS;
        return s_ssh_pass;
    }
    /* その場で打ってもらったもの。**打った相手にしか送らない**のは
     * NVS のものと同じ規則（相手ごと覚えてある）。 */
    if (s_ask_pass[0] != '\0' && strcmp(user, s_ask_user) == 0 &&
        strcmp(host, s_ask_host) == 0) {
        *src = PASS_ASKED;
        return s_ask_pass;
    }
    /* **NULL は渡せない。** libssh2_userauth_password が strlen() に
     * 素通しするので落ちる。 */
    *src = PASS_NONE;
    return "";
}

/* 認証に要るものを組む。鍵の場所は cdk_ssh_key_path() が唯一の出どころ。 */
static cdk_ssh_login_t ssh_login(const char *user, const char *host,
                                 const char *pass)
{
    const cdk_ssh_login_t l = {
        .host = host,
        .port = 22,
        .user = user,
        .password = pass,
        /* 鍵が無ければ port 層が黙って飛ばす。あればパスワードより先に試す。 */
        .key_path = cdk_ssh_key_path(),
        .key_pass = NULL,
    };
    return l;
}

/* パスワードを聞く（下で定義。app_ssh_open / scp_connect と呼び合う）。 */
static void ask_password(const char *user, const char *host, cdk_shell_ask_fn cb);
static void ssh_password_entered(void *user, const char *answer);

/* いまの相手へ繋ぐ。相手は s_ssh_arg_* があればそちら、無ければ NVS。
 * **繋ぎ直しもここを通る**ので、引数で入ったセッションは同じ相手へ戻る。
 *
 * interactive は「人がいま `ssh` と打った」。**繋ぎ直し（`cdk_retry`）では
 * false。** 誰も見ていない画面でパスワードを聞いても答えは来ないし、
 * 聞いている間シェルが伏せ字のまま止まる。 */
static bool app_ssh_open(bool interactive)
{
    load_config();

    const char *user = s_ssh_user;
    const char *host = s_ssh_host;
    const char *pass = s_ssh_pass;
    pass_src_t  src = PASS_NONE;

    if (s_ssh_arg_host[0] != '\0') {
        user = s_ssh_arg_user;
        host = s_ssh_arg_host;
    }
    /* 溜めたパスワードを送ってよい相手かを決める。判定は pick_password に
     * 1 つだけ置いてある（`scp` と共有する。**規則を 2 か所に持たない**）。 */
    pass = pick_password(user, host, &src);

    if (host[0] == '\0' || user[0] == '\0') {
        term_printf("\033[31mssh   : 接続先が未設定 (set ssh.host / ssh.user)\033[0m\r\n");
        return false;
    }

    /* 鍵だけで行くことを言っておく。**黙って行くと、入れなかったときに
     * 「パスワードが違う」と思われる。** 桁が狭いので短く付ける。 */
    term_printf("ssh   : %s@%s に接続中%s...\r\n", user, host,
                (src == PASS_NONE) ? "（鍵のみ）" : "");

    /* PTY のサイズは端末の実値。最終ハードの 80x26 を決め打ちしない。 */
    const cdk_ssh_config_t cfg = {
        .login = ssh_login(user, host, pass),
        .term = "xterm-256color",
        .cols = s_term.cols,
        .rows = s_term.rows,
    };

    const esp_err_t err = cdk_ssh_connect(&cfg);
    if (err != ESP_OK) {
        /* 認証だけは生のコードで出さない。**`ssh user@host` は鍵だけで
         * 行くので、鍵が相手の authorized_keys に無い、がいちばん多い。**
         * ESP_ERR_NOT_ALLOWED と出されても次に何をすればいいか分からない。 */
        if (err == ESP_ERR_NOT_ALLOWED) {
            /* **打ってもらったものが通らなかったら忘れる。**
             * 残すと、次の `ssh` も同じ間違ったパスワードで黙って落ちて、
             * 打ち直す道が無くなる（`pick_password` が持っている限り
             * PASS_NONE にならないので、下の「聞く」にも入らない）。 */
            if (src == PASS_ASKED) {
                ssh_forget_asked();
            }
            if (src == PASS_NONE) {
                /* 鍵が相手の authorized_keys に無い、がいちばん多い。
                 * **ここで初めてパスワードを聞く。** 先に聞くと、鍵で
                 * 入れる相手にまで平文の合言葉を打たせることになる。 */
                if (interactive) {
                    term_printf("\033[31mssh   : 鍵では入れませんでした\033[0m\r\n");
                    ask_password(user, host, ssh_password_entered);
                    return false;
                }
                term_printf("\033[31mssh   : 認証に失敗 (ssh key で確認)\033[0m\r\n");
            } else {
                /* **パスワードを送ったのに `ssh key で確認` と出すと嘘になる。**
                 * 直すところが鍵なのか合言葉なのか分からなくなる。 */
                term_printf("\033[31mssh   : 認証に失敗"
                            "（鍵もパスワードも通りません）\033[0m\r\n");
            }
        } else if (err == ESP_ERR_NOT_FOUND) {
            /* 名前が引けないか、届かないか、22 番が閉じているか。
             * どれも「宛先を確かめる」で同じなのでまとめて出す。 */
            term_printf("\033[31mssh   : %s に届きません\033[0m\r\n", host);
        } else {
            term_printf("\033[31mssh   : 接続失敗 %s\033[0m\r\n",
                        esp_err_to_name(err));
        }
        return false;
    }
    /* **スタックの余裕を実測して残す。** 公開鍵認証を足したときに
     * メインタスクが溢れた（#40）。ここが libssh2 と mbedTLS の
     * ピークなので、数えるならこの直後。 */
    ESP_LOGI(TAG, "SSH 接続完了。メインタスクのスタック残り %u バイト",
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
    enter_ssh();
    return true;
}

/* 打ってもらったパスワードを、聞いた相手と一緒に覚える。
 * **打った相手にしか送らない**（§8.3 の規則をその場のパスワードにも
 * そのまま当てる）。`ssh` と `scp` で 1 か所。 */
static void remember_asked(const char *answer)
{
    strncpy(s_ask_user, s_ask_target_user, sizeof(s_ask_user) - 1);
    s_ask_user[sizeof(s_ask_user) - 1] = '\0';
    strncpy(s_ask_host, s_ask_target_host, sizeof(s_ask_host) - 1);
    s_ask_host[sizeof(s_ask_host) - 1] = '\0';
    strncpy(s_ask_pass, answer, sizeof(s_ask_pass) - 1);
    s_ask_pass[sizeof(s_ask_pass) - 1] = '\0';
}

/* パスワードが返ってきた。**ここから繋ぎ直す。**
 *
 * cdk_shell_key の中から呼ばれるので、そのまま繋いでよい（`ssh` コマンドが
 * 呼ばれるのと同じ場所・同じタスク）。 */
static void ssh_password_entered(void *user, const char *answer)
{
    (void)user;
    if (answer == NULL) {
        term_printf("\033[33mssh   : やめました\033[0m\r\n");
        return;
    }
    remember_asked(answer);

    /* **1 回だけ。** ここで失敗しても app_ssh_open が s_ask_* を捨てるので、
     * 聞き直しの無限ループにはならない（次に `ssh` と打てばまた聞く）。 */
    (void)app_ssh_open(true);
}

/* 答えは cb に返る（NULL ならやめた）。cb は remember_asked() してから
 * 繋ぎ直すこと。 */
static void ask_password(const char *user, const char *host, cdk_shell_ask_fn cb)
{
    strncpy(s_ask_target_user, user, sizeof(s_ask_target_user) - 1);
    s_ask_target_user[sizeof(s_ask_target_user) - 1] = '\0';
    strncpy(s_ask_target_host, host, sizeof(s_ask_target_host) - 1);
    s_ask_target_host[sizeof(s_ask_target_host) - 1] = '\0';

    /* **誰のパスワードを聞かれているのかを出す。** 打ち間違えた宛先に
     * 気づくのは、合言葉を打ってしまう前でなければ意味がない。 */
    snprintf(s_ask_prompt, sizeof(s_ask_prompt), "%s@%s のパスワード: ", user,
             host);
    /* 入れ物は s_ask_pass。**入りきらないものは切り詰めずに断る**
     * （cdk_shell_ask が「長すぎます」と出してやめる）。 */
    cdk_shell_ask(&s_shell, s_ask_prompt, (int)sizeof(s_ask_pass) - 1, cb, NULL);
}

/* シェルからの入口。user/host が NULL なら NVS の設定へ戻す。 */
static bool app_ssh_open_to(const char *user, const char *host)
{
    if (user != NULL && host != NULL) {
        strncpy(s_ssh_arg_user, user, sizeof(s_ssh_arg_user) - 1);
        s_ssh_arg_user[sizeof(s_ssh_arg_user) - 1] = '\0';
        strncpy(s_ssh_arg_host, host, sizeof(s_ssh_arg_host) - 1);
        s_ssh_arg_host[sizeof(s_ssh_arg_host) - 1] = '\0';
    } else {
        /* **引数なしの `ssh` だけが指定を消す。** 前に `ssh a@b` して
         * いても引きずらない。その場で打ってもらったパスワードも一緒に
         * 捨てる（宛先を戻したのに合言葉だけ残るのは分かりにくい）。 */
        s_ssh_arg_user[0] = '\0';
        s_ssh_arg_host[0] = '\0';
        ssh_forget_asked();
    }
    return app_ssh_open(true);
}

/* `scp` で頼まれた中身。**パスワードを聞いたあと、同じもので繋ぎ直す**
 * ために持つ。cmd_scp の入れ物（cdk_commands.c の static）を当てにしない
 * （向こうの都合で寿命が変わると、知らないうちに別の中身で繋ぎに行く）。
 * 大きさは cdk_scp_args_t と cmd_scp の s_scp_remote に合わせてある。 */
static bool s_scp_upload;
static char s_scp_local[192];
static char s_scp_remote[200];
static char s_scp_user[48];
static char s_scp_host[80];

static void scp_password_entered(void *user, const char *answer);

/* s_scp_* の中身で繋ぐ。**接続と認証だけ待って、転送はメインループに渡す。**
 *
 * 相手が NVS と同じなら溜めたパスワードを使い、違えば鍵だけで行く
 * （その場で打ってもらったものがあれば、それも使う）。判定は
 * `pick_password()` 1 か所（`ssh` と共有。**規則を 2 か所に持たない**。
 * DESIGN.md §8.3）。
 *
 * **鍵で断られたら聞く**のも `ssh` と同じ（§8.5）。認証はここで済ませて
 * から SESSION_SCP に入るので、聞くのはまだシェルにいるうち。 */
static bool scp_connect(void)
{
    load_config();

    const char *user = s_scp_user;
    const char *host = s_scp_host;
    pass_src_t  src = PASS_NONE;
    const char *pass = pick_password(user, host, &src);

    /* パスは **`cdk_wasm_path_resolve()`** を通す。`..` を畳んでから
     * `/sdcard` の下かを見る唯一の実装（`cdk_fscmds.c` の `resolve()` は
     * 畳まないので `/sdcard/../nvs` が通ってしまう）。
     * `cdk.py push` も同じ関数を通している。**規則を 2 か所に持たない。** */
    static char full[CDK_WASM_PATH_MAX];
    const int   pr = cdk_wasm_path_resolve(s_scp_local, (int)strlen(s_scp_local),
                                           full, (int)sizeof(full));
    if (pr != CDK_E_OK) {
        term_printf("\033[31mscp   : %s\033[0m\r\n",
                    (pr == CDK_E_PERM) ? "microSD の外は触れません"
                                       : "パスが読めません");
        return false;
    }

    term_printf("scp   : %s %s%s...\r\n", s_scp_upload ? "送ります" : "取ります",
                s_scp_remote, (src == PASS_NONE) ? "（鍵のみ）" : "");

    char                  why[128] = {0};
    const cdk_ssh_login_t login = ssh_login(user, host, pass);
    const esp_err_t       err =
        cdk_scp_start(s_scp_upload, full, s_scp_remote, &login, why, sizeof(why));
    if (err != ESP_OK) {
        if (err == ESP_ERR_NOT_ALLOWED) {
            /* **通らなかったものは忘れる**（`ssh` と同じ。残すと次の `scp` も
             * 同じ間違ったパスワードで黙って落ち、聞く道に入らない）。 */
            if (src == PASS_ASKED) {
                ssh_forget_asked();
            }
            if (src == PASS_NONE) {
                term_printf("\033[31mscp   : 鍵では入れませんでした\033[0m\r\n");
                ask_password(user, host, scp_password_entered);
                return false;
            }
        }
        term_printf("\033[31mscp   : %s\033[0m\r\n", why);
        return false;
    }
    enter_scp();
    return true;
}

/* パスワードが返ってきた。cdk_shell_key の中から呼ばれるので、そのまま
 * 繋いでよい（ssh_password_entered と同じ）。 */
static void scp_password_entered(void *user, const char *answer)
{
    (void)user;
    if (answer == NULL) {
        term_printf("\033[33mscp   : やめました\033[0m\r\n");
        return;
    }
    remember_asked(answer);

    /* **1 回だけ。** いまは PASS_ASKED で行くので、失敗しても聞き直さずに
     * 忘れて終わる（もう一度 `scp` と打てばまた聞く）。 */
    (void)scp_connect();
}

/* 入れ物に入らないものは切り詰めずに断る。途中で切れたパスで繋ぐと、
 * 頼んだのとは別のファイルを触る。 */
static bool scp_copy(char *dst, size_t cap, const char *src)
{
    const size_t n = strlen(src);
    if (n >= cap) {
        return false;
    }
    memcpy(dst, src, n + 1);
    return true;
}

/* `scp` の入口。
 *
 * **`s_ssh_arg_*` を触らない。** あれは `ssh` の繋ぎ直し先なので、
 * `scp` の相手で上書きすると、あとで切れた SSH が別の機械へ繋ぎ直す。 */
static bool app_scp_start(bool upload, const char *local, const char *remote,
                          const char *user, const char *host)
{
    s_scp_upload = upload;
    if (!scp_copy(s_scp_local, sizeof(s_scp_local), local) ||
        !scp_copy(s_scp_remote, sizeof(s_scp_remote), remote) ||
        !scp_copy(s_scp_user, sizeof(s_scp_user), user) ||
        !scp_copy(s_scp_host, sizeof(s_scp_host), host)) {
        term_printf("\033[31mscp   : 引数が長すぎます\033[0m\r\n");
        return false;
    }
    return scp_connect();
}

/* あるか（ファイルでもディレクトリでも）。 */
static bool sd_exists(const char *path)
{
    struct stat st;
    cdk_sd_lock();
    const int rc = stat(path, &st);
    cdk_sd_unlock();
    return rc == 0;
}

/* `wget`。**名前を決めるのはここ。** 繋いでヘッダを受け取るまで待ち、
 * そのあとはメインループが刻む。
 *
 *  - 名前を指定されたら、そのまま（あれば上書き。scp と同じ）
 *  - 省かれたら URL から作り（`cdk_shell_wget_name`）、同名があれば
 *    `.1` `.2` … に逃がす（本家 wget と同じ）。**`.tmp` も同名とみなす**
 *    （書きかけの隣に置くと、片方の後片付けがもう片方を消す）
 *
 * どちらもパスは **`cdk_wasm_path_resolve()`** を通す（**規則を 2 か所に
 * 持たない**）。 */
static bool app_wget_start(const char *url, const char *name)
{
    static char full[CDK_WASM_PATH_MAX];
    static char cand[CDK_WASM_PATH_MAX];
    const char *path = full;

    if (name != NULL) {
        const int pr =
            cdk_wasm_path_resolve(name, (int)strlen(name), full, (int)sizeof(full));
        if (pr != CDK_E_OK) {
            term_printf("\033[31mwget  : %s\033[0m\r\n",
                        (pr == CDK_E_PERM) ? "microSD の外は触れません"
                                           : "パスが読めません");
            return false;
        }
        struct stat st;
        cdk_sd_lock();
        const bool is_dir = (stat(full, &st) == 0) && S_ISDIR(st.st_mode);
        cdk_sd_unlock();
        if (is_dir) {
            term_printf("\033[31mwget  : %s はディレクトリです\033[0m\r\n", full);
            return false;
        }
    } else {
        /* `/sdcard/` と `.tmp` と `.99` が入る長さに抑える。 */
        char      base[CDK_WASM_PATH_MAX - 24];
        const int pr = cdk_shell_wget_name(url, base, sizeof(base))
                           ? cdk_wasm_path_resolve(base, (int)strlen(base), full,
                                                   (int)sizeof(full))
                           : CDK_E_INVAL;
        if (pr != CDK_E_OK) {
            term_printf("\033[31mwget  : 名前を作れません。名前を指定してください"
                        "\033[0m\r\n");
            return false;
        }
        for (int n = 0;; n++) {
            if (n == 0) {
                snprintf(cand, sizeof(cand), "%s", full);
            } else if (n > 99 || !cdk_shell_wget_numbered(full, n, cand, sizeof(cand))) {
                term_printf("\033[31mwget  : 同じ名前が多すぎます。名前を指定してください"
                            "\033[0m\r\n");
                return false;
            }
            char tmp[CDK_WASM_PATH_MAX + 8];
            snprintf(tmp, sizeof(tmp), "%s.tmp", cand);
            if (!sd_exists(cand) && !sd_exists(tmp)) {
                break;
            }
        }
        path = cand;
    }

    term_printf("wget  : %s\r\n", url);
    term_printf("wget  : -> %s\r\n", path);

    char            why[128] = {0};
    const esp_err_t err = cdk_wget_start(url, path, why, sizeof(why));
    if (err != ESP_OK) {
        term_printf("\033[31mwget  : %s\033[0m\r\n", why);
        return false;
    }
    enter_wget();
    return true;
}

static void app_ssh_close(void)
{
    cdk_ssh_close();
    if (s_session == SESSION_SSH) {
        enter_shell();
    }
}

static bool app_ssh_is_open(void)
{
    return cdk_ssh_is_open();
}

/* `ssh forget`。**人に貸す前に押せる口。**
 *
 * 電源を切れば消えるものだが、「消えているはず」を確かめる手段が無いのは
 * 落ち着かない。NVS の `ssh.pass` はここでは消さない（`set` の持ち物で、
 * 消したつもりが設定ごと消えていた、が起きるとつらい）。 */
static void app_ssh_forget(void)
{
    ssh_forget_asked();
}

/* 繋ぎ直すのをやめて電波を止める。**外へ持ち出したときのための口。**
 *
 * 一度でも繋がったあとに切れた場合は期限を持たずに繋ぎ直し続ける仕様
 * （DESIGN.md §8.1）。家で点けたまま外出すると、相手が二度と現れないのに
 * 探し続けて電池だけが減る。それを人が止める。 */
static void app_wifi_off(void)
{
    /* もう止まっているなら「止めました」と言わない。**黙って効かないより
     * 効かないと言うほうがいい**が、効いてもいないのに効いたと言うのは
     * どちらより悪い（SSID 未設定でここに来ることもある）。 */
    if (s_wifi_state == CDK_WIFI_IDLE && !cdk_wifi_is_connected()) {
        term_printf("wifi  : \033[33mもう止まっています\033[0m\r\n");
        return;
    }

    /* 後片付けを 2 つ。**どちらも今は通らない。**
     * `wifi off` はシェルからしか打てず、シェルに居るとき SSH は閉じて
     * いるし、キーは何であれ route_key が先に再接続を止めるため。
     * それでも置いてあるのは、`wifi off` を「止まった状態にする」命令
     * として単体で正しくしておきたいから。**片方でも残ると、止めたのに
     * 電波を使い続ける**（SSH は繋ぎ直しに行き、時計は WiFi を待つ）。 */
    if (cdk_ssh_is_open()) {
        /* こちらから閉じる (DISCONNECT_LOCAL)。通信断として扱われると
         * 再接続の時計が回り出す。 */
        app_ssh_close();
        term_printf("ssh   : 切断しました\r\n");
    }
    if (cdk_retry_active(&s_retry)) {
        cdk_retry_stop(&s_retry);
    }

    cdk_wifi_off();
    /* wifi_pump に「変わった」と思わせない。知らせはここで出している。 */
    s_wifi_state = cdk_wifi_poll();
    s_wifi_trying[0] = '\0';

    /* 桁が狭いときは短く出す。折り返すと 12 行しかない画面を 1 行余計に
     * 食う（失敗行と同じ分け方）。**戻し方は残す。**「止めました」だけだと
     * 戻せるものなのかが分からない。 */
    if (s_term.cols < 60) {
        term_printf("wifi  : \033[33m止めました (wifi connect)\033[0m\r\n");
    } else {
        term_printf("wifi  : \033[33m止めました（wifi connect で戻せます）"
                    "\033[0m\r\n");
    }
}

static void app_skk_set_mode(cdk_skk_mode_t m)
{
    cdk_skk_set_mode(&s_skk, m);
    cdk_screen_set_preedit(&s_screen, cdk_skk_preedit(&s_skk));
    s_dirty = true;
}

static cdk_skk_mode_t app_skk_get_mode(void)
{
    return cdk_skk_get_mode(&s_skk);
}

static const cdk_skk_dict_t *app_skk_dict(void)
{
    return &s_skkdict;
}

static bool app_edit_open(const char *path)
{
    if (s_edit_arena == NULL) {
        return false;
    }
    char full[CDK_EDIT_PATH_MAX];
    cdk_edit_resolve_path(path, full, sizeof(full));
    /* 読めなくても開く。理由はステータス行に出る。
     * 「開けません」で弾くと、新しいファイルが作れない。 */
    (void)cdk_edit_file_load(&s_edit, full);
    s_edit.mode_label = cdk_skk_mode_label(&s_skk);
    enter_edit();
    return true;
}

static bool app_wasm_open(const char *path, int argc, char **argv)
{
    char why[128] = {0};
    /* **argv はここで写される。** cdk_shell_split の argv は execute() の
     * スタックを指していて、コマンドが return した時点で無効になる。 */
    const esp_err_t err = cdk_wasm_run_start(path, argc, argv,
                                             s_term.cols, s_term.rows,
                                             why, sizeof(why));
    if (err != ESP_OK) {
        term_printf("\033[31mwasm  : %s\033[0m\r\n", why[0] ? why : esp_err_to_name(err));
        s_dirty = true;
        return false;
    }
    enter_wasm();
    return true;
}

static void app_reboot(void)
{
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

/* ------------------------------------------------------------------ */

void app_main(void)
{
    ESP_LOGI(TAG, "%s 起動中", CDK_VERSION);

    ESP_ERROR_CHECK(cdk_display_init(&s_disp));

    const size_t canvas_bytes = cdk_canvas_bytes(s_disp.width, s_disp.height);
    s_pixels = heap_caps_malloc(canvas_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_pixels == NULL || !cdk_canvas_init(&s_canvas, s_pixels, s_disp.width,
                                             s_disp.height)) {
        ESP_LOGE(TAG, "キャンバスを確保できません (%u バイト)", (unsigned)canvas_bytes);
        abort();
    }

    const size_t font_len = (size_t)(font_end - font_start);
    if (!cdk_font_open(&s_font, font_start, font_len)) {
        ESP_LOGE(TAG, "フォントブロブが壊れています (%u バイト)", (unsigned)font_len);
        abort();
    }
    ESP_LOGI(TAG, "フォント %u 字 セル %ux%u (%u バイト)", (unsigned)s_font.count,
             s_font.cell_w, s_font.cell_h, (unsigned)font_len);

    cdk_screen_init(&s_screen, &s_font);

    /* **端末を作る前に読む。** 拡大率で桁数と行数が変わるので、
     * cdk_term_init より後だと寸法が合わなくなる。ガンマや keymap と違って
     * その場では反映できず、再起動が要るのはこのため。 */
    {
        char buf[8];
        long v = 1;
        if (cdk_cfg_init() == ESP_OK &&
            cdk_cfg_get(CDK_CFG_FONTSCALE, buf, sizeof(buf)) == ESP_OK &&
            buf[0] != '\0') {
            v = strtol(buf, NULL, 10);
            if (v < 1 || v > 2) {
                ESP_LOGW(TAG, "fontscale=%s は 1 か 2 のみ。1 を使います", buf);
                v = 1;
            }
        }
        cdk_screen_set_scale(&s_screen, (uint8_t)v);

        /* ステータス帯も**端末を作る前に**決める。1 行使うので行数が変わる。 */
        char sb[8];
        if (cdk_cfg_get(CDK_CFG_STATUSBAR, sb, sizeof(sb)) == ESP_OK) {
            s_statusbar = (strcmp(sb, "on") == 0);
        }
    }

    cdk_canvas_clear(&s_canvas, s_screen.default_bg);

    const int cols = cdk_screen_cols(&s_screen, s_disp.width);
    const int all_rows = cdk_screen_rows(&s_screen, s_disp.height);

    /* **帯は端末グリッドの外。** 端末には 1 行少なく渡し、その 1 行ぶんの
     * 場所へ main が直接描く。§0 は「余りの 8px を帯に使う」と書いているが、
     * 8px に 12px のフォントは入らない。専用の小さいフォントを作る手もあるが、
     * 2 倍表示のとき帯だけ極小になって展示では読めない。1 行使えば
     * 拡大率にそのまま追随する（DESIGN.md の該当節に理由を書いた）。 */
    const int rows = s_statusbar ? (all_rows - 1) : all_rows;
    s_status_row = all_rows - 1;

    const size_t tmem = cdk_term_mem_size(cols, rows);
    s_termmem = heap_caps_malloc(tmem, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_termmem == NULL || !cdk_term_init(&s_term, cols, rows, s_termmem, tmem)) {
        ESP_LOGE(TAG, "端末を初期化できません (%u バイト)", (unsigned)tmem);
        abort();
    }
    cdk_term_set_reply(&s_term, on_term_reply, NULL);
    ESP_LOGI(TAG, "端末 %d桁 x %d行 (%u バイト)", cols, rows, (unsigned)tmem);

    /* スクロールバック。**内部 DRAM には置かない**（空きはあっても最大ブロックが
     * 30KB ほどしかない）。取れなくても起動は続ける。遡れなくなるだけ。 */
    {
        const size_t hmem = cdk_term_hist_mem_size(cols, SCROLLBACK_LINES);
        void *hp = heap_caps_malloc(hmem, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (hp != NULL && cdk_term_set_history(&s_term, hp, hmem, SCROLLBACK_LINES)) {
            ESP_LOGI(TAG, "スクロールバック %d 行 (%u バイト)",
                     SCROLLBACK_LINES, (unsigned)hmem);
        } else {
            free(hp);
            ESP_LOGW(TAG, "スクロールバックを確保できません (%u バイト)",
                     (unsigned)hmem);
        }
    }

    /* **端末を作ったあとで呼ぶ。** 中で s_term をダーティにするので、
     * cdk_term_init より前だと未初期化の領域を触ることになる。 */
    cdk_apply_gamma_setting();

#if CDK_BOARD_HAS_USB_HOST
    /* USB キーボード。挿さっていなくても失敗しない（接続は非同期）。 */
    {
        const esp_err_t err = cdk_usbhid_init();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "USB ホストを起動できません: %s", esp_err_to_name(err));
        }
    }
    cdk_apply_keymap_setting();
    cdk_apply_reconnect_setting();
#endif
    ESP_LOGI(TAG, "ガンマ補正 %u/10", (unsigned)s_screen.gamma_x10);

    draw_banner();
    draw_status(true);
    cdk_screen_draw(&s_screen, &s_canvas, &s_term, true);
    ESP_ERROR_CHECK(cdk_display_flush(&s_canvas));
    s_dirty = false;

    /* 入力の解釈。バイト列 → キーイベント → route_key。 */
    cdk_keys_decode_init(&s_keydec, route_key, NULL);

    /* シェル。コマンドは main/cdk_commands.c。 */
    cdk_shell_init(&s_shell, shell_out, NULL);
    /* 桁数を教えないと、画面幅を超えたコマンド行を描き直すたびに上の段が
     * 残っていく。 */
    cdk_shell_set_cols(&s_shell, s_term.cols);
    s_ctx = (app_ctx_t){
        .sh = &s_shell,
        .term = &s_term,
        .wifi_connect = app_wifi_connect,
        .wifi_status = app_wifi_status,
        .wifi_off = app_wifi_off,
        .ssh_open = app_ssh_open_to,
        .scp_start = app_scp_start,
        .wget_start = app_wget_start,
        .ssh_close = app_ssh_close,
        .ssh_is_open = app_ssh_is_open,
        .ssh_forget = app_ssh_forget,
        .skk_set_mode = app_skk_set_mode,
        .skk_get_mode = app_skk_get_mode,
        .skk_dict = app_skk_dict,
        .edit_open = app_edit_open,
        .wasm_open = app_wasm_open,
#if CDK_BOARD_HAS_EXT_UART
        .uart_open = app_uart_open,
#else
        /* この基板に外部 UART は無い。コマンド側が NULL を見て断る。 */
        .uart_open = NULL,
#endif
        .reboot = app_reboot,
    };
    /* コマンド表を繋いで渡す。help は登録された表をそのまま見る。
     * 溢れたぶんを黙って落とすと「コマンドが無い」としか見えないので、
     * 静的表明で先に止める。 */
#define ALL_CMDS_MAX 48
    _Static_assert(ALL_CMDS_MAX >= 32, "コマンド表が小さすぎる");
    static cdk_shell_cmd_t   all_cmds[ALL_CMDS_MAX];
    const cdk_shell_cmd_t   *tables[] = {APP_COMMANDS, APP_FS_COMMANDS,
                                         APP_SKK_COMMANDS, APP_EDIT_COMMANDS,
                                         APP_WASM_COMMANDS};
    const size_t             counts[] = {APP_COMMANDS_COUNT, APP_FS_COMMANDS_COUNT,
                                         APP_SKK_COMMANDS_COUNT,
                                         APP_EDIT_COMMANDS_COUNT,
                                         APP_WASM_COMMANDS_COUNT};
    /* 表を足すときは 2 か所ある。片方だけ足すと counts の外を読む。 */
    _Static_assert(sizeof(tables) / sizeof(tables[0])
                       == sizeof(counts) / sizeof(counts[0]),
                   "tables と counts の数が合っていない");
    size_t ncmds = 0;
    for (size_t t = 0; t < sizeof(tables) / sizeof(tables[0]); t++) {
        for (size_t i = 0; i < counts[t]; i++) {
            if (ncmds >= ALL_CMDS_MAX) {
                ESP_LOGE(TAG, "コマンド表が溢れました。ALL_CMDS_MAX を増やすこと");
                break;
            }
            all_cmds[ncmds++] = tables[t][i];
        }
    }
    cdk_shell_set_commands(&s_shell, all_cmds, ncmds, &s_ctx);
    ESP_LOGI(TAG, "コマンド %u 個", (unsigned)ncmds);

    cdk_link_set_handler(on_frame, NULL);
    ESP_ERROR_CHECK(cdk_link_init());
    (void)cdk_link_send(CDK_MSG_READY, CDK_VERSION, (uint32_t)strlen(CDK_VERSION));

    /* microSD。挿さっていなければ黙って諦める（後から `sd mount` で入れられる）。 */
    if (cdk_sd_mount() == ESP_OK) {
        char     name[24] = {0};
        uint64_t cap = 0;
        cdk_sd_info(name, sizeof(name), &cap, NULL);
        s_sd_mb = (uint32_t)(cap / (1024ULL * 1024ULL));
        term_printf("sd    : \033[32m%s %luMB\033[0m\r\n", name,
                    (unsigned long)s_sd_mb);
    } else {
        term_printf("\033[33msd    : 見つかりません\033[0m\r\n");
    }

    /* 日本語入力。辞書は SD が優先で、無ければフラッシュの skkdict。
     * SD のマウントより後に開くこと。順番を逆にすると、SD に辞書を
     * 置いてもフラッシュのほうが使われる。 */
    /* エディタの本文を置く領域。起動時に 1 回だけ確保する（DESIGN.md §14）。 */
    s_edit_arena = (char *)heap_caps_malloc(EDIT_ARENA_BYTES, MALLOC_CAP_SPIRAM);
    if (s_edit_arena != NULL) {
        cdk_edit_init(&s_edit, s_edit_arena, EDIT_ARENA_BYTES, shell_out, NULL);
        cdk_edit_set_size(&s_edit, cols, rows);
    } else {
        ESP_LOGE(TAG, "エディタの領域を確保できません");
    }

    /* ユーザーアプリ (.wasm) を置く領域。ここも起動時に 1 回だけ。
     * 取れなくても起動は続ける（`wasm run` だけが使えなくなる）。 */
    if (cdk_wasm_run_init() != ESP_OK) {
        term_printf("\033[33mwasm  : 置き場を確保できません\033[0m\r\n");
    }

    /* 転送用の領域。起動時に 1 回だけ（DESIGN.md §14）。
     * 取れなくても scp が使えないだけなので、続ける。 */
    if (cdk_scp_init() != ESP_OK) {
        term_printf("\033[33mscp   : 置き場を確保できません（scp は使えません）"
                    "\033[0m\r\n");
    }
    if (cdk_wget_init() != ESP_OK) {
        term_printf("\033[33mwget  : 置き場を確保できません（wget は使えません）"
                    "\033[0m\r\n");
    }

    cdk_skk_init(&s_skk, skk_commit, NULL);
    cdk_skk_user_init(&s_skkuser, s_skkuser_store,
                      (int)(sizeof(s_skkuser_store) / sizeof(s_skkuser_store[0])));
    const int learned = cdk_skkuser_attach(&s_skkuser);
    cdk_skk_set_user(&s_skk, &s_skkuser);

    if (cdk_skkdict_open(&s_skkdict) == ESP_OK) {
        cdk_skk_set_dict(&s_skk, &s_skkdict);
        /* 桁が狭いときは短く出す。**折り返すと 13 行しかない画面で
         * 1 行余計に食い、バナーが上へ流れる。** */
        if (s_term.cols < 60) {
            term_printf("skk   : \033[32m%lu 語\033[0m 学習 %d\r\n",
                        (unsigned long)(s_skkdict.ari_count + s_skkdict.nasi_count),
                        learned);
        } else {
            term_printf("skk   : \033[32m送りあり%lu 送りなし%lu\033[0m 学習 %d 件\r\n",
                        (unsigned long)s_skkdict.ari_count,
                        (unsigned long)s_skkdict.nasi_count, learned);
        }
    } else {
        term_printf("\033[33mskk   : 辞書がありません\033[0m\r\n");
    }

    /* 起動時に WiFi を繋ぎに行っておく。**待たない**（DESIGN.md §8.1）。
     * SSH は `ssh` コマンドで入る。
     * 自動で SSH に入るとシェルに触れないまま起動が終わってしまう。 */
    app_wifi_connect();

    ESP_LOGI(TAG, "空きヒープ internal=%u spiram=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "%s %s [%s] %dx%d", CDK_READY_SENTINEL, CDK_VERSION,
             cdk_board_name(), cols, rows);

    term_printf("\r\nhelp でコマンド一覧\r\n");
    enter_shell();

    while (true) {
        cdk_link_poll(5);

#if CDK_BOARD_HAS_USB_HOST
        /* USB キーボード。**route_key はここ（メインループ）からしか呼ばない。**
         * 端末・画面・SKK・SSH を触るので、USB のタスクから直接呼ぶと壊れる。 */
        cdk_usbhid_poll(route_key, NULL, now_ms());
#endif

        /* SSH の受信をそのまま端末へ流す。DESIGN.md §8.2 の通り
         * 専用タスクは作らず、メインループが両方を見る。 */
        if (s_session == SESSION_SSH) {
            if (!cdk_ssh_is_open()) {
                const cdk_ssh_disconnect_t why = cdk_ssh_disconnect_reason();
                /* **通信断のときだけ繋ぎ直す。** リモートで exit したのに
                 * 繋ぎ直されると、抜ける手段が無くなる。 */
                if (s_reconnect_enabled && why == CDK_SSH_DISCONNECT_ERROR) {
                    term_printf("\r\n\033[33mssh   : 切断されました。"
                                "繋ぎ直します（どれかキーで中止）\033[0m\r\n");
                    cdk_retry_init(&s_retry, RECONNECT_BASE_MS, RECONNECT_CAP_MS,
                                   RECONNECT_TRIES);
                    cdk_retry_begin(&s_retry, now_ms());
                } else {
                    term_printf("\r\n\033[31mssh   : 切断されました\033[0m\r\n");
                }
                enter_shell();
            } else {
                uint8_t rx[512];
                for (int i = 0; i < 8; i++) { /* 1 周で汲みすぎない */
                    const int n = cdk_ssh_read(rx, sizeof(rx));
                    if (n > 0) {
                        cdk_term_feed(&s_term, rx, (size_t)n);
                        s_dirty = true;
                        continue;
                    }
                    break;
                }
            }
        }

#if CDK_BOARD_HAS_EXT_UART
        /* 外部 UART の受信をそのまま端末へ。SSH と同じで専用タスクは
         * 作らない。`cdk_uart_read` は元から待たない（timeout 0）。 */
        if (s_session == SESSION_UART) {
            uint8_t rx[512];
            for (int i = 0; i < 8; i++) { /* 1 周で汲みすぎない */
                const int n = cdk_uart_read(rx, sizeof(rx));
                if (n > 0) {
                    cdk_term_feed(&s_term, rx, (size_t)n);
                    s_dirty = true;
                    continue;
                }
                break;
            }
        }
#endif

        /* ユーザーアプリ。**別タスクで走っているので、ここは汲むだけ。**
         * 端末とキャンバスを触るのはメインループだけ、という約束を守る。 */
        if (s_session == SESSION_WASM) {
            char wbuf[256];
            char report[160];
            bool done = false;
            for (int i = 0; i < 8; i++) { /* 1 周で汲みすぎない */
                const int n = cdk_wasm_run_poll(wbuf, (int)sizeof(wbuf), &done,
                                                report, sizeof(report));
                if (n > 0) {
                    cdk_term_feed(&s_term, (const uint8_t *)wbuf, (size_t)n);
                    s_dirty = true;
                    continue;
                }
                break;
            }
            if (done) {
                term_printf("%s", report);
                leave_wasm();
            }
        }

        /* 再接続。**待っている間もシェルは普通に使える。**
         * 画面を占有して待たせると、設定を直すこともできない。 */
        if (cdk_retry_active(&s_retry) && cdk_retry_due(&s_retry, now_ms())) {
            /* **待っている間はシェルが出ている。** 知らせ（と app_ssh_open
             * が出すもの）をプロンプトの後ろに続けず、終わったらプロンプトを
             * 出し直す。繋がったら enter_ssh がシェルを止めるので、show は
             * 何もしない。 */
            cdk_shell_hide_line(&s_shell);
            term_printf("\033[33mssh   : 再接続 %u 回目...\033[0m\r\n",
                        (unsigned)cdk_retry_next_attempt(&s_retry));
            /* **繋ぎ直しでは聞かない**（interactive = false）。人が見て
             * いるとは限らないし、聞いてもシェルが伏せ字のまま止まる。
             * その場で打ってもらったパスワードは相手ごと覚えてあるので、
             * 同じ相手への繋ぎ直しはそれで通る。 */
            if (cdk_wifi_is_connected() && app_ssh_open(false)) {
                cdk_retry_stop(&s_retry);
            } else {
                cdk_retry_failed(&s_retry, now_ms());
                if (cdk_retry_exhausted(&s_retry)) {
                    term_printf("\033[31mssh   : 繋ぎ直せませんでした。"
                                "`ssh` でやり直せます\033[0m\r\n");
                }
            }
            cdk_shell_show_line(&s_shell);
        }

        /* ファイル転送。**別タスクではなくここで刻む。** 1 周で動かす量は
         * port_scp が決めているので、メインループを止めない。 */
        if (s_session == SESSION_SCP) {
            bool done = false;
            char report[192];
            cdk_scp_poll(&done, report, sizeof(report));
            if (done) {
                term_printf("%s", report);
                leave_scp();
            } else {
                scp_progress_line();
            }
        }
        if (s_session == SESSION_WGET) {
            bool done = false;
            char report[192];
            cdk_wget_poll(&done, report, sizeof(report));
            if (done) {
                term_printf("%s", report);
                leave_wget();
            } else {
                wget_progress_line();
            }
        }

        /* WiFi。**待たずに繋ぎに行っている**ので、結果はここで受け取る。 */
        wifi_pump();

        /* ステータス帯。内容が変わったときだけ描き直す。 */
        draw_status(false);

        if (s_dirty) {
            /* 端末のダーティ行だけをキャンバスへ、
             * キャンバスのダーティ行帯だけをパネルへ送る。 */
            cdk_screen_draw(&s_screen, &s_canvas, &s_term, false);
            (void)cdk_display_flush(&s_canvas);
            s_dirty = false;
        }
    }
}
