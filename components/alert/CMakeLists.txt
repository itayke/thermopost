# alert_logic.cpp is deliberately IDF-free (it also builds under tests/host/).
# The IDF-facing wrapper — LVGL visuals plus the ES8311 tone — lands here as
# alert.cpp later, and that is where any IDF dependency belongs.
idf_component_register(
    SRCS
        "alert_logic.cpp"
    INCLUDE_DIRS
        "include"
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
