idf_component_register(
    SRCS "main.c" "state.c" "sdcard.c" "storage.c"
         "http_server.c" "lcd.c" "power.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_http_server esp_event json
             sdmmc esp_driver_sdmmc fatfs esp_timer esp_tinyusb
             esp_lcd esp_driver_spi esp_driver_gpio lwip
)
