# alert_logic.cpp is deliberately IDF-free (it also builds under tests/host/).
# alert.cpp is the runtime around it — the tick, the shared state and the tone —
# and is where any IDF dependency belongs.
#
# PRIV_REQUIRES, not REQUIRES, and that is load-bearing: `settings` publicly
# requires this component because settings::thresholds() returns an
# alert::Thresholds, so a public requirement back would be a cycle. Keeping the
# dependency private breaks it — alert.hpp exposes nothing from settings.
idf_component_register(
    SRCS
        "alert_logic.cpp"
        "alert.cpp"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        audio           # the tone, when the band is left
        sensor          # the readings that are tested against it
        settings        # the band itself, plus the mute and the volume
        esp_timer       # the monotonic clock the snooze deadline is on
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
