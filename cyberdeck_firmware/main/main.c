/* Cyberdeck ファームウェア — M1
 *
 * M0: ビルド基盤 / ホスト側テスト / LCD 表示 / スクショ転送 / 有限時間のログ取得
 * M1: VT パーサ + 東雲 12px フォント + テキストコンソール  ← いまここ
 *
 * まだシェルも SSH も無いので、制御チャネルから来た TEXT/KEY を
 * そのままターミナルに流し込む（ローカルエコー）。
 * これで「バイト列 → 画面」の経路が実機で丸ごと検証できる。
 */
#include "cdk_canvas.h"
#include "cdk_display.h"
#include "cdk_font.h"
#include "cdk_keys.h"
#include "cdk_link.h"
#include "cdk_net.h"
#include "cdk_proto.h"
#include "cdk_screen.h"
#include "cdk_term.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "cyberdeck";

/* tools/cdk.py がこの行を見て「起動完了」と判断する。 */
#define CDK_READY_SENTINEL "#CDK-READY"
#define CDK_VERSION        "cyberdeck M1"

/* フォントはフラッシュに焼き込む。SD もファイルシステムも要らない。 */
extern const uint8_t font_start[] asm("_binary_shinonome12_cdkfont_start");
extern const uint8_t font_end[] asm("_binary_shinonome12_cdkfont_end");

/* 起動バナー。host/fixtures/vt/banner.vt と同一のバイト列。
 * ホスト側の cdk_render vt が同じファイルを描くので、
 * 実機のスクリーンショットと 1 バイト違わず比較できる。 */
extern const uint8_t banner_start[] asm("_binary_banner_vt_start");
extern const uint8_t banner_end[] asm("_binary_banner_vt_end");

static cdk_display_info_t s_disp;
static cdk_canvas_t       s_canvas;
static uint16_t          *s_pixels;
static cdk_font_t         s_font;
static cdk_screen_t       s_screen;
static cdk_term_t         s_term;
static void              *s_termmem;
static bool               s_dirty; /* 再描画が要るか */

static void send_screenshot(void)
{
    const cdk_wire_shot_info_t info = {
        .width = (uint16_t)s_canvas.w,
        .height = (uint16_t)s_canvas.h,
        .format = CDK_PIXFMT_RGB565_LE,
        .reserved = {0, 0, 0},
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

/* 端末が返す応答 (DA / DSR)。SSH が繋がるまでは行き先が無いのでログに出す。 */
static void on_term_reply(void *user, const uint8_t *data, size_t len)
{
    (void)user;
    ESP_LOGI(TAG, "端末からの応答 %u バイト", (unsigned)len);
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
        cdk_term_feed(&s_term, payload, len);
        s_dirty = true;
        break;

    case CDK_MSG_KEY:
        if (len >= sizeof(cdk_wire_key_t)) {
            cdk_wire_key_t w;
            memcpy(&w, payload, sizeof(w));
            const cdk_key_t k = {w.keycode, w.mods, w.codepoint};
            uint8_t out[16];
            const size_t n = cdk_keys_encode(&k, s_term.app_cursor_keys, out, sizeof(out));
            if (n > 0) {
                /* M1 はローカルエコー。M2 で SSH のチャネルへ送る。 */
                cdk_term_feed(&s_term, out, n);
                s_dirty = true;
            }
        }
        break;

    default:
        ESP_LOGW(TAG, "未知のフレーム type=0x%02X len=%u", type, (unsigned)len);
        break;
    }
}

static void draw_banner(void)
{
    cdk_term_feed(&s_term, banner_start, (size_t)(banner_end - banner_start));
    s_dirty = true;
}

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
    cdk_canvas_clear(&s_canvas, s_screen.default_bg);

    const int cols = cdk_screen_cols(&s_screen, s_disp.width);
    const int rows = cdk_screen_rows(&s_screen, s_disp.height);
    const size_t tmem = cdk_term_mem_size(cols, rows);
    s_termmem = heap_caps_malloc(tmem, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_termmem == NULL || !cdk_term_init(&s_term, cols, rows, s_termmem, tmem)) {
        ESP_LOGE(TAG, "端末を初期化できません (%u バイト)", (unsigned)tmem);
        abort();
    }
    cdk_term_set_reply(&s_term, on_term_reply, NULL);
    ESP_LOGI(TAG, "端末 %d桁 x %d行 (%u バイト)", cols, rows, (unsigned)tmem);

    draw_banner();
    cdk_screen_draw(&s_screen, &s_canvas, &s_term, true);
    ESP_ERROR_CHECK(cdk_display_flush(&s_canvas));
    s_dirty = false;

    /* M2: libssh2 が実機で生きているかを起動時に一度だけ確かめる。 */
    (void)cdk_net_probe();

    cdk_link_set_handler(on_frame, NULL);
    ESP_ERROR_CHECK(cdk_link_init());
    (void)cdk_link_send(CDK_MSG_READY, CDK_VERSION, (uint32_t)strlen(CDK_VERSION));

    ESP_LOGI(TAG, "空きヒープ internal=%u spiram=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "%s %s %dx%d", CDK_READY_SENTINEL, CDK_VERSION, cols, rows);

    while (true) {
        cdk_link_poll(20);

        if (s_dirty) {
            /* 端末のダーティ行だけをキャンバスへ、
             * キャンバスのダーティ行帯だけをパネルへ送る。 */
            cdk_screen_draw(&s_screen, &s_canvas, &s_term, false);
            (void)cdk_display_flush(&s_canvas);
            s_dirty = false;
        }
    }
}
