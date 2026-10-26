#pragma once
#include "weather_test_stubs.h"
typedef struct { uint32_t size; } esp_partition_t;
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS 4
esp_err_t esp_partition_read(const esp_partition_t *,uint32_t,void *,size_t);
esp_err_t esp_partition_write(const esp_partition_t *,uint32_t,const void *,size_t);
esp_err_t esp_partition_erase_range(const esp_partition_t *,uint32_t,size_t);
const esp_partition_t *esp_partition_find_first(int,int,const char *);
