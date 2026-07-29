idf_component_register(
    SRCS
        "storage.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        joltwallet__littlefs
        nvs_flash
        esp_partition
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
