# The configuration web server of CLAUDE.md §10.16.
#
# `esp_http_server` is **in-tree** — it arrives with ESP-IDF and is not a new
# entry on root §1's dependency list, which is the reason §10.8.6 named it as the
# cheap alternative to a 6 mm on-screen keyboard in the first place.
#
# `web_paths.cpp` is on this line and includes `<cstddef>` and nothing else, so
# the host tests compile it with a bare C++ compiler (§10.11) — the whitelist
# that keeps `config.json` off the network is the one thing here that must be
# testable without a board.
#
# `/api/status` reads what the front page is about — whether this device could
# answer a request (`nats`, `registration`) and whether it is healthy (`boards`,
# for the PMIC). Reading state is the same class of thing the status pages of
# §10.8.5 do, and none of it can reach a verdict (§10.10).
#
# **`responder` is deliberately not on this line**, and cannot be: it requires
# `screens`, `screens` requires this component for the row that says whether the
# server is up, and a cycle in `REQUIRES` is a build that does not happen. Its
# counters arrive through `web::SetApprovals`, which is what that header argues.
#
# `web_settings.cpp` is the write path of §10.16 — what a form may change, which is
# the only way anything outside this device changes what it does. It needs cJSON
# (the document), `config` (the fields it writes), `nats` (the URL parser it
# refuses with) and `ui` — that last one for `ui::WifiMode`, so the three words a
# page sends reach the two config fields through the same function the Wi-Fi screen
# uses rather than through a second copy of the mapping. `ui` has an empty
# `REQUIRES` and is a leaf of pure arithmetic, which is the argument
# `components/display` already makes for depending on it (§10.8.5).
# `web_auth.cpp` is on it for the same reason and with the strongest case of the
# three: it is the comparison standing between a network and a device that can be
# pointed at another NATS server, it includes `<cstddef>` and `<cstdint>`, and it
# needs **no** crypto component — the credential is base64-encoded and compared,
# never decoded (`web_auth.h` argues why that is the safer direction), so mbedTLS
# stays out of this line and out of the host build.
idf_component_register(SRCS "web_auth.cpp" "web_paths.cpp" "web_policy.cpp" "web_settings.cpp" "web_server.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES esp_http_server storage config wifimgr app_update
                             nats registration boards ui espressif__cjson)
