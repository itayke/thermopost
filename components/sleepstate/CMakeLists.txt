idf_component_register(
    SRCS
        "sleepstate.cpp"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        esp_timer   # the clock the deadlines are converted from
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
