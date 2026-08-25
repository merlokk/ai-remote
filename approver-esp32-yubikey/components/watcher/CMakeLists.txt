idf_component_register(SRCS "watcher.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES protocol nats ui esp_timer)
