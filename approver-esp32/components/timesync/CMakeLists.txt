# The clock's network half (CLAUDE.md §10.8.2).
#
# `sync_policy.cpp` is the half worth protecting, the same way `wifi_policy.cpp`
# is next door: it includes nothing but `<cstdint>`, which is what lets
# §10.11's host tier compile it with a bare C++ compiler and no fake. If a
# dependency ever has to be added here for the policy's sake rather than the
# task's, something with a header in it went into the wrong file.
#
# `esp_netif` is here for `esp_netif_sntp.h`, the SNTP client that ships with
# the framework; `wifimgr` for the one question this asks it — is there an
# internet — which is the seam §10.9 said this would hang off.
idf_component_register(SRCS "sync_policy.cpp" "timesync.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES rtc config wifimgr esp_netif esp_timer)
