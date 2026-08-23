idf_component_register(
    SRCS
        "shell.cpp"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        console     # esp_console — IDF's, not this component (see shell.hpp)
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
