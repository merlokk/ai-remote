# The radio and nothing above it (CLAUDE.md §10.9). `esp_netif` and
# `nvs_flash` are here because `esp_wifi` cannot start without either: the
# first is the IP stack it hands packets to, the second is where it keeps its
# calibration — **nothing of ours is in NVS** (§10.15).
#
# What is deliberately absent is `config`: this component is handed an SSID and
# a password, and has no idea where they came from. The component that does
# know is `wifimgr`, which is also the one with the logic in it.
idf_component_register(SRCS "wifi_radio.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi esp_netif esp_event nvs_flash)
