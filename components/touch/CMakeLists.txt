idf_component_register(
    SRCS
        "touch.cpp"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "../../main"        # board_pins.hpp
    PRIV_REQUIRES
        driver              # i2c_master, gpio
        esp_lcd             # panel IO for the touch controller
        board_i2c
        display             # the LVGL display the indev attaches to
        power               # a touch on a dark screen only wakes it
        lvgl__lvgl
        espressif__esp_lcd_touch_ft5x06
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
