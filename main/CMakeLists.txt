idf_component_register(
    SRCS "main.c"
         "demo_navigation.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "demo_display.c"
         "demo_button.c"
         "demo_audio.c"
         "demo_battery.c"
         "demo_radio.c"
         "demo_wifi.c"
         "demo_ble.c"
         "demo_low_power.c"
         "passport/passport_keys.c"
         "passport/passport_json.c"
         "passport/passport_json_decodeutf8.c"
         "passport/passport_sha256.c"
         "passport/passport_pack.c"
         "passport/passport_events.c"
         "passport/travel_state.c"
         "passport/travel_ui.c"
         "passport/travel_cfg.c"
         "passport/travel_prov.c"
         "passport/travel_sync.c"
         "passport/travel_app.c"
    INCLUDE_DIRS "." "passport"
    REQUIRES bsp bt esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash
         esp_http_server esp_http_client fatfs
)

# Generated travel font subsets (deterministic; see assets/fonts/README.md).
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/travel_font_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/travel_font_20.c"
)
