/* Real USB window gate: one reader, busy refusal and bounded cancel request. */
#include "suite_translation_usb.h"
#include "suite_translation_usb_proto.h"

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

static int64_t now_ms;
static unsigned created;
static bool audio_held;
static TaskHandle_t created_handle;

bool suite_audio_job_held(void) { return audio_held; }
suite_translation_apply_t suite_translation_config_apply(const char *origin,
                                                        const char *token, bool *changed)
{
    (void)origin; (void)token;
    if (changed) *changed = false;
    return SUITE_TRANSLATION_APPLY_OK;
}
suite_translation_usb_run_t suite_translation_usb_run(
    const suite_translation_usb_transport_t *transport,
    suite_translation_usb_apply_fn apply, void *apply_ctx,
    bool (*cancelled)(void *ctx), void *cancel_ctx, uint32_t window_ms)
{
    (void)transport; (void)apply; (void)apply_ctx;
    (void)cancelled; (void)cancel_ctx; (void)window_ms;
    return SUITE_TRANSLATION_USB_RUN_COMPLETED;
}
esp_err_t usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *config)
{
    assert(config->rx_buffer_size > 64);
    return ESP_OK;
}
esp_err_t usb_serial_jtag_driver_uninstall(void) { return ESP_OK; }
int usb_serial_jtag_read_bytes(void *buf, uint32_t length, TickType_t ticks)
{ (void)buf; (void)length; (void)ticks; return 0; }
int usb_serial_jtag_write_bytes(const void *src, size_t size, TickType_t ticks)
{ (void)src; (void)size; (void)ticks; return 0; }
esp_err_t usb_serial_jtag_wait_tx_done(TickType_t ticks) { (void)ticks; return ESP_OK; }
void usb_serial_jtag_vfs_use_driver(void) {}
void usb_serial_jtag_vfs_use_nonblocking(void) {}
int64_t esp_timer_get_time(void) { return ++now_ms * 1000; }
BaseType_t xTaskCreate(TaskFunction_t entry, const char *name, unsigned stack,
                       void *arg, unsigned priority, TaskHandle_t *task)
{
    (void)entry; (void)name; (void)arg;
    assert(stack >= 4096 && priority > 0);
    created++;
    created_handle = (TaskHandle_t)1;
    *task = created_handle;
    return pdPASS;
}
void vTaskDelete(TaskHandle_t task) { (void)task; }
void xTaskNotifyGive(TaskHandle_t task) { (void)task; }
void test_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
#define ESP_LOGE(...) test_log(__VA_ARGS__)
#define ESP_LOGW(...) test_log(__VA_ARGS__)
#define ESP_LOGI(...) test_log(__VA_ARGS__)

int main(void)
{
    audio_held = true;
    assert(!suite_translation_usb_open_window());
    assert(created == 0 && !suite_translation_usb_window_open());

    audio_held = false;
    assert(suite_translation_usb_open_window());
    assert(created == 1 && suite_translation_usb_window_open());
    assert(!suite_translation_usb_open_window()); /* exactly one reader */
    assert(created == 1);
    suite_translation_usb_cancel();
    assert(suite_translation_usb_window_open()); /* worker owns teardown */

    puts("USB window busy gate / single reader / cancel request: PASS");
    return 0;
}
