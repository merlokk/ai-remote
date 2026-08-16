# The logic above the radio (CLAUDE.md §10.9).
#
# `wifi_policy.cpp` is the half worth protecting: it includes nothing but
# `<cstdint>`, which is what lets §10.11's host tier compile it with a bare
# C++ compiler and no fake. If a dependency ever has to be added to this line
# for the policy's sake rather than the manager's, something with a header in
# it was put in the wrong file.
idf_component_register(SRCS "wifi_policy.cpp" "wifi_manager.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES wifi config esp_timer)
