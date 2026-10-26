#include "suite_net.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static SemaphoreHandle_t s_mutex;
esp_err_t suite_net_init(void)
{
    if (!s_mutex) s_mutex=xSemaphoreCreateMutex();
    return s_mutex ? ESP_OK : ESP_ERR_NO_MEM;
}
bool suite_net_take(unsigned ms)
{ return s_mutex && xSemaphoreTake(s_mutex,pdMS_TO_TICKS(ms))==pdTRUE; }
void suite_net_give(void) { if (s_mutex) xSemaphoreGive(s_mutex); }
