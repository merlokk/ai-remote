# The bus link (CLAUDE.md §10.3, §10.5).
#
# Two of these four files are the half worth protecting: `endpoint.cpp` and
# `link_policy.cpp` include `<cstdint>`/`<cstring>` and nothing else, which is
# what lets §10.11's host tier compile them with a bare C++ compiler and no
# fake. If a dependency ever has to be added to this line for *their* sake
# rather than the socket's, something with a header in it went into the wrong
# file — the rule `wifimgr` and `timesync` state next door.
#
# `debsahu__espidf-nats` is the managed component of §10.4: header-only C++, so
# it appears here and nowhere else in the build. `nats_bus.cpp` is the only
# file in this firmware that includes it, which is what keeps its 7,000 lines
# out of everything that merely wants to publish.
idf_component_register(SRCS "endpoint.cpp" "link_policy.cpp" "nats_bus.cpp" "nats_link.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES config wifimgr esp_timer debsahu__espidf-nats)
