idf_component_register(
    SRCS
        "recorder.cpp"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        alert       # sample_flags() compares against alert::Thresholds
        cloud       # nudge the uploader when a record lands
        esp_timer   # uptime_us stamp
        sensor
        settings
        sleepstate  # written() kept across a deep sleep
        storage
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
