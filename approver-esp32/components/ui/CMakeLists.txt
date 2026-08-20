# No REQUIRES, and that is worth noticing rather than filling in: everything in
# here includes `<cstdint>` and nothing else — not LVGL, not ESP-IDF — which is
# what lets the host tests of §10.11 compile it with a bare C++ compiler. An entry
# appearing on this line is a sign that something with a dependency was put in
# the wrong component.
idf_component_register(SRCS "navigator.cpp" "clock_face.cpp" "request_card.cpp" "limits_view.cpp"
                            "activity_view.cpp"
                            "settings_menu.cpp" "touch_cal.cpp" "idle_policy.cpp" "wifi_view.cpp"
                    INCLUDE_DIRS ".")
