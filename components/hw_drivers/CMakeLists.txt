idf_component_register(SRCS "src/drivers.cpp"
                            "src/mic_level.cpp"
                            "src/pump_control.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES driver esp_adc)
