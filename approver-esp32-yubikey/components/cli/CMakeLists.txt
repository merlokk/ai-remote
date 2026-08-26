# The console (CLAUDE.md §10.7). `console` is ESP-IDF's own REPL component;
# everything after it is a thing this device has a readout for.
#
# **The list is short because this board is** (§10.13): there is no chip and no
# surface here for a `pmic`, `rtc`, `imu`, `audio`, `display` or `i2cbus`
# component to talk to, and no web server. What this device does have a readout
# for is `led`, `indicator` and `fido` — one light and one key. `ui` is on the
# line because the request queue is a `ui::` type and so is the age formatter the
# readouts print.
#
# `esp_psram` and `spi_flash` are there for `status` alone: it reports what the
# chip actually came up with — how much PSRAM, in which mode, how much flash —
# against what the build assumed, because `hardware.md` §10.1 says both are
# assumptions on a board that ships in several memory variants.
idf_component_register(SRCS "console.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES console storage config crypto protocol registration responder
                             boards buttons led indicator fido ui wifi wifimgr
                             nats app_update esp_app_format esp_timer mbedtls
                             esp_psram spi_flash)
