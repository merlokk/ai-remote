# The console (CLAUDE.md §10.7). `console` is ESP-IDF's own REPL component;
# everything after it is a thing this device has a readout for.
#
# **Nine components are missing from this line compared with the sibling board's**,
# and each of them is a chip or a surface that is not here: `pmic`, `rtc`, `imu`,
# `audio`, `display`, `screens`, `i2cbus`, `web` — and `ui` stays, because the
# request queue and the two watched documents are still `ui::` types. What replaced
# them is `led`, `indicator` and `fido`: one light and one key.
idf_component_register(SRCS "console.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES console storage config crypto protocol registration responder watcher
                             timezone timesync boards buttons led indicator fido ui wifi wifimgr
                             nats app_update esp_app_format esp_timer mbedtls)
