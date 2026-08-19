# The screens of CLAUDE.md §10.8: the clock, the limits that arrive over it, the
# settings list with the status pages, the touch test, the Wi-Fi screen and the
# list of what is on the air — and the request card that goes over all of them.
#
# This is the one component in the firmware that is allowed to include both LVGL
# and the things that report on the world, and that is exactly why the deciding
# half of the clock is **not** here: it is `components/ui`, whose `REQUIRES` line
# is empty so that the host tests can compile it (§10.11). An entry appearing
# below is fine; a decision appearing in a file below is the sign that something
# was put in the wrong component.
idf_component_register(SRCS "clock_screen.cpp" "request_screen.cpp" "limits_screen.cpp"
                            "settings_screen.cpp" "status_screen.cpp" "touch_screen.cpp"
                            "wifi_screen.cpp" "wifi_scan_screen.cpp"
                            "screens.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES ui display pmic imu buttons audio config wifimgr nats web app_update
                             lvgl__lvgl)
