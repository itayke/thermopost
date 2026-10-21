idf_component_register(
    SRCS
        "ota.cpp"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        app_update        # esp_ota_*: the slots, the boot choice and the rollback
        esp_app_format    # esp_app_get_description, the running version
        esp_http_client   # the download
        esp-tls           # its TLS transport
        mbedtls           # the cert bundle, and the sha256 the Sheet checks against
        esp_timer         # the proving deadline
        console           # esp_console — the `ota` command
        shell             # ours: the REPL that command lands in
        alert             # no update while an alarm is open
        power             # nor on a low battery
        settings          # the record interval the deadline allows for
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
