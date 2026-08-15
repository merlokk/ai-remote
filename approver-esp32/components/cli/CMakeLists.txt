idf_component_register(SRCS "console.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES console storage boards pmic rtc buttons app_update esp_app_format esp_timer)
