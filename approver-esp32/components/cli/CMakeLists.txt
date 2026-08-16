idf_component_register(SRCS "console.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES console storage config timezone timesync boards pmic rtc buttons imu audio display wifi wifimgr app_update esp_app_format esp_timer)
