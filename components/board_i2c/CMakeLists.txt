idf_component_register(
    SRCS
        "board_i2c.cpp"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "../../main"        # board_pins.hpp
    REQUIRES
        driver              # i2c_master — appears in the public header
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
