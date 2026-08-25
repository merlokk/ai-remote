# §6 and §7's wire format (CLAUDE.md §10.2) — what a decision is signed over, and
# the registration exchange around it.
#
# One dependency, and the one that is **not** here is the point of the file.
# `signing.cpp` includes `<cstdint>` and nothing at all; `registration.cpp` adds
# cJSON, which keeps both in §10.11's host tier because those tests already
# compile the real cJSON for `components/config`.
#
# What must never appear on this line is the crypto library. These bytes are
# assembled here and signed elsewhere, and the registration reply's signature is
# checked through a function pointer handed in from outside — which is what lets
# the ordering rule of §10.7 be tested without libsodium, and what stops that
# ordering from being the caller's discipline. `registration.h` argues it.
# `ui` is on this line for one struct: `ui::Request` is §7's field list verbatim,
# and the alternative was a second copy of it here plus a conversion between them
# — two places for a field to be forgotten. It costs nothing, because
# `components/ui` includes `<cstdint>` and nothing else.
idf_component_register(SRCS "signing.cpp" "registration.cpp" "approval.cpp" "status.cpp"
                            "activity.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES ui espressif__cjson)
