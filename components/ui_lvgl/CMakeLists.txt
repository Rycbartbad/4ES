idf_component_register(
    SRCS
        "src/ui_lvgl.cpp"
        "src/ui_screen_diag.cpp"
        "src/ui_peer_view.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES lvgl lcd_touch esp_timer espnow_comm freertos
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
