/*
 * travel_app.c — travel application shell (D1 subset).
 *
 * Concurrency map:
 *  - LVGL task (owned by BSP): the only task touching lv objects, or while
 *    `bsp_lvgl_lock()` is held from other tasks.
 *  - key task: polls the ADC-ladder button values with the bsp button API,
 *    feeds the pure state machine in passport_keys, forwards LVGL events.
 *    Callbacks stay non-blocking; slow work only in workers (AGENTS.md).
 *  - worker task: provisioning/sync/pack download; communicates results to
 *    the state model and lets LVGL redraw.
 *
 * Teardown (AGENTS.md): travel_app_stop() ends tasks before page deletion.
 */
#include "travel_app.h"

#include <string.h>

#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "passport_keys.h"
#include "travel_state.h"
#include "travel_ui.h"

static const char *TAG = "travel";

#define KEY_TASK_STACK 4096
#define WORKER_STACK 6144
#define KEY_POLL_MS 10

static struct passport_key_state s_key_states[PASSPORT_BTN_COUNT];
static TaskHandle_t s_key_task;
static TaskHandle_t s_worker_task;
static volatile bool s_running;
static struct travel_state s_state;
static struct travel_ui s_ui;
static uint32_t s_key_tick;
static char s_last_button_event[3];
static volatile bool s_key_dirty[3];
static volatile bool s_started;

static void keys_dispatch_later(char event, int button)
{
    if (button < 0 || button >= 3)
        return;
    s_last_button_event[button] = event;
    s_key_dirty[button] = true;
    (void)event;
}

static void apply_keys_in_lvgl(void)
{
    for (int button = 0; button < 3; ++button) {
        if (!s_key_dirty[button])
            continue;
        s_key_dirty[button] = false;
        char event = s_last_button_event[button];
        if (travel_state_key(&s_state, event, button)) {
            travel_ui_render(&s_ui);
        }
    }
}

static const struct {
    int min_mv;
    int max_mv;
} s_btn_windows[BSP_BTN_COUNT] = BSP_BTN_MV_TABLE;

static void key_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    while (s_running) {
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() - last) * (portTICK_PERIOD_MS);
        int mv = bsp_button_read_mv();
        for (int button = 0; button < BSP_BTN_COUNT; ++button) {
            bool down = mv >= 0 && mv < 3300 &&
                        mv >= s_btn_windows[button].min_mv &&
                        mv < s_btn_windows[button].max_mv;
            char event = passport_key_feed(&s_key_states[button], button,
                                           down, now_ms);
            if (event != 0)
                keys_dispatch_later(event, button);
        }
        if (bsp_lvgl_lock(50)) {
            apply_keys_in_lvgl();
            bsp_lvgl_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(KEY_POLL_MS));
    }
    s_key_task = NULL;
    vTaskDelete(NULL);
}

static void worker_task(void *arg)
{
    (void)arg;
    while (s_running) {
        /* D1c: provisioning/sync worker loop lives here; the pure parts
         * (event journal, manifest checks) run as unit-tested modules. */
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    s_worker_task = NULL;
    vTaskDelete(NULL);
}

int travel_app_start(void)
{
    if (s_started)
        return 0;
    s_running = true;
    if (bsp_button_init(NULL, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "bsp button init failed");
        return -1;
    }
    travel_state_init(&s_state);
        if (bsp_lvgl_lock(1000)) {
            if (travel_ui_init(&s_ui, &s_state) != 0) {
                ESP_LOGE(TAG, "travel ui init failed");
                return -1;
            }
            bsp_lvgl_unlock();
        } else {
            return -1;
        }
    if (xTaskCreate(key_task, "trv-key", KEY_TASK_STACK, NULL, 5, &s_key_task)
            != pdPASS) {
        ESP_LOGE(TAG, "key task failed");
        return -1;
    }
    if (xTaskCreate(worker_task, "trv-worker", WORKER_STACK, NULL, 4,
                    &s_worker_task) != pdPASS) {
        ESP_LOGE(TAG, "worker task failed");
        return -1;
    }
    s_started = true;
    return 0;
}

void travel_app_stop(void)
{
    if (!s_started)
        return;
    s_running = false;
    while (s_key_task || s_worker_task) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (bsp_lvgl_lock(1000)) {
        travel_ui_deinit(&s_ui);
        bsp_lvgl_unlock();
    }
    s_started = false;
}

bool travel_app_active(void)
{
    return s_started;
}
