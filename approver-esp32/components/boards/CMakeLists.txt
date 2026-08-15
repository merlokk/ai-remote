idf_component_register(SRCS "board.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_gpio i2cbus pmic rtc)
