#pragma once
#include "weather_test_stubs.h"
esp_err_t bsp_audio_init(void);
esp_err_t bsp_audio_set_format(uint32_t,uint8_t,uint8_t);
esp_err_t bsp_audio_read(void *,size_t);
esp_err_t bsp_audio_release(void);
