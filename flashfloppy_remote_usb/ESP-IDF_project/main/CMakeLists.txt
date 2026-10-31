idf_component_register(
    SRCS "main.c" "state.c" "sdcard.c" "storage.c"
         "http_server.c" "lcd.c" "power.c" "util.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_http_server esp_event json
             sdmmc esp_driver_sdmmc fatfs esp_timer esp_tinyusb
             esp_lcd esp_driver_spi esp_driver_gpio lwip
)

# v39: stamp USB MSC activity for the swap idle check. The esp_tinyusb
# storage component owns these TinyUSB callbacks, so the linker routes
# TinyUSB's calls through __wrap_* in sdcard.c, which forward to the
# component's originals (__real_*).
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=tud_msc_read10_cb"
    "-Wl,--wrap=tud_msc_write10_cb")
