# The console (CLAUDE.md §10.7). `console` is ESP-IDF's own REPL component;
# everything after it is a thing this device has a readout for.
#
# **Eleven components are missing from this line compared with the sibling
# board's**, and each of them is a chip or a surface that is not here: `pmic`,
# `rtc`, `imu`, `audio`, `display`, `screens`, `i2cbus`, `web` — and so are
# `watcher`, which read §9.7's and §9.10's documents for a screen this device does
# not have, and `timesync` / `timezone`, which kept a clock this device cannot show
# and nothing here reads. `ui` stays, because the request queue is still a `ui::`
# type and so is the age formatter the readouts print. What replaced them is `led`,
# `indicator` and `fido`: one light and one key.
idf_component_register(SRCS "console.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES console storage config crypto protocol registration responder
                             boards buttons led indicator fido ui wifi wifimgr
                             nats app_update esp_app_format esp_timer mbedtls)
