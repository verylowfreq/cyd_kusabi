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
#include "cdk_proto.h"
#include "cdk_screen.h"
#include "cdk_shell.h"
#include "cdk_skk.h"
#include "cdk_skkdict_src.h"
#include "cdk_retry.h"
#include "cdk_status.h"
#include "cdk_storage.h"
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

#include "secrets.h"

static const char *TAG = "cyberdeck";

/* tools/cdk.py がこの行を見て「起動完了」と判断する。 */
#define CDK_READY_SENTINEL "#CDK-READY"
#define CDK_VERSION        "cyberdeck M6"

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
    SESSION_WASM
} session_t;

static session_t s_session = SESSION_SHELL;

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
        term_printf("\033[33mssh   : 再接続をやめました\033[0m\r\n");
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

    if (s_session == SESSION_SSH && cdk_ssh_is_open()) {
        uint8_t out[16];
        const size_t n = cdk_keys_encode(k, s_term.app_cursor_keys, out, sizeof(out));
        if (n > 0) {
            (void)cdk_ssh_write(out, n);
        }
        return;
    }
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

/* 端末に 1 行出して即座に画面へ反映する。起動中の進捗を実機で見せるため。 */
static void term_printf(const char *fmt, ...)
{
    char buf[192];
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
    st.sd_ok = cdk_sd_mounted();
    st.sd_mb = s_sd_mb;
    st.ime = cdk_skk_mode_label(&s_skk);
    st.kbd = cdk_usbhid_connected();
    st.ssh_up = cdk_ssh_is_open();
    st.scroll_back = cdk_term_view_offset(&s_term);

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

    /* すでに繋がっているなら「接続中...」を出さない。
     * 出してから即「接続」と続くと、繋ぎ直したように見える。 */
    if (cdk_wifi_ssid_is(s_wifi_ssid)) {
        char now[20];
        cdk_wifi_ip(now, sizeof(now));
        term_printf("wifi  : \033[32m既に %s に接続しています (%s)\033[0m\r\n",
                    s_wifi_ssid, now);
        return;
    }

    term_printf("wifi  : %s に接続中...\r\n", s_wifi_ssid);

    const esp_err_t err = cdk_wifi_connect(s_wifi_ssid, s_wifi_psk, 20000);
    if (err != ESP_OK) {
        term_printf("\033[31mwifi  : 失敗 %s reason=%d\033[0m\r\n",
                    esp_err_to_name(err), cdk_wifi_last_disconnect_reason());
        return;
    }

    char ip[20];
    cdk_wifi_ip(ip, sizeof(ip));
    term_printf("wifi  : \033[32m接続 %s\033[0m\r\n", ip);
}

static void app_wifi_status(void)
{
    /* いまは cmd_wifi が直接 cdk_wifi_* を読むので何もしない。
     * 表示の体裁をコマンド側に閉じ込めておきたいのでフックだけ残す。 */
}

static bool app_ssh_open(void)
{
    load_config();

    if (s_ssh_host[0] == '\0' || s_ssh_user[0] == '\0') {
        term_printf("\033[31mssh   : 接続先が未設定 (set ssh.host / ssh.user)\033[0m\r\n");
        return false;
    }

    term_printf("ssh   : %s@%s に接続中...\r\n", s_ssh_user, s_ssh_host);

    /* PTY のサイズは端末の実値。最終ハードの 80x26 を決め打ちしない。 */
    const cdk_ssh_config_t cfg = {
        .host = s_ssh_host,
        .port = 22,
        .user = s_ssh_user,
        .password = s_ssh_pass,
        .term = "xterm-256color",
        .cols = s_term.cols,
        .rows = s_term.rows,
        /* 鍵が無ければ port 層が黙って飛ばす。あればパスワードより先に試す。 */
        .key_path = cdk_ssh_key_path(),
        .key_pass = NULL,
    };

    const esp_err_t err = cdk_ssh_connect(&cfg);
    if (err != ESP_OK) {
        term_printf("\033[31mssh   : 接続失敗 %s\033[0m\r\n", esp_err_to_name(err));
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
    s_ctx = (app_ctx_t){
        .sh = &s_shell,
        .term = &s_term,
        .wifi_connect = app_wifi_connect,
        .wifi_status = app_wifi_status,
        .ssh_open = app_ssh_open,
        .ssh_close = app_ssh_close,
        .ssh_is_open = app_ssh_is_open,
        .skk_set_mode = app_skk_set_mode,
        .skk_get_mode = app_skk_get_mode,
        .skk_dict = app_skk_dict,
        .edit_open = app_edit_open,
        .wasm_open = app_wasm_open,
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

    /* 起動時に WiFi まで繋いでおく。SSH は `ssh` コマンドで入る。
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
            term_printf("\033[33mssh   : 再接続 %u 回目...\033[0m\r\n",
                        (unsigned)cdk_retry_next_attempt(&s_retry));
            if (cdk_wifi_is_connected() && app_ssh_open()) {
                cdk_retry_stop(&s_retry);
            } else {
                cdk_retry_failed(&s_retry, now_ms());
                if (cdk_retry_exhausted(&s_retry)) {
                    term_printf("\033[31mssh   : 繋ぎ直せませんでした。"
                                "`ssh` でやり直せます\033[0m\r\n");
                }
            }
        }

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
