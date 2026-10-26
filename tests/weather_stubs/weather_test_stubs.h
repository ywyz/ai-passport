#pragma once

// 天气任务主机测试的最小平台边界:只声明 main/suite_weather.c 实际用到的
// ESP-IDF / FreeRTOS 子集。行为(可恢复任务、可控信号量、脚本化 HTTP
// 客户端)在 tests/weather_stubs/weather_runtime.c。
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_TIMEOUT 0x107
const char *esp_err_to_name(esp_err_t error);

void test_log(const char *tag, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
#define ESP_LOGE(...) test_log(__VA_ARGS__)
#define ESP_LOGW(...) test_log(__VA_ARGS__)
#define ESP_LOGI(...) test_log(__VA_ARGS__)

// FreeRTOS 子集:任务句柄与信号量句柄都指向测试可控的桩对象。
typedef int BaseType_t;
typedef unsigned TickType_t;
/* Critical-section subset used by bounded diagnostics. The host test owns an
   external depth counter so scheduler preemption can be modeled explicitly. */
typedef struct { int unused; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { 0 }
void test_critical_enter(void);
void test_critical_exit(void);
#define portENTER_CRITICAL(mux) ((void)(mux),test_critical_enter())
#define portEXIT_CRITICAL(mux) ((void)(mux),test_critical_exit())
typedef struct test_task *TaskHandle_t;
typedef struct test_sem *SemaphoreHandle_t;
typedef void (*TaskFunction_t)(void *);
#define pdTRUE 1
#define pdPASS 1
#define pdFALSE 0
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
BaseType_t xTaskCreate(TaskFunction_t entry, const char *name, unsigned stack,
                       void *arg, unsigned priority, TaskHandle_t *task);
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks);
void xTaskNotifyGive(TaskHandle_t task);
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem);
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks);
void vSemaphoreDelete(SemaphoreHandle_t sem);

// 堆诊断:只要求返回确定值,行为断言不依赖具体数值。
#define MALLOC_CAP_INTERNAL (1 << 1)
#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_DMA (1 << 3)
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
uint32_t esp_get_free_heap_size(void);

// esp_http_client 子集:字段名与实现里使用的成员保持一致。
typedef enum {
    HTTP_EVENT_ERROR = 0,
    HTTP_EVENT_ON_CONNECTED,
    HTTP_EVENT_HEADERS_SENT,
    HTTP_EVENT_ON_HEADER,
    HTTP_EVENT_ON_DATA,
    HTTP_EVENT_ON_FINISH,
} esp_http_client_event_id_t;

typedef struct esp_http_client_event {
    esp_http_client_event_id_t event_id;
    void *data;
    int data_len;
    void *user_data;
} esp_http_client_event_t;

typedef esp_err_t (*http_event_handle_cb)(esp_http_client_event_t *evt);

typedef struct {
    const char *url;
    http_event_handle_cb event_handler;
    esp_err_t (*crt_bundle_attach)(void *conf);
    int timeout_ms;
    int buffer_size;
    int buffer_size_tx;
    bool disable_auto_redirect;
    void *user_data;
} esp_http_client_config_t;

typedef struct esp_http_client *esp_http_client_handle_t;

esp_err_t esp_crt_bundle_attach(void *conf);
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_perform(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
void esp_http_client_cleanup(esp_http_client_handle_t client);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client,const char *key,const char *value);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client);
esp_err_t esp_http_client_open(esp_http_client_handle_t client,int length);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client);
int esp_http_client_read(esp_http_client_handle_t client,char *data,int length);
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t client,int timeout);
#define HTTP_METHOD_POST 1
esp_err_t esp_http_client_set_method(esp_http_client_handle_t client,int method);
int esp_http_client_write(esp_http_client_handle_t client,const char *data,int length);
