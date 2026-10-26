#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t suite_net_init(void);
bool suite_net_take(unsigned timeout_ms);
void suite_net_give(void);
