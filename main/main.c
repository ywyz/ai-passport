// main/main.c —— FoloToy AI Passport 五合一应用入口。
// 启动链:BSP 显示 → 应用外壳 → 连接基础(Wi-Fi/SNTP/天气) → 首页(三体时钟)。
// 硬件初始化沿用 BSP;失败路径参考原基线 demo(见 git 历史/演示分支)。
#include "bsp_battery.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "lvgl.h"

#include "suite_app.h"
#include "suite_connection.h"
#include "suite_home.h"
#include "suite_prov_ble.h"
#include "suite_storage.h"
#include "suite_weather.h"
#include "suite_tasks.h"
#include "suite_net.h"

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

    // NimBLE 配网 host 启动时常驻,但只在"手机配网"页打开时才广播。
    // 必须在 Wi-Fi 静态缓冲等大块分配之前初始化:BT 传输池需约 30KB 内部
    // 连续堆,ESP32-C3 无 PSRAM,晚分配会因内存吃紧导致 nimble_port_init
    // 失败(hci inits failed)。NVS 先行:BT 控制器使能时要加载 PHY 校准数据。
    // 非致命:失败时配网页会提示,其它功能不受影响。
    (void)suite_storage_init();
    ESP_LOGI(TAG, "BT 初始化前:空闲堆 %lu,最大连续内部块 %lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (suite_prov_ble_init() != ESP_OK) {
        ESP_LOGW(TAG, "BLE 配网服务初始化失败,手机配网不可用");
    }

    if (suite_app_input_init() != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败,应用无法交互");
        return;
    }

    if (suite_connection_start() != ESP_OK) {
        ESP_LOGW(TAG, "连接管理启动失败,首页将显示离线");
    }
    (void)suite_net_init();
    (void)suite_weather_start();
    (void)suite_tasks_start();

    if (!suite_app_start(&SUITE_PAGE_HOME)) {
        ESP_LOGE(TAG, "首页进入失败");
        return;
    }

    ESP_LOGI(TAG, "应用就绪");
}
