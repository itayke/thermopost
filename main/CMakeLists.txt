idf_component_register(
    SRCS
        "app_main.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        driver          # i2c_master, spi_master, gpio
        esp_lcd         # panel IO, for the bring-up controller-ID read
        esp_hw_support  # esp_chip_info
        esp_psram       # esp_psram_get_size
        spi_flash       # esp_flash_get_size
        esp_timer
        nvs_flash
        storage
        settings
        alert
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
