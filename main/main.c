// main/main.c —— FoloToy AI Passport 行程应用入口(feature/passport-core D1)。
//
// 开机直接进入 travel UI(强制 UI 重设计要求:不经过基线测试菜单,不复用
// ui_pixel 外壳)。基线 demo 仍在仓库内,只作为独立的硬件测试参考,不再是
// 应用外壳。
//
// 按键语义(全局统一,由 travel 状态机与 passport_keys 驱动):
//   上/下 短按   列表移动(边界停止;快速长按进入加速重复)
//   确定  短按   进入/执行
//   确定  长按   打开 P04 动作菜单(发布 PRD)
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "esp_sleep.h"

#include "travel_app.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "main";

void app_main(void) {
    // 行程应用启动:D1 之后开机即为 travel UI。基线硬件测试菜单保留在仓库
    // 内作为独立参考,不再通过启动路径进入。
    ESP_LOGI(TAG, "FoloToy AI Passport travel app (feature/passport-core D1)");
    ESP_LOGI(TAG, "wakeup cause %d", esp_sleep_get_wakeup_cause());

    bsp_i2c_init();

    if (bsp_display_init() != ESP_OK || bsp_lvgl_init() == NULL) {
        ESP_LOGE(TAG, "display/LVGL init failed; travel cannot start "
                      "(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    if (travel_app_start() != ESP_OK) {
        ESP_LOGE(TAG, "travel app start failed");
        return;
    }
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
