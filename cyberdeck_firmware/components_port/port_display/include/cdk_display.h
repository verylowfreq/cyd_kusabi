/* cdk_display — キャンバスを液晶パネルへ出す薄いポート層。
 *
 * 上位（cdk_term / cdk_canvas）は BSP も esp_lcd も知らない。
 * 最終ハードへ移るときはこのファイルの実装だけを差し替える。
 */
#pragma once

#include "cdk_canvas.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int width;
    int height;
} cdk_display_info_t;

/* パネルの電源投入・初期化・バックライト点灯までを行う。
 * CoreS3 では LCD 電源が AXP2101 / AW9523 経由なので BSP に任せる。 */
esp_err_t cdk_display_init(cdk_display_info_t *info);

/* キャンバスのダーティ行だけをパネルへ転送し、ダーティを消す。
 * 転送が不要なら何もせず ESP_OK を返す。 */
esp_err_t cdk_display_flush(cdk_canvas_t *canvas);

/* 0-100 [%] */
esp_err_t cdk_display_set_brightness(int percent);

#ifdef __cplusplus
}
#endif
