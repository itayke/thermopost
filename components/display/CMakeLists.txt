idf_component_register(
    SRCS
        "display.cpp"
        "lv_port_mem.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "../../main"        # board_pins.hpp
    REQUIRES
        driver              # spi_master, ledc, gpio
        esp_lcd
        esp_timer
        lvgl__lvgl
        espressif__esp_lcd_ili9341
    PRIV_REQUIRES
        console             # the `shot` screenshot command
        mbedtls             # its base64 encoder
    # lv_port_mem.c provides LVGL's lv_*_core allocator overrides
    # (LV_USE_CUSTOM_MALLOC). LVGL doesn't depend on this component, so without
    # whole-archive its object is never pulled in to satisfy those references
    # and the link fails on undefined lv_malloc_core/etc.
    WHOLE_ARCHIVE
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
