# Checking the handler's signature, and base64 (CLAUDE.md §10.6). **There is no
# key in here.** This component used to hold the board's Ed25519 identity — an
# eFuse route through the HMAC unit and a stored-seed fallback in NVS — and §10.18
# took the signer into the security key, so what is left is one verify and two
# base64 calls. `crypto.h` says what that removal bought.
#
# `espressif__libsodium` is §10.4's Ed25519 and stays for exactly one reason: §6's
# reply is signed by the handler and the handler's key is Ed25519 by fixed
# protocol, while mbedTLS has no EdDSA at all. It is still the one component in
# this firmware that adds a crypto library, and it is now the only thing it is
# added for.
#
# `nvs_flash` is no longer here to *store* anything. It is here to **erase**: the
# seed an older firmware left in NVS is a private key belonging to nothing, and the
# firmware that stopped using it is the one that removes it. `esp_security`,
# `efuse` and `bootloader_support` went with the routes that needed them — the HMAC
# unit, the eFuse purpose lookup, and the SAR-ADC entropy the seed was generated
# from before the radio was up.
#
# **Nothing about approvals is in here** (§10.14.2). It checks bytes; what those
# bytes mean is `components/protocol`'s subject, and the split is what keeps that
# half of §7 testable on the host with no board.
idf_component_register(SRCS "crypto.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash espressif__libsodium)
