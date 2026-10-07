idf_component_register(
    SRCS "main.c"
         "suite_app.c"
         "suite_pages.c"
         "suite_input.c"
         "suite_clock.c"
         "suite_conn_fsm.c"
         "suite_connection.c"
         "suite_storage.c"
         "suite_weather.c"
         "suite_weather_logic.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_event esp_netif esp_timer esp_wifi esp_http_client lwip mbedtls nvs_flash
)
# 基线演示页(demo_*.c / ui_pixel*.c)不编入本应用;文件保留供主机测试与参考。
