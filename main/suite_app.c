// main/suite_app.c —— 应用外壳实现。
#include "suite_app.h"

#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <string.h>

static const char *TAG = "suite_app";

#define SUITE_INPUT_QUEUE_DEPTH 8
#define SUITE_INPUT_TASK_STACK 4096

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t ev;
} raw_key_t;

static const suite_page_t *s_stack[SUITE_PAGE_STACK_MAX];
static int s_depth;
static suite_input_reducer_t s_reducer;
static QueueHandle_t s_key_queue;
static TaskHandle_t s_input_task;
static volatile bool s_ready;

const suite_page_t *suite_app_current(void)
{
    return s_depth > 0 ? s_stack[s_depth - 1] : NULL;
}

bool suite_app_push(const suite_page_t *page)
{
    if (!page || !page->enter || !page->exit) return false;
    if (s_depth >= SUITE_PAGE_STACK_MAX) {
        ESP_LOGW(TAG, "页面栈已满,拒绝进入 %s", page->name);
        return false;
    }
    if (!bsp_lvgl_lock(500)) return false;
    const suite_page_t *cur = suite_app_current();
    if (cur) cur->exit();
    s_stack[s_depth++] = page;
    page->enter();
    bsp_lvgl_unlock();
    return true;
}

bool suite_app_back(void)
{
    if (s_depth <= 1) return false;
    if (!bsp_lvgl_lock(500)) return false;
    s_stack[--s_depth]->exit();
    const suite_page_t *cur = suite_app_current();
    cur->enter();
    bsp_lvgl_unlock();
    return true;
}

static void route_action(suite_action_t act)
{
    if (act == SUITE_ACTION_NONE) return;
    if (act == SUITE_ACTION_BACK) {
        // 栈底页收到 BACK 不动作(首页不做退出)。
        (void)suite_app_back();
        return;
    }
    const suite_page_t *cur = suite_app_current();
    if (cur && cur->action) cur->action(act);
}

static void input_task(void *arg)
{
    (void)arg;
    raw_key_t key;
    for (;;) {
        if (xQueueReceive(s_key_queue, &key, portMAX_DELAY) == pdTRUE) {
            if (!s_ready) continue;  // 首页就绪前的输入丢弃,防迟到回调
            suite_btn_t btn = (suite_btn_t)key.btn;
            suite_key_ev_t ev = (suite_key_ev_t)key.ev;
            route_action(suite_input_reduce(&s_reducer, btn, ev));
        }
    }
}

// 按键回调运行在共享 esp_timer 任务:只入队立即返回。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (!s_key_queue) return;
    raw_key_t key = { .btn = btn, .ev = ev };
    (void)xQueueSend(s_key_queue, &key, 0);
}

esp_err_t suite_app_input_init(void)
{
    s_key_queue = xQueueCreate(SUITE_INPUT_QUEUE_DEPTH, sizeof(raw_key_t));
    if (!s_key_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(input_task, "suite_input", SUITE_INPUT_TASK_STACK, NULL, 5,
                    &s_input_task) != pdPASS) {
        vQueueDelete(s_key_queue);
        s_key_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return bsp_button_init(on_key, NULL);
}

bool suite_app_start(const suite_page_t *boot_page)
{
    suite_input_reducer_init(&s_reducer);
    s_depth = 0;
    bool ok = suite_app_push(boot_page);
    s_ready = ok;
    if (!ok) ESP_LOGE(TAG, "首页 %s 进入失败", boot_page ? boot_page->name : "?");
    return ok;
}
