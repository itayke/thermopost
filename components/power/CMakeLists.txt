idf_component_register(
    SRCS
        "power.cpp"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "../../main"    # board_pins.hpp, for the battery sense pin
    PRIV_REQUIRES
        alert           # a live alarm outranks the whole ladder
        display         # the backlight, the panel blank, and the LVGL lock
        esp_adc         # oneshot + curve-fitting calibration on ADC1_CH8
        esp_driver_gpio # rtc_io, to hold the touch interrupt pad through deep sleep
        esp_driver_usb_serial_jtag  # the SOF monitor: is a USB host on the cable?
        esp_timer       # the monotonic clock wake() stamps
        settings        # the power mode and the sleep delay
        sensor          # its own alert verdict, to check the ALERT line against
        console         # the `power` serial command
        lvgl__lvgl      # the inactivity counter the ladder reads
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
