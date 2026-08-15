idf_component_register(SRCS "panel.cpp" "touch.cpp" "lvgl_display.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES i2cbus esp_lcd esp_driver_spi esp_driver_gpio
                             lvgl__lvgl espressif__esp_lcd_touch
                    PRIV_REQUIRES espressif__esp_lvgl_port
                                  espressif__esp_lcd_sh8601
                                  waveshare__esp_lcd_touch_cst9217)
