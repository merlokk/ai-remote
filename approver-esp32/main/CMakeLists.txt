idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES boards storage config timezone cli display ui wifimgr app_update esp_app_format esp_timer)

# Build a SPIFFS image out of `spiffs_image/` and give it the `storage`
# partition of partitions.csv. FLASH_IN_PROJECT means a plain `idf.py flash`
# writes it too — otherwise the first boot finds an unformatted partition.
spiffs_create_partition_image(storage ../spiffs_image FLASH_IN_PROJECT)
