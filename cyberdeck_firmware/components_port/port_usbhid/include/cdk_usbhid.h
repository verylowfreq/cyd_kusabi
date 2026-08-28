/* cdk_usbhid — USB キーボードをキーイベントにする（最終ハードのみ）
 *
 * DESIGN.md §7.3。ESP32-S3 の USB PHY は 1 組しかないので、
 * USB をホストに使う基板では USB-Serial-JTAG が無い（HARDWARE_DESIGN.md F-1）。
 * したがってこれが有効なのは v1 だけ。
 *
 * **キーは USB のタスクから直接配らない。** route_key は端末・画面・SKK・SSH を
 * 触るので、メインループ以外から呼ぶと壊れる。USB 側はキューに積むだけにして、
 * `cdk_usbhid_poll()` がメインループの文脈で取り出す。
 */
#ifndef CDK_USBHID_H
#define CDK_USBHID_H

#include "cdk_board.h"
#include "cdk_keys.h"
#include "esp_err.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* USB ホストを立ち上げる。VBUS を入れてキーボードの接続を待つ。
 * 接続は非同期なので、この時点で刺さっていなくてもよい。 */
esp_err_t cdk_usbhid_init(void);

/* たまったキーイベントを取り出して fn に渡す。メインループから呼ぶ。
 * now_ms は連続押しの判定に使う。戻り値は配ったキーの数。 */
int cdk_usbhid_poll(cdk_key_fn fn, void *user, uint32_t now_ms);

/* キーボードが今つながっているか。起動バナーや status 表示用。 */
bool cdk_usbhid_connected(void);

/* キー配列を切り替える（DESIGN.md §7.4）。`set keymap us|jis` から呼ぶ。
 *
 * **どちらかに決め打ちできない。** 同じキーの usage id は配列によらず同じで、
 * 刻印だけが違う。挿さっているのがどちらかは、HID の情報からは分からない
 * （espressif/usb_host_hid は HID ディスクリプタの bCountryCode を外に出さない）。
 * JIS にしか無いキーが押されれば分かるが、それまでは分からない。 */
void cdk_usbhid_set_layout(bool jis);
bool cdk_usbhid_layout_is_jis(void);

#ifdef __cplusplus
}
#endif

#endif /* CDK_USBHID_H */
