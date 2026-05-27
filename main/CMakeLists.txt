# Conditional source selection based on device role
if(CONFIG_DEVICE_ROLE_SENSOR)
    set(MAIN_SRCS "app_main_sensor.cpp")
else()
    set(MAIN_SRCS "app_main.cpp")
endif()

idf_component_register(SRCS "${MAIN_SRCS}"
                       INCLUDE_DIRS ""
                         REQUIRES interpreter espnow_comm hw_drivers script_io web_console nvs_flash lcd_touch)

# C++17 required (TC-P1.1)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
