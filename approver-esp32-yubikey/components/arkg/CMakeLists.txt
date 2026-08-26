# **The derivation that decides which key signs a verdict** (CLAUDE.md §10.18).
#
# Two files, and the split is §10.14.2's:
#
#   * `arkg.cpp` includes nothing at all — no ESP-IDF, no crypto library, no
#     allocation. Five of the derivation's seven steps live there, which is why the
#     host tier can run them against numbers `tools/make_arkg_vectors.py` produced
#     (§10.11 tier 2) with no board and no PSA;
#   * `arkg_psa.cpp` is the four primitives that need the chip: SHA-256, a scalar
#     multiplication and an ECDH through PSA Crypto, and one point addition through
#     `mbedtls_ecp_muladd` because PSA has no name for that operation. That file
#     says why in full at its top.
#
# **`mbedtls` is the only dependency**, and it is one this firmware already links
# for `esp-tls`, so §10.4's list does not move: the curve arithmetic here is bytes
# already in the image rather than a new library.
#
# Nothing here knows what CBOR, a subject or a verdict is. Turning `previewSign`'s
# COSE key into two points is `components/fido`'s, and the request those points end
# up in is `components/protocol`'s.
idf_component_register(SRCS "arkg.cpp" "arkg_psa.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES mbedtls)
