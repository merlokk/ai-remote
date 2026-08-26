# The host tier of CLAUDE.md §10.11 — the tests that need no board, and the tier
# that has to be comprehensive because it is the one that runs on every change.
#
# **This is a plain CMake project, not an ESP-IDF one, and that is a finding
# rather than a shortcut.** The sibling folder's copy of this file records it:
# §10.11 assumed ESP-IDF's `linux` target, it is offered by this install
# (`idf.py --preview --list-targets` lists it), and it does not work here —
# `set-target linux` selects esp-clang and then tries to link a Windows PE against
# `kernel32`/`user32` with `ld.lld`, ending in
#
#   ld.lld: error: unable to find library -lkernel32
#
# So the tests are built by the host compiler directly, and
# `working-with-code.md` has the one line that runs them.
#
# What is kept from the plan is Unity: the sources come out of the ESP-IDF
# checkout rather than from a vendored copy, so the day the `linux` target works
# these tests move to it unchanged.
#
# **What is testable here is what has no dependency on a board**, which on this
# device is more of the firmware than on the sibling's — there are no chips to
# fake. The four subjects that matter most are all in this list:
#
#   * the **state ranking** (§10.17), which decides what a single emitter says
#     about a device that is several things at once;
#   * the **LED encoding**, whose only other verification method is looking at a
#     desk;
#   * **CTAPHID and CBOR**, which are parsers fed by a device on the other end of
#     a cable — the same class of untrusted input §10.10 talks about, and the one
#     a real key will never exercise the bad paths of;
#   * **§7's signing bytes**, which have to match another language byte for byte.
#
# **Tier 2 lives here too, and its header is the sibling board's** — see the
# `VECTORS_DIR` block below for why there is only one copy of it in the
# repository.

cmake_minimum_required(VERSION 3.22)
project(approver_esp32_yubikey_host_test C CXX)

set(CMAKE_C_STANDARD 11)

# **C++20, because the shipping code uses designated initialisers.** ESP-IDF
# compiles them as gnu++26 and GCC accepts the syntax far earlier as an
# extension; MSVC does not, and refuses below /std:c++20. Nothing here needs
# C++20 for its own sake — this is the number that lets the shipping code compile
# unchanged.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Where Unity lives inside the framework. Overridable so this is not pinned to
# one machine's install; `run.cmd` passes the value it finds.
if(NOT DEFINED IDF_PATH)
    set(IDF_PATH "$ENV{IDF_PATH}")
endif()
set(UNITY_DIR "${IDF_PATH}/components/unity/unity/src")

# cJSON is a managed component, so it is not in git — `idf.py build` fetches it
# into `managed_components/`. Depending on it here is the price of testing the
# real parser instead of a stand-in.
if(NOT DEFINED CJSON_DIR)
    set(CJSON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/espressif__cjson/cJSON")
endif()

if(NOT EXISTS "${CJSON_DIR}/cJSON.c")
    message(FATAL_ERROR
        "cJSON not found at ${CJSON_DIR}. Run `idf.py build` once to fetch the "
        "managed components; working-with-code.md has the command.")
endif()

if(NOT EXISTS "${UNITY_DIR}/unity.c")
    message(FATAL_ERROR
        "Unity not found at ${UNITY_DIR}. Pass -DIDF_PATH=<esp-idf checkout>; "
        "working-with-code.md has this machine's path.")
endif()

# **Tier 2's header is the sibling board's, and this folder deliberately does not
# keep a copy of it.**
#
# `parity_vectors.h` is generated from `approver/protocol.py` and `lib/crypto.py`
# and pins §7's signing bytes against another language. That is a *protocol*
# fact, identical for both devices by definition — so two committed copies of it
# would be two files that must never differ and nothing that would notice if they
# did. There is one, and both boards read it.
#
# The generator lives there too (`approver-esp32/tools/make_vectors.py`) and
# writes there. A copy of it in this folder was written and deleted: it resolved
# its output path the same way, so running it from here would have silently
# regenerated the *other* board's files. One generated file, one generator, one
# place.
#
# Overridable for the same reason `IDF_PATH` and `CJSON_DIR` are.
if(NOT DEFINED VECTORS_DIR)
    set(VECTORS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../approver-esp32/host_test/vectors")
endif()

if(NOT EXISTS "${VECTORS_DIR}/parity_vectors.h")
    message(FATAL_ERROR
        "The parity vectors were not found at ${VECTORS_DIR}. They are generated "
        "by approver-esp32/tools/make_vectors.py and committed there; pass "
        "-DVECTORS_DIR=<dir> if that folder is not beside this one. tests.md §10.11 "
        "explains why there is only one copy.")
endif()

add_executable(host_test
    test_main.cpp

    # This board's own two pieces of hardware, and the ranking above them.
    test_led.cpp
    test_indicator.cpp
    test_buttons.cpp

    # The key on the OTG port, bottom up: framing, then CBOR, then CTAP2, and
    # then the derivation that decides which key the verdict is signed with.
    test_ctaphid.cpp
    test_cbor.cpp
    test_ctap2.cpp
    test_arkg.cpp

    test_request_card.cpp
    test_config.cpp
    test_wifi_policy.cpp
    test_reachability.cpp
    test_nats.cpp
    test_signing.cpp
    test_registration.cpp
    test_approval.cpp
    test_age_text.cpp

    # Tier 2: the vectors `approver/protocol.py` and `lib/crypto.py` generated,
    # run against the assemblers that have to reproduce them. The header comes
    # from `VECTORS_DIR` above; nothing here needs Python to build.
    test_vectors.cpp

    # The fake ESP-IDF the sources are compiled against. `fakes/` shadows the
    # framework's headers on the include path below, so the sources listed after
    # it are the ones that ship, unmodified.
    fakes/fake_platform.cpp
    fakes/fake_storage.cpp

    # Under test.
    #
    # **The LED's arithmetic** (§10.17.1): what colour to show at a given
    # millisecond, and what bytes that colour is on the wire. The UART next door
    # is all ESP-IDF and belongs to the device tier.
    ../components/led/led_frames.cpp
    # **The ranking** (§10.17): the fourteen-way decision about what a device
    # that is several things at once should look like.
    ../components/indicator/indicator_policy.cpp
    # **The key, in three layers, none of which knows what USB is** (§10.18).
    ../components/fido/ctaphid_frames.cpp
    ../components/fido/cbor.cpp
    ../components/fido/ctap2.cpp
    # **The ARKG derivation** (§10.18): the arithmetic that decides which public
    # key this device registers, and therefore whether anything it signs can ever
    # be verified. The curve arrives through a function-pointer backend, so all of
    # it compiles here; the PSA backend next to it is device-only.
    ../components/arkg/arkg.cpp
    # SHA-256 for the tests only -- `sha256_ref.h` says why it is not a fake and
    # why nothing on the device links it.
    sha256_ref.c

    ../components/ui/request_card.cpp
    ../components/ui/age_text.cpp
    ../components/config/config.cpp
    # The atomic write of §10.15, which has no ESP-IDF in it and so is the
    # sequence that ships rather than a model of one in `fakes/`.
    ../components/storage/file_ops.cpp
    ../components/buttons/buttons.cpp
    # The policy half of the Wi-Fi pair (§10.9). Like the ranking it includes
    # nothing, so it needs no fake — the driver next to it is all ESP-IDF and
    # belongs to the device tier.
    ../components/wifimgr/wifi_policy.cpp
    ../components/wifimgr/reachability.cpp
    # And the bus link's (§10.5): where the server is, and when to have a
    # connection to it.
    ../components/nats/endpoint.cpp
    ../components/nats/link_policy.cpp
    # And §7's own half (§10.2): the bytes a decision is signed over. Here the
    # reason is the strongest of all — this is the one file whose output has to
    # match another language byte for byte.
    ../components/protocol/signing.cpp
    ../components/protocol/registration.cpp
    ../components/protocol/approval.cpp

    # cJSON, the real one, out of the managed component `idf.py build` fetched.
    "${CJSON_DIR}/cJSON.c"

    "${UNITY_DIR}/unity.c"
)

target_include_directories(host_test PRIVATE
    .
    fakes                   # must come before anything that could be real IDF
    "${VECTORS_DIR}/.."      # so `#include "vectors/parity_vectors.h"` resolves
    ../components/led
    ../components/indicator
    ../components/fido
    ../components/arkg
    ../components/ui
    ../components/config
    ../components/buttons
    ../components/storage
    ../components/wifimgr
    ../components/nats
    ../components/protocol
    "${CJSON_DIR}"
    "${UNITY_DIR}"
)

# Unity prints floats in its failure messages; without this it will not compile
# the double support it references.
target_compile_definitions(host_test PRIVATE UNITY_INCLUDE_DOUBLE)

# Where `fake::PutRepoFile` finds the committed `config.json` /
# `config.init.json`. The tests read the **real** files, which is what makes
# "the shipped defaults parse" mean anything.
target_compile_definitions(host_test PRIVATE
    SPIFFS_IMAGE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../spiffs_image")

if(MSVC)
    # MSVC calls `fopen` and friends deprecated. That is its opinion about the
    # standard C library, not a finding about this code, and the firmware
    # compiles the same lines under GCC with -Wall -Wextra -Werror.
    target_compile_definitions(host_test PRIVATE _CRT_SECURE_NO_WARNINGS
                                                 _CRT_NONSTDC_NO_WARNINGS)
    target_compile_options(host_test PRIVATE /W4 /WX)
else()
    target_compile_options(host_test PRIVATE -Wall -Wextra -Werror)
endif()
