#pragma once

void test_log(const char *tag, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

#define ESP_LOGE(...) test_log(__VA_ARGS__)
#define ESP_LOGW(...) test_log(__VA_ARGS__)
#define ESP_LOGI(...) test_log(__VA_ARGS__)
