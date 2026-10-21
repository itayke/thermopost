idf_component_register(
    SRCS
        "ui_main.cpp"
        "ui_chrome.cpp"
        "ui_nav.cpp"
        "ui_stepper.cpp"
        "ui_menu.cpp"
        "ui_settings.cpp"
        "ui_network.cpp"
        "ui_configure.cpp"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "../../main"    # board_pins.hpp, for the panel resolution the layout derives from
    PRIV_REQUIRES
        alert           # thresholds for the band lines; the alarm state and its two buttons
        audio           # the Sound page's alarm test
        cloud           # the Network page: link state, addresses, sync
        cloud           # the status strip's uploaded counter
        display         # the LVGL lock and the display handle
        ota             # the Network page's firmware line
        power           # the Power page: the ladder's mode, tier and battery
        recorder        # the graph's history ring
        sensor          # the live reading
        settings
        lvgl__lvgl
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
