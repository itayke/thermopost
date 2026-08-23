idf_component_register(
    SRCS
        "audio.cpp"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "../../main"                    # board_pins.hpp — the I2S pin map and the amp enable
    PRIV_REQUIRES
        board_i2c                       # the shared bus the codec's control port sits on
        driver                          # i2c_master, for the address probe
        esp_driver_i2s
        espressif__esp_codec_dev
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
