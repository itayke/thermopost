idf_component_register(
    SRCS
        "settings.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        alert           # thresholds() returns an alert::Thresholds
        nvs_flash
        esp_hw_support  # esp_read_mac, for the default post name
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
