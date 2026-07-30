idf_component_register(
    SRCS
        "ui_main.cpp"
        "ui_chrome.cpp"
        "ui_nav.cpp"
        "ui_stepper.cpp"
        "ui_menu.cpp"
        "ui_settings.cpp"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "../../main"    # board_pins.hpp, for the panel resolution the layout derives from
    PRIV_REQUIRES
        alert           # thresholds, for the band lines and the readout tint
        display         # the LVGL lock and the display handle
        recorder        # the graph's history ring
        sensor          # the live reading
        settings
        lvgl__lvgl
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
