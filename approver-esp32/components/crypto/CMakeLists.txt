# The device's identity (CLAUDE.md §10.6) — an Ed25519 key, derived per boot from
# an eFuse key through the HMAC unit where one is burned, and from a seed kept in
# NVS where one is not. `device_key.h` says which route shipped and what the
# second one costs.
#
# `esp_security` is where `esp_hmac.h` lives on this framework version, `efuse`
# is what answers whether a key block was burned for it, and
# `espressif__libsodium` is §10.4's Ed25519 — mbedTLS has no EdDSA at all, which
# is why this is the one component in the firmware that adds a crypto library.
#
# `nvs_flash` and `bootloader_support` are §10.6's **fallback**, and they are the
# reason this component's dependency line is longer than the design called for:
# with no eFuse key burned the seed is generated once — with the SAR-ADC entropy
# source of `bootloader_random.h`, because the radio is not up this early — and
# kept in NVS. That is the one namespace this firmware puts there, against
# §10.15's rule, with the exception argued in `device_key.h`.
#
# **Nothing about approvals is in here** (§10.14.2). It signs bytes; what those
# bytes mean is `components/protocol`'s subject, and the split is what keeps that
# half of §7 testable on the host with no board.
idf_component_register(SRCS "device_key.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES esp_security efuse nvs_flash bootloader_support espressif__libsodium)
