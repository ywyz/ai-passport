#include "suite_translation_usb.h"

#include "suite_audio_job.h"
#include "suite_translation_usb_proto.h"

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdatomic.h>
#include <string.h>

static const char *TAG = "translation_usb";

#define WINDOW_STACK 4096U
#define WINDOW_PRIORITY 3U
#define DRIVER_RX_BYTES 512U
#define DRIVER_TX_BYTES 512U

static atomic_bool s_open, s_cancel;
static TaskHandle_t s_task;
static bool (*s_busy_check)(void);

static int driver_read(void *ctx, void *buffer, size_t capacity, uint32_t timeout_ms)
{
    (void)ctx;
    return usb_serial_jtag_read_bytes(buffer, (uint32_t)capacity, pdMS_TO_TICKS(timeout_ms));
}

static int driver_write(void *ctx, const void *buffer, size_t bytes, uint32_t timeout_ms)
{
    (void)ctx;
    return usb_serial_jtag_write_bytes(buffer, bytes, pdMS_TO_TICKS(timeout_ms));
}

static int64_t driver_now(void *ctx)
{
    (void)ctx;
    return esp_timer_get_time() / 1000;
}

static bool window_cancelled(void *ctx)
{
    (void)ctx;
    return atomic_load(&s_cancel);
}

static suite_translation_apply_t apply_config(const char *origin, const char *token, void *ctx)
{
    (void)ctx;
    bool changed = false;
    return suite_translation_config_apply(origin, token, &changed);
}

static void window_task(void *arg)
{
    (void)arg;
    usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    config.rx_buffer_size = DRIVER_RX_BYTES;
    config.tx_buffer_size = DRIVER_TX_BYTES;
    esp_err_t installed = usb_serial_jtag_driver_install(&config);
    if (installed == ESP_OK) {
        /* Keep normal ESP_LOG output working while this window owns a reader. */
        usb_serial_jtag_vfs_use_driver();
        suite_translation_usb_transport_t transport = {
            .read = driver_read, .write = driver_write, .now_ms = driver_now, .ctx = NULL};
        suite_translation_usb_run_t result = suite_translation_usb_run(
            &transport, apply_config, NULL, window_cancelled, NULL,
            SUITE_TRANSLATION_USB_WINDOW_DEFAULT_MS);
        (void)usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(200));
        usb_serial_jtag_vfs_use_nonblocking();
        (void)usb_serial_jtag_driver_uninstall();
        ESP_LOGI(TAG, "Provisioning window closed result=%u", (unsigned)result);
    } else {
        ESP_LOGE(TAG, "USB Serial/JTAG driver installation failed");
    }
    atomic_store(&s_open, false);
    s_task = NULL;
    vTaskDelete(NULL);
}

bool suite_translation_usb_open_window(void)
{
    if (atomic_load(&s_open) || s_task) return false;
    if (suite_audio_job_held() || (s_busy_check && s_busy_check())) return false;
    atomic_store(&s_cancel, false);
    atomic_store(&s_open, true);
    if (xTaskCreate(window_task, "translation_usb", WINDOW_STACK, NULL,
                    WINDOW_PRIORITY, &s_task) != pdPASS) {
        s_task = NULL;
        atomic_store(&s_open, false);
        return false;
    }
    return true;
}

void suite_translation_usb_cancel(void)
{
    atomic_store(&s_cancel, true);
    if (s_task) xTaskNotifyGive(s_task);
}

bool suite_translation_usb_window_open(void)
{
    return atomic_load(&s_open);
}

void suite_translation_usb_set_busy_check(bool (*busy)(void))
{
    s_busy_check = busy;
}
