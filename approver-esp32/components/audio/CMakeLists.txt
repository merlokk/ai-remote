idf_component_register(SRCS "es8311.cpp" "speaker.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES i2cbus storage esp_driver_i2s esp_driver_gpio)
