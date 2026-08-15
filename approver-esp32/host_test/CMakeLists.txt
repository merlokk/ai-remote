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
# What can be tested here is exactly what has no dependency on a board: today
# that is `components/ui`, whose CMakeLists has an empty REQUIRES for this
# reason. The I²C lease's fake backend (§10.14.3) and the protocol parity
# vectors (§10.11 tier 2) are the next two tenants.

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

    # The fake ESP-IDF the drivers are compiled against. `fakes/` shadows the
    # framework's headers on the include path below, so the sources listed
    # after it are the ones that ship, unmodified.
    fakes/fake_platform.cpp

    # Under test.
    ../components/ui/navigator.cpp
    ../components/i2cbus/i2c_bus.cpp
    ../components/pmic/axp2101.cpp
    ../components/rtc/pcf85063.cpp
    ../components/imu/qmi8658.cpp
    ../components/audio/es8311.cpp

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
    "${UNITY_DIR}"
)

# Unity prints floats in its failure messages; without this it will not compile
# the double support it references.
target_compile_definitions(host_test PRIVATE UNITY_INCLUDE_DOUBLE)

if(MSVC)
    target_compile_options(host_test PRIVATE /W4 /WX)
else()
    target_compile_options(host_test PRIVATE -Wall -Wextra -Werror)
endif()
