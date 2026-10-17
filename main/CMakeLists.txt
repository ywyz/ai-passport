idf_component_register(
    SRCS "suite_prov_proto.c" "suite_prov_ble.c" "main.c"
         "suite_app.c"
         "suite_pages.c"
         "suite_input.c"
         "suite_clock.c"
         "suite_conn_fsm.c"
         "suite_connection.c"
         "suite_storage.c"
         "suite_city_data.c"
         "suite_city_picker.c"
         "suite_settings.c"
         "suite_provision.c"
         "suite_provision_qr.c"
         "suite_weather.c"
         "suite_weather_logic.c"
         "suite_scene.c"
         "suite_home.c"
         "suite_launcher.c"
         "fonts/suite_font_16.c"
         "fonts/suite_font_clock.c"
    INCLUDE_DIRS "." "fonts"
    REQUIRES bsp app_update esp_event esp_netif esp_timer esp_wifi esp_http_client lwip mbedtls nvs_flash json bt
)
# 字体 C 文件按 LV_LVGL_H_INCLUDE_SIMPLE 包含 lvgl.h(managed component 布局)。
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
# 基线演示页(demo_*.c / ui_pixel*.c)不编入本应用;文件保留供主机测试与参考。
