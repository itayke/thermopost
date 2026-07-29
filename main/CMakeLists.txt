# bringup_probe.cpp is always compiled even though it only runs under
# CONFIG_THERMOPOST_RUN_BRINGUP_PROBE. Making the source list conditional
# instead would be free at link time but needs an explicit `idf.py reconfigure`
# after toggling the option — ninja re-reads sdkconfig.h, not CMakeLists.txt —
# and a stale build that silently ignores the switch is a nasty way to lose an
# hour. With IDF's -ffunction-sections + --gc-sections the unreferenced probe is
# dropped from the image anyway, so this costs nothing but build time.
idf_component_register(
    SRCS
        "app_main.cpp"
        "bringup_probe.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        driver          # i2c_master, spi_master, gpio (probe)
        esp_lcd         # panel IO, for the probe's controller-ID read
        esp_hw_support  # esp_chip_info
        esp_psram       # esp_psram_get_size
        spi_flash       # esp_flash_get_size
        esp_timer
        nvs_flash
        display
        storage
        settings
        alert
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
