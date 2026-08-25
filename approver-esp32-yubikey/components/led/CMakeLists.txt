idf_component_register(SRCS "led.cpp" "led_frames.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_uart esp_driver_gpio esp_timer)
