// main/main.c —— FoloToy AI Passport 五合一应用入口(干线)。
// 干线阶段:应用外壳 + 连接基础(Wi-Fi/SNTP/天气客户端) + 状态页。
// 硬件初始化沿用 BSP;失败路径参考原基线 demo(见 git 历史/演示分支)。
#include "bsp_battery.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "esp_log.h"
#include "lvgl.h"

#include "suite_app.h"
#include "suite_connection.h"
#include "suite_pages.h"
#include "suite_weather.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "FoloToy AI Passport 五合一应用启动(干线)");

    bsp_i2c_init();

    // 屏幕失败则应用无法继续:打日志后返回,不做串口降级。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,应用无法继续");
        return;
    }
    bsp_display_backlight(100);

    // 电池与连接均为增强项:失败不阻塞外壳与状态页。
    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "电池计初始化失败,电量显示不可用");
    }

    if (suite_app_input_init() != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败,应用无法交互");
        return;
    }

    if (suite_connection_start() != ESP_OK) {
        ESP_LOGW(TAG, "连接管理启动失败,状态页将显示离线");
    }
    (void)suite_weather_start();

    if (!suite_app_start(&SUITE_PAGE_STATUS)) {
        ESP_LOGE(TAG, "首页进入失败");
        return;
    }

    ESP_LOGI(TAG, "应用就绪");
}
