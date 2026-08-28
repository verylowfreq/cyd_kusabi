#include "cdk_usbhid.h"

#if CDK_BOARD_HAS_USB_HOST

#include "cdk_hid.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/hid_host.h"
#include "usb/usb_host.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "cdk_usbhid";

/* 溜められるレポートの数。人間の打鍵より十分速く捌けるので、
 * ここが溢れるのは描画が長く止まったときだけ。 */
#define REP_QUEUE_LEN 16

/* 最初の何回かだけ INFO で出す。立ち上げの証拠は要るが、
 * 打鍵のたびに永久に出し続ける意味はない。以降は DEBUG。
 *
 * **数えるのはキーが乗っているレポートだけ。** 離したときの全ゼロも
 * 数えると枠が半分になり、しかも情報量はゼロ。実際、最初の確認で
 * 目当ての打鍵が枠を使い切ったあとに来て、写真を撮る羽目になった。 */
#define LOUD_REPORTS 40

typedef struct {
    uint8_t  data[8];
    uint32_t ms;
} rep_item_t;

static QueueHandle_t s_reps;
static volatile bool s_connected;

/* **cdk_hid の状態はメインループだけが触る。** USB 側はレポートをキューに
 * 積むだけ。キーの組み立てを USB タスクでやると、オートリピート用の時計や
 * CapsLock の状態を 2 つのタスクから触ることになる。 */
static cdk_hid_t s_hid;
static uint32_t  s_seen;

/* ---- USB ホストライブラリのイベントを回すタスク ---------------------- */

static void usb_lib_task(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if ((flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) != 0u) {
            usb_host_device_free_all();
        }
    }
}

/* ---- HID インタフェースのイベント ------------------------------------ */

static void iface_cb(hid_host_device_handle_t dev, const hid_host_interface_event_t event,
                     void *arg)
{
    (void)arg;

    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
        uint8_t data[16];
        size_t  len = 0;
        if (hid_host_device_get_raw_input_report_data(dev, data, sizeof(data), &len) !=
            ESP_OK) {
            return;
        }
        if (len < 8u) {
            return;
        }

        rep_item_t it;
        memcpy(it.data, data, 8);
        it.ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (xQueueSend(s_reps, &it, 0) != pdTRUE) {
            ESP_LOGW(TAG, "レポートを取りこぼしました");
        }
        break;
    }

    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "キーボードが外れました");
        s_connected = false;
        (void)hid_host_device_close(dev);
        break;

    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        ESP_LOGW(TAG, "転送エラー");
        break;

    default:
        break;
    }
}

/* ---- デバイスが挿さったとき ------------------------------------------ */

static void driver_cb(hid_host_device_handle_t dev, const hid_host_driver_event_t event,
                      void *arg)
{
    (void)arg;
    if (event != HID_HOST_DRIVER_EVENT_CONNECTED) {
        return;
    }

    hid_host_dev_params_t p;
    if (hid_host_device_get_params(dev, &p) != ESP_OK) {
        return;
    }
    ESP_LOGI(TAG, "HID 接続 addr=%u iface=%u subclass=%u proto=%u", p.addr, p.iface_num,
             p.sub_class, p.proto);

    if (p.proto != HID_PROTOCOL_KEYBOARD) {
        ESP_LOGI(TAG, "キーボードではないので使いません");
        return;
    }

    const hid_host_device_config_t cfg = {.callback = iface_cb, .callback_arg = NULL};
    if (hid_host_device_open(dev, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "device_open に失敗");
        return;
    }

    /* ブートプロトコルを要求する。8 バイト固定という前提をここで確定させる。
     *
     * **失敗しても続ける。** 実機の HHKB は SET_PROTOCOL がタイムアウトするが、
     * それでも 8 バイトのブート形式で送ってくる。ここで諦めると、
     * 「制御転送だけ通らないキーボード」が全部使えなくなる。
     * 実際に届いたレポートの長さで判断すればよい（8 未満は捨てている）。 */
    if (p.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
        const esp_err_t err =
            hid_class_request_set_protocol(dev, HID_REPORT_PROTOCOL_BOOT);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SET_PROTOCOL(boot) に失敗: %s。8 バイトで来ることを期待して続行",
                     esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "ブートインタフェースではありません (subclass=%u)", p.sub_class);
    }

    hid_host_dev_info_t info;
    if (hid_host_get_device_info(dev, &info) == ESP_OK) {
        ESP_LOGI(TAG, "VID=%04X PID=%04X product=%ls", info.VID, info.PID, info.iProduct);
    }

    if (hid_host_device_start(dev) != ESP_OK) {
        ESP_LOGE(TAG, "device_start に失敗");
        return;
    }
    s_connected = true;
    ESP_LOGI(TAG, "キーボードが使えます (配列=%s)",
             s_hid.layout == CDK_HID_LAYOUT_JIS ? "JIS" : "US");
}

/* ---- 公開 API --------------------------------------------------------- */

esp_err_t cdk_usbhid_init(void)
{
    if (s_reps != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_reps = xQueueCreate(REP_QUEUE_LEN, sizeof(rep_item_t));
    if (s_reps == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cdk_hid_init(&s_hid, CDK_HID_LAYOUT_US);

    /* VBUS を入れる。忘れるとキーボードに電気が行かず、
     * 「刺さっているのに何も起きない」という切り分けにくい状態になる。 */
    const gpio_config_t vbus = {
        .pin_bit_mask = 1ULL << CDK_BOARD_USB_VBUS_EN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&vbus), TAG, "vbus gpio");
    ESP_RETURN_ON_ERROR(gpio_set_level(CDK_BOARD_USB_VBUS_EN, 1), TAG, "vbus on");
    ESP_LOGI(TAG, "VBUS を入れました (G%d)", CDK_BOARD_USB_VBUS_EN);

    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_RETURN_ON_ERROR(usb_host_install(&host_cfg), TAG, "usb_host_install");

    if (xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    const hid_host_driver_config_t hid_cfg = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = driver_cb,
        .callback_arg = NULL,
    };
    ESP_RETURN_ON_ERROR(hid_host_install(&hid_cfg), TAG, "hid_host_install");

    ESP_LOGI(TAG, "USB ホスト準備完了。キーボードの接続を待ちます");
    return ESP_OK;
}

/* 立ち上げ中の診断。生のレポートと、そこから作ったキーを 1 行に並べる。
 * **押した文字が違う**ときに、バスの問題なのか配列表の問題なのかを
 * 1 回の打鍵で切り分けられるようにするため。 */
static void log_report(const cdk_hid_t *h, const rep_item_t *it)
{
    char decoded[64];
    int  pos = 0;
    decoded[0] = '\0';

    for (int i = 2; i < 8 && pos < (int)sizeof(decoded) - 16; i++) {
        const uint8_t u = it->data[i];
        if (u == 0u) {
            continue;
        }
        cdk_key_t k;
        if (cdk_hid_map(h, u, it->data[0], &k)) {
            if (k.codepoint >= 0x20u && k.codepoint < 0x7Fu) {
                pos += snprintf(decoded + pos, sizeof(decoded) - (size_t)pos, " '%c'",
                                (char)k.codepoint);
            } else {
                pos += snprintf(decoded + pos, sizeof(decoded) - (size_t)pos,
                                " key=0x%02X cp=U+%04X", k.keycode, (unsigned)k.codepoint);
            }
        } else {
            pos += snprintf(decoded + pos, sizeof(decoded) - (size_t)pos, " (0x%02X?)", u);
        }
    }

    /* ESP_LOG* の書式は**リテラルでないと通らない**（マクロが文字列連結する）。
     * 変数に持たせられないので 2 回書く。 */
    if (s_seen < LOUD_REPORTS) {
        ESP_LOGI(TAG, "report %02X | %02X %02X %02X %02X %02X %02X ->%s", it->data[0],
                 it->data[2], it->data[3], it->data[4], it->data[5], it->data[6],
                 it->data[7], decoded);
    } else {
        ESP_LOGD(TAG, "report %02X | %02X %02X %02X %02X %02X %02X ->%s", it->data[0],
                 it->data[2], it->data[3], it->data[4], it->data[5], it->data[6],
                 it->data[7], decoded);
    }
}

int cdk_usbhid_poll(cdk_key_fn fn, void *user, uint32_t now_ms)
{
    if (s_reps == NULL || fn == NULL) {
        return 0;
    }

    rep_item_t it;
    while (xQueueReceive(s_reps, &it, 0) == pdTRUE) {
        bool has_key = false;
        for (int i = 2; i < 8; i++) {
            if (it.data[i] != 0u) {
                has_key = true;
                break;
            }
        }
        if (has_key) {
            log_report(&s_hid, &it);
            s_seen++;
        }

        /* JIS にしか無いキーが来たら、配列の取り違えを知らせる。
         * 勝手に切り替えると `set keymap` で明示した設定を裏切るので、
         * 言うだけにしておく。 */
        for (int i = 2; i < 8; i++) {
            if (cdk_hid_usage_is_jis_only(it.data[i]) &&
                s_hid.layout != CDK_HID_LAYOUT_JIS) {
                ESP_LOGW(TAG, "JIS にしか無いキー (0x%02X) が来ました。"
                              "`set keymap jis` を試してください",
                         it.data[i]);
                break;
            }
        }

        cdk_hid_report(&s_hid, it.data, sizeof(it.data), it.ms, fn, user);
    }

    /* 押しっぱなしの繰り返し。**レポートではなくここで時間を進める。** */
    cdk_hid_tick(&s_hid, now_ms, fn, user);
    return 0;
}

bool cdk_usbhid_connected(void)
{
    return s_connected;
}

void cdk_usbhid_set_layout(bool jis)
{
    cdk_hid_set_layout(&s_hid, jis ? CDK_HID_LAYOUT_JIS : CDK_HID_LAYOUT_US);
}

bool cdk_usbhid_layout_is_jis(void)
{
    return s_hid.layout == CDK_HID_LAYOUT_JIS;
}

#else /* CDK_BOARD_HAS_USB_HOST */

esp_err_t cdk_usbhid_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

int cdk_usbhid_poll(cdk_key_fn fn, void *user, uint32_t now_ms)
{
    (void)fn;
    (void)user;
    (void)now_ms;
    return 0;
}

bool cdk_usbhid_connected(void)
{
    return false;
}

void cdk_usbhid_set_layout(bool jis)
{
    (void)jis;
}

bool cdk_usbhid_layout_is_jis(void)
{
    return false;
}

#endif /* CDK_BOARD_HAS_USB_HOST */
