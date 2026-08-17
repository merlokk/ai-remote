# The five screens of CLAUDE.md §10.8 — three of them so far: the clock, the
# limits that arrive over it, and the request card that goes over both.
#
# This is the one component in the firmware that is allowed to include both LVGL
# and the things that report on the world, and that is exactly why the deciding
# half of the clock is **not** here: it is `components/ui`, whose `REQUIRES` line
# is empty so that the host tests can compile it (§10.11). An entry appearing
# below is fine; a decision appearing in a file below is the sign that something
# was put in the wrong component.
idf_component_register(SRCS "clock_screen.cpp" "request_screen.cpp" "limits_screen.cpp" "screens.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES ui display pmic buttons audio wifimgr nats lvgl__lvgl)
