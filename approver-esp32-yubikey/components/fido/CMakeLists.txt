idf_component_register(SRCS "fido.cpp" "fido_usb.cpp" "ctap2.cpp" "cbor.cpp" "ctaphid_frames.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES usb storage crypto arkg esp_timer mbedtls espressif__cjson)
