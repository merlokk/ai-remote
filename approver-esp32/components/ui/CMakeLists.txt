# No REQUIRES, and that is worth noticing rather than filling in: the navigator
# includes nothing — not LVGL, not ESP-IDF — which is what lets the host tests
# of §10.11 compile it with a bare C++ compiler. An entry appearing on this line
# is a sign that something with a dependency was put in the wrong component.
idf_component_register(SRCS "navigator.cpp"
                    INCLUDE_DIRS ".")
