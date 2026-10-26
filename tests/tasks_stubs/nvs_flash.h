#pragma once
#include "esp_partition.h"
typedef struct { uint8_t eky[32],tky[32]; } nvs_sec_cfg_t;
esp_err_t nvs_flash_read_security_cfg(const esp_partition_t *,nvs_sec_cfg_t *);
esp_err_t nvs_flash_secure_init_partition(const char *,nvs_sec_cfg_t *);
#define ESP_ERR_NVS_NO_FREE_PAGES 0x110d
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1110
esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_erase(void);
