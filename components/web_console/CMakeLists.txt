idf_component_register(SRCS "src/web_console.cpp"
                              "src/wifi_scan.cpp"
                              "src/llm_client.cpp"
                              "src/script_normalizer.cpp"
                              "src/script_inject.cpp"
                              "src/dns_server.cpp"
                        INCLUDE_DIRS "include"
                        REQUIRES esp_http_server esp_wifi esp_http_client json interpreter script_io espnow_comm hw_drivers nvs_flash
                        PRIV_REQUIRES mbedtls)
