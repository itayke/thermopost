idf_component_register(
    SRCS
        "cloud.cpp"
        "cloud_json.cpp"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        esp_wifi          # station, and the AP the captive portal will add
        esp_netif         # netif + SNTP client
        esp_event         # Wi-Fi / IP event loop
        esp_timer         # reconnect backoff
        esp_http_client   # the POST to Apps Script
        esp-tls           # TLS transport for the HTTP client
        mbedtls           # esp_crt_bundle_attach — the bundled Google roots
        nvs_flash         # endpoint + high-water mark (and the driver's creds)
        console           # esp_console — the `cloud` command
        shell             # ours: the REPL those commands land in
        settings          # the post's name, sent with every batch
        storage           # the record log the queue drains
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
