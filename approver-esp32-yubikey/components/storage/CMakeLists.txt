# The `storage` partition (CLAUDE.md §10.15).
#
# Two files, split by what they include rather than by what they do:
# `storage.cpp` is the mount and the listing, all `esp_spiffs.h`; `file_ops.cpp`
# is the atomic write of §10.15 and has no ESP-IDF in it at all, which is what
# puts that sequence under §10.11's host tier unmodified — the rule
# `wifi_policy.cpp` and `sync_policy.cpp` follow next door.
idf_component_register(SRCS "storage.cpp" "file_ops.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES spiffs)
