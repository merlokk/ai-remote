# The configuration web server of CLAUDE.md §10.16.
#
# `esp_http_server` is **in-tree** — it arrives with ESP-IDF and is not a new
# entry on root §1's dependency list, which matters more here than it did on the
# sibling board: this folder's dependency set is "that list minus four and plus
# one" (§10.4), and this component adds nothing to it.
#
# `web_paths.cpp` is on this line and includes `<cstddef>` and nothing else, so
# the host tests compile it with a bare C++ compiler (§10.11) — the whitelist
# that keeps `config.json` **and `fido.json`** off the network is the one thing
# here that must be testable without a board. `web_auth.cpp` is on it for the same
# reason and with the strongest case of the three: it is the comparison standing
# between a network and a device that can be pointed at another NATS server, it
# includes `<cstddef>` and `<cstdint>`, and it needs **no** crypto component — the
# credential is base64-encoded and compared, never decoded (`web_auth.h` argues
# why that is the safer direction), so neither libsodium nor mbedTLS is on this
# line and neither is in the host build.
#
# `/api/status` reads what the front page is about — whether this device could
# answer a request (`nats`, `registration`) and whether it is healthy. Reading
# state cannot reach a verdict (§10.10).
#
# **`responder` and `fido` are deliberately not on this line.** On the sibling
# board the equivalent exclusion was forced — a cycle through `screens` — and here
# it is a choice: the counters, the key's two booleans and the light's word all
# arrive through `web::SetApprovals`, filled by `main`, which is the one file that
# may depend on everything. A component that could reach the responder would be
# one refactor away from being able to reach the gate, and §10.10 rule 4 is easier
# to keep true when the call does not exist.
#
# `web_settings.cpp` is the write path of §10.16 — what a form may change, which
# is the only way anything outside this device changes what it does. It needs
# cJSON (the document), `config` (the fields it writes) and `nats` (the URL parser
# it refuses with). **It does not need `ui`**, which is where the sibling board
# gets its three-word Wi-Fi mode mapping from: that enum belongs to a Wi-Fi
# *screen* and there is no screen here, so the mapping is `web_settings.h`'s own
# and has exactly one owner.
idf_component_register(SRCS "web_auth.cpp" "web_paths.cpp" "web_policy.cpp" "web_settings.cpp" "web_server.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES esp_http_server storage config wifimgr app_update
                             nats registration espressif__cjson)
