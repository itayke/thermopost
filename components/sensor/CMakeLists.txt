idf_component_register(
    SRCS
        "sensor.cpp"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "../../main"        # board_pins.hpp
    PRIV_REQUIRES
        driver              # i2c_master
        esp_timer
        board_i2c
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
