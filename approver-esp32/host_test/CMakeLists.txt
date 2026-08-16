# The host tier of CLAUDE.md §10.11 — the tests that need no board, and the
# tier that has to be comprehensive because it is the one that runs on every
# change.
#
# **This is a plain CMake project, not an ESP-IDF one, and that is a finding
# rather than a shortcut.** §10.11 assumed ESP-IDF's `linux` target: it is
# offered by this install (`idf.py --preview --list-targets` lists it), and it
# does not work here. `set-target linux` selects esp-clang and then tries to
# link a Windows PE against `kernel32`/`user32` with `ld.lld`, which ends in
#
#   ld.lld: error: unable to find library -lkernel32
#
# — the Linux target on a Windows host, with no Windows SDK in the picture.
# Measured on this machine, not assumed. So the tests are built by the host
# compiler directly, and `working-with-code.md` has the one line that runs
# them.
#
# What is kept from the plan is Unity: the sources come out of the ESP-IDF
# checkout rather than from a vendored copy, so the day the `linux` target
# works these tests move to it unchanged. Unity is plain C89 and compiles
# anywhere, which is why that is possible at all.
#
# What can be tested here is exactly what has no dependency on a board — which
# started as `components/ui` alone and turned out to be almost the whole
# library layer, because `fakes/` shadows ESP-IDF's headers rather than sitting
# behind an interface (§10.14.3): the navigator, the leased bus, and every
# driver on it. The protocol parity vectors of §10.11 tier 2 are the tenant
# still to come.

cmake_minimum_required(VERSION 3.22)
project(approver_esp32_host_test C CXX)

set(CMAKE_C_STANDARD 11)

# **C++20, because the drivers use designated initialisers.** ESP-IDF compiles
# them as gnu++26 and GCC accepts the syntax far earlier as an extension; MSVC
# does not, and refuses below /std:c++20. Nothing here needs C++20 for its own
# sake — this is the number that lets the shipping code compile unchanged.
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
set(CJSON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/espressif__cjson/cJSON")

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

add_executable(host_test
    test_main.cpp
    test_navigator.cpp
    test_i2c_bus.cpp
    test_pmic.cpp
    test_rtc.cpp
    test_imu.cpp
    test_es8311.cpp
    test_config.cpp
    test_buttons.cpp
    test_timezone.cpp
    test_speaker.cpp
    test_wifi_policy.cpp
    test_reachability.cpp
    test_timesync.cpp

    # The fake ESP-IDF the drivers are compiled against. `fakes/` shadows the
    # framework's headers on the include path below, so the sources listed
    # after it are the ones that ship, unmodified.
    fakes/fake_platform.cpp
    fakes/fake_storage.cpp
    fakes/fake_i2s.cpp

    # Under test.
    ../components/ui/navigator.cpp
    ../components/i2cbus/i2c_bus.cpp
    ../components/pmic/axp2101.cpp
    ../components/rtc/pcf85063.cpp
    ../components/imu/qmi8658.cpp
    ../components/audio/es8311.cpp
    ../components/audio/speaker.cpp
    ../components/config/config.cpp
    ../components/timezone/timezone.cpp
    ../components/buttons/buttons.cpp
    # The policy half of the Wi-Fi pair (§10.9). Like the navigator it includes
    # nothing, so it needs no fake — the driver next to it is all ESP-IDF and
    # belongs to the device tier.
    ../components/wifimgr/wifi_policy.cpp
    ../components/wifimgr/reachability.cpp
    # And the clock's half of the same shape (§10.8.2): when to ask a server
    # what time it is, with the asking left next door.
    ../components/timesync/sync_policy.cpp

    # cJSON, the real one, out of the managed component `idf.py build` fetched.
    "${CJSON_DIR}/cJSON.c"

    "${UNITY_DIR}/unity.c"
)

target_include_directories(host_test PRIVATE
    .
    fakes                   # must come before anything that could be real IDF
    ../components/ui
    ../components/i2cbus
    ../components/pmic
    ../components/rtc
    ../components/imu
    ../components/audio
    ../components/config
    ../components/timezone
    ../components/buttons
    ../components/storage
    ../components/wifimgr
    ../components/timesync
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

# `setenv`, `tzset`, `localtime_r` and `timegm` under the names the firmware
# calls them, forced into every C++ translation unit so the shipping sources
# need no `#ifdef _WIN32`. **CXX only** — Unity and cJSON are C, and a header
# that includes `<cstdlib>` makes MSVC refuse the file outright.
if(MSVC)
    # No quotes inside the flag: CMake would quote the whole token and MSVC then
    # reads the path as part of the option name. The path has no spaces.
    target_compile_options(host_test PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:/FI${CMAKE_CURRENT_SOURCE_DIR}/fakes/posix_compat.h>")
endif()

if(MSVC)
    # MSVC calls `fopen`, `tzset` and friends deprecated. That is its opinion
    # about the standard C library, not a finding about this code, and the
    # firmware compiles the same lines under GCC with -Wall -Wextra -Werror.
    target_compile_definitions(host_test PRIVATE _CRT_SECURE_NO_WARNINGS
                                                 _CRT_NONSTDC_NO_WARNINGS)
    target_compile_options(host_test PRIVATE /W4 /WX)
else()
    target_compile_options(host_test PRIVATE -Wall -Wextra -Werror)
endif()
