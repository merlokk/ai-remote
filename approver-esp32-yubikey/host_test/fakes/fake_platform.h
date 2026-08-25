#pragma once

// The fake ESP-IDF the host tests compile the real drivers against
// (CLAUDE.md §10.14.3, §10.11).
//
// **The production code is not modified and does not know this exists.**
// §10.14.3 asked for "an interface with two implementations", and this is the
// other way to get the same property: the headers `i2c_bus.cpp` includes are
// shadowed by fakes on the include path, so the file that ships is the file
// under test. The reason it went this way rather than the other is one fact —
// the lease's mutex is a FreeRTOS object and is *not* behind any bus backend,
// so a `Backend` interface would have needed these shims anyway and then added
// a vtable on top of them. §10.14.3 records the change.
//
// What that buys, and it is the whole argument: every driver on this board
// includes exactly `i2c_bus.h`, `esp_err.h`, `esp_log.h` and FreeRTOS. So one
// shim set makes the PMIC, the RTC, the IMU and the codec testable too,
// against the same fake wire.
//
// The fake models an ordinary I²C register device, because that is what all
// five chips are: a write of `{reg}` moves the cursor, a write of
// `{reg, v, ...}` stores from there, a read takes from the cursor on, and a
// write-read does both. Nothing about any particular chip is in here — the
// register *meanings* live in each driver's test.

#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

namespace fake {

struct Transfer {
    enum class Kind : uint8_t { kProbe, kWrite, kRead, kWriteRead };

    Kind kind;
    uint8_t address;
    uint32_t clock_hz;

    // **Which lease this transfer happened under**, bumped every time the
    // mutex is given back. It is the only way to assert the thing §10.14.3
    // exists for — that a read-modify-write, or the five reads behind one
    // `power` line, were *one* uninterrupted sequence rather than five that
    // happened to work. Same epoch, same lease.
    uint32_t epoch;
    // Enough for every register write these drivers make; a longer one is
    // recorded truncated, with `write_length` telling the truth about it.
    uint8_t write[8];
    size_t write_length;
    size_t read_length;
};

// A chip on the fake wire. 256 registers is every 8-bit register map, which is
// all five of this board's chips.
struct Device {
    bool present;
    uint8_t address;
    uint8_t regs[256];
    uint8_t cursor;
};

struct Platform {
    static constexpr size_t kMaxTransfers = 128;
    static constexpr size_t kMaxDevices = 8;
    static constexpr size_t kMaxPins = 40;

    // --- what went over the wire ---
    Transfer transfers[kMaxTransfers];
    size_t transfer_count;
    bool transfers_overflowed;

    // --- what is on the wire ---
    Device devices[kMaxDevices];
    size_t device_count;

    // --- the driver's own bookkeeping, so a test can see it leak ---
    bool bus_open;
    size_t bus_open_count;
    size_t bus_delete_count;
    size_t open_handles;  // add_device minus rm_device: this is the leak check

    // --- failure injection ---
    esp_err_t next_error;
    uint8_t next_error_address;  // kAnyAddress for "whichever comes first"
    size_t next_error_skip;      // transfers to let through before it fires

    // --- FreeRTOS ---
    // The mutex never blocks here, which is deliberate: a fake that blocked
    // could not tell "asked for 20 ms" from "asked for forever", and that
    // distinction is the whole of §10.14.3's timeout-not-block rule.
    bool mutex_taken;
    TickType_t last_take_ticks;
    size_t take_calls;
    size_t give_calls;
    uint32_t epoch;
    uint32_t delay_ms_total;

    // **Milliseconds slept with the bus held**, which §10.14.3 says must be
    // zero: "nothing sleeps, retries a network, or draws while holding the
    // bus". A driver that naps through a chip's settling time under a lease
    // drops a touch read and skips a clock tick for nothing, and it is
    // invisible on hardware because it still works.
    uint32_t delay_ms_while_held;

    // **The clock, in microseconds, and it only moves when a test says so.**
    // `esp_timer_get_time()` reads it and `vTaskDelay` advances it — which is
    // not a convenience but a requirement: `Buttons::HeldFor` polls in a loop
    // with a delay between, so a clock that stood still would spin forever and
    // a clock that ran on its own would make "held for five seconds" a race.
    uint64_t clock_us;

    // --- GPIO ---
    int level[kMaxPins];
    // **Whether the world is holding this pin**, as opposed to an internal
    // pull deciding it. A button shorted to ground beats a pull-up, so
    // `gpio_config` must not overwrite a level a test has set — which is
    // exactly §10.15's case: the finger is on `KEY` before `Init` runs.
    bool level_forced[kMaxPins];
    size_t rising_edges[kMaxPins];
    gpio_mode_t last_mode[kMaxPins];

    // --- I2S, for the speaker ---
    // The captured stream is what makes "it played the data and not the
    // header" an assertion rather than a hope. 8 KB holds two of the driver's
    // 4 KB buffer fills, which is more than any test here streams.
    struct I2s {
        static constexpr size_t kMaxCaptured = 8192;

        bool channel_open;
        bool enabled;
        size_t enable_count;
        size_t disable_count;
        bool auto_clear;

        uint32_t sample_rate;
        uint32_t mclk_multiple;
        size_t configure_count;  // init_std_mode
        size_t reconfig_count;   // reconfig_std_clock

        gpio_num_t mclk;
        gpio_num_t bclk;
        gpio_num_t ws;
        gpio_num_t dout;
        gpio_num_t din;
        i2s_data_bit_width_t bits;
        i2s_slot_mode_t slots;

        uint8_t captured[kMaxCaptured];
        size_t captured_length;
        bool captured_overflowed;
        size_t written_total;
        size_t write_calls;

        esp_err_t next_write_error;
    } i2s;

    bool log_enabled;
};

inline constexpr uint8_t kAnyAddress = 0xFF;

Platform &P();

// Call it at the top of every test. `setUp` does, so a test that forgets is
// still clean.
void Reset();

// Puts a chip on the wire at `address`, with all registers zero. Everything
// not added is absent: a transfer to it NACKs, which is what an unpopulated
// address does on a real bus.
Device *AddDevice(uint8_t address);
Device *DeviceAt(uint8_t address);

// The next transfer (to `address`, or to anything) returns `err` instead of
// touching the device. One shot.
void FailNext(esp_err_t err, uint8_t address = kAnyAddress);

// Lets `skip` matching transfers through and fails the one after them. For a
// driver that does several things in a row, "fail the third" is usually the
// interesting case — the one where something has already been changed and has
// to be put back.
void FailAfter(size_t skip, esp_err_t err, uint8_t address = kAnyAddress);

size_t CountTransfers(Transfer::Kind kind);
const Transfer *LastTransfer();

// True when every transfer recorded from `first` onwards shares one epoch —
// "all of this happened under a single lease". Pass the transfer count taken
// before the call under test.
bool OneLeaseSince(size_t first);

// How many distinct leases the transfers from `first` onwards were spread
// across. `OneLeaseSince` is this == 1; the count itself is what a driver with
// a legitimate reason to let go partway — a settling delay it must not sleep
// through under the lease — is asserted against.
size_t CountLeasesSince(size_t first);

// The most recent write of `value` to `reg` on `address`, or nullptr. Reads
// better in a test than walking the log by hand.
const Transfer *FindWrite(uint8_t address, uint8_t reg);
bool WroteRegister(uint8_t address, uint8_t reg, uint8_t value);

// Takes the mutex from outside the Bus, which is how a test plays "another
// task is holding the lease" without threads.
void TakeMutexFromAnotherTask();
void GiveMutexFromAnotherTask();

// Moves the fake clock. `vTaskDelay` does the same thing, so a driver that
// waits does not need help from the test to get there.
void AdvanceMs(uint32_t ms);

// Sets what a pin reads, **without counting an edge**. Edges belong to what
// the firmware drives (`Bus::Recover` clocking SCL); this is the world putting
// a level on an input, which is a different thing and must not be mistaken for
// the first.
void SetPinLevel(gpio_num_t pin, int level);
size_t RisingEdges(gpio_num_t pin);

// The next `i2s_channel_write` fails with `err`. One shot, the same shape the
// I²C injection has.
void FailNextI2sWrite(esp_err_t err);

}  // namespace fake
