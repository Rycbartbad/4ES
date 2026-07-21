idf_component_register(SRCS "src/protocol.cpp"
                              "src/peer_mgr.cpp"
                              "src/comm.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES "esp_wifi" "esp_timer")
