idf_component_register(SRCS "src/tcp_protocol.cpp"
                              "src/tcp_comm_master.cpp"
                              "src/tcp_comm_sensor.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES "espnow_comm"
                       PRIV_REQUIRES "lwip esp_wifi")
