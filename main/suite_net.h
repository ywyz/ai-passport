#pragma once
#include <stdbool.h>
#include "esp_err.h"
/* Serialize TLS owners on this no-PSRAM board. Never take under the UI lock. */
esp_err_t suite_net_init(void);
bool suite_net_take(unsigned timeout_ms);
void suite_net_give(void);
