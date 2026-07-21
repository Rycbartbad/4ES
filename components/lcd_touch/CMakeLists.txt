idf_component_register(SRCS "src/st7789.cpp" "src/cst816d.cpp" "src/touch_logic.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES driver spi_flash)

if(CONFIG_LCD_TOUCH_ENABLE_BACKLIGHT_PWM)
    # LEDC PWM is used for backlight dimming
    add_compile_options(-DCONFIG_LCD_TOUCH_BACKLIGHT_PWM)
endif()
