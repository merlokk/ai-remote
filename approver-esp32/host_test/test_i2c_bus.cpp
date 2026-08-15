// The leased I²C bus of CLAUDE.md §10.14.3, against the fake wire.
//
// This is the tier §10.14.3 named and then owed for a while: contention, an
// acquire that times out, a recovery after a stuck slave. **The file under test
// is the one that ships** — `components/i2cbus/i2c_bus.cpp`, unmodified; what
// is faked is ESP-IDF underneath it (`fakes/`, which argues why it went that
// way rather than through a backend interface).

#include "fake_platform.h"
#include "i2c_bus.h"
#include "unity.h"

using fake::Transfer;

namespace {

// Two of the board's real addresses, so a failure reads like the board it is
// about. The wiring is `board.h`'s; these are just plausible numbers here.
constexpr uint8_t kPmic = 0x34;
constexpr uint8_t kRtc = 0x51;

constexpr gpio_num_t kScl = GPIO_NUM_7;
constexpr gpio_num_t kSda = GPIO_NUM_8;

// **Each test declares its own `Bus` by value.** There is no "reset it": the
// class deletes its copy assignment because it owns a mutex, which is exactly
// the property §10.14.1 asks of anything holding a resource — so a fresh one
// per test is the only honest way to get a fresh one, and `fake::Reset()` in
// `setUp` clears the wire underneath it.
void BringUp(i2cbus::Bus &bus) {
    TEST_ASSERT_EQUAL_INT(ESP_OK, bus.Init(kScl, kSda));
}

}  // namespace

// --- Coming up -----------------------------------------------------------

void test_init_opens_the_bus_once(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    TEST_ASSERT_TRUE(bus.Ready());
    TEST_ASSERT_EQUAL_UINT(1, fake::P().bus_open_count);

    // Idempotent: a second Init is a no-op rather than a second driver
    // instance, which on the real bus would fail with the port in use.
    TEST_ASSERT_EQUAL_INT(ESP_OK, bus.Init(kScl, kSda));
    TEST_ASSERT_EQUAL_UINT(1, fake::P().bus_open_count);
}

void test_a_bus_that_never_came_up_hands_out_dead_leases(void) {
    i2cbus::Bus bus;
    TEST_ASSERT_FALSE(bus.Ready());

    auto lease = bus.Acquire();
    TEST_ASSERT_FALSE(lease.Held());
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, lease.WriteRegister(kPmic, 0x10, 0x01));
    TEST_ASSERT_EQUAL_UINT(0, fake::P().transfer_count);
}

// --- The lease (§10.14.3's reason for existing) ---------------------------

void test_the_lease_releases_itself_on_every_path(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    fake::AddDevice(kPmic);

    {
        auto lease = bus.Acquire();
        TEST_ASSERT_TRUE(lease.Held());
        TEST_ASSERT_TRUE(fake::P().mutex_taken);
    }  // no explicit release anywhere — that is the point

    TEST_ASSERT_FALSE(fake::P().mutex_taken);
    TEST_ASSERT_EQUAL_UINT(fake::P().take_calls, fake::P().give_calls);
}

void test_a_busy_bus_is_a_skip_and_not_a_block(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    fake::AddDevice(kPmic);

    // Somebody else is mid-sequence.
    fake::TakeMutexFromAnotherTask();

    auto lease = bus.Acquire(20);
    TEST_ASSERT_FALSE(lease.Held());

    // **Bounded, not forever.** The tick count the driver asked for is the
    // thing that says it will come back — a fake that blocked could not tell
    // this apart from `portMAX_DELAY`, which is why it does not block.
    TEST_ASSERT_EQUAL_UINT32(20, fake::P().last_take_ticks);
    TEST_ASSERT_NOT_EQUAL(portMAX_DELAY, fake::P().last_take_ticks);

    // And nothing reached the wire through a lease that was not held.
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, lease.WriteRegister(kPmic, 0x10, 0x01));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, lease.Read(kPmic, nullptr, 0));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, lease.Probe(kPmic));
    TEST_ASSERT_EQUAL_UINT(0, fake::P().transfer_count);
}

void test_a_failed_acquire_does_not_release_somebody_elses_lease(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    fake::TakeMutexFromAnotherTask();

    {
        auto lease = bus.Acquire(5);
        TEST_ASSERT_FALSE(lease.Held());
    }  // destructor runs here

    // Still held by the other task. A guard that released on the way out of a
    // *failed* acquire would hand the bus to whoever asked next, mid-sequence.
    TEST_ASSERT_TRUE(fake::P().mutex_taken);
    TEST_ASSERT_EQUAL_UINT(0, fake::P().give_calls);
}

void test_a_sequence_under_one_lease_is_uninterrupted(void) {
    // The case §10.14.3 says the lease exists for: a read-modify-write must not
    // have another task's transfer land in the middle. Here that is asserted
    // as "the bus was never given back between the three transfers".
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *pmic = fake::AddDevice(kPmic);
    pmic->regs[0x30] = 0b0000'0001;

    {
        auto lease = bus.Acquire();
        uint8_t value = 0;
        TEST_ASSERT_EQUAL_INT(ESP_OK, lease.ReadRegister(kPmic, 0x30, &value, 1));
        value |= 0b0001'0000;
        TEST_ASSERT_EQUAL_INT(ESP_OK, lease.WriteRegister(kPmic, 0x30, value));
        TEST_ASSERT_EQUAL_INT(ESP_OK, lease.ReadRegister(kPmic, 0x30, &value, 1));

        TEST_ASSERT_EQUAL_UINT(0, fake::P().give_calls);
        TEST_ASSERT_EQUAL_HEX8(0b0001'0001, value);
    }

    TEST_ASSERT_EQUAL_UINT(1, fake::P().give_calls);
    TEST_ASSERT_EQUAL_UINT(3, fake::P().transfer_count);
}

// --- What the transfers actually look like on the wire --------------------

void test_write_register_is_one_write_of_two_bytes(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    fake::AddDevice(kPmic);

    auto lease = bus.Acquire();
    TEST_ASSERT_EQUAL_INT(ESP_OK, lease.WriteRegister(kPmic, 0x27, 0x0B));

    const Transfer *t = fake::LastTransfer();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Transfer::Kind::kWrite), static_cast<int>(t->kind));
    TEST_ASSERT_EQUAL_HEX8(kPmic, t->address);
    TEST_ASSERT_EQUAL_UINT(2, t->write_length);
    TEST_ASSERT_EQUAL_HEX8(0x27, t->write[0]);
    TEST_ASSERT_EQUAL_HEX8(0x0B, t->write[1]);
    TEST_ASSERT_EQUAL_HEX8(0x0B, fake::DeviceAt(kPmic)->regs[0x27]);
}

void test_read_register_is_a_single_write_read_not_two_transfers(void) {
    // Two separate transfers would be a repeated-start the bus could break
    // between — the RTC's burst read is the case where that is not academic
    // (§10.8.2: a read that catches a carry mixes two moments).
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *rtc = fake::AddDevice(kRtc);
    rtc->regs[0x04] = 0x11;
    rtc->regs[0x05] = 0x22;
    rtc->regs[0x06] = 0x33;

    auto lease = bus.Acquire();
    uint8_t out[3] = {};
    TEST_ASSERT_EQUAL_INT(ESP_OK, lease.ReadRegister(kRtc, 0x04, out, sizeof(out)));

    TEST_ASSERT_EQUAL_UINT(1, fake::P().transfer_count);
    const Transfer *t = fake::LastTransfer();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Transfer::Kind::kWriteRead),
                          static_cast<int>(t->kind));
    TEST_ASSERT_EQUAL_UINT(1, t->write_length);
    TEST_ASSERT_EQUAL_HEX8(0x04, t->write[0]);
    TEST_ASSERT_EQUAL_UINT(3, t->read_length);
    TEST_ASSERT_EQUAL_HEX8(0x11, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x22, out[1]);
    TEST_ASSERT_EQUAL_HEX8(0x33, out[2]);
}

void test_a_nack_is_handed_back_and_does_not_poison_the_lease(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    fake::AddDevice(kPmic);

    auto lease = bus.Acquire();
    fake::FailNext(ESP_FAIL, kPmic);
    TEST_ASSERT_EQUAL_INT(ESP_FAIL, lease.WriteRegister(kPmic, 0x10, 0x01));

    // The next transfer works, and the lease is still ours: a chip that NACKs
    // once is not a reason to drop the bus.
    TEST_ASSERT_TRUE(lease.Held());
    TEST_ASSERT_EQUAL_INT(ESP_OK, lease.WriteRegister(kPmic, 0x10, 0x02));
    TEST_ASSERT_EQUAL_HEX8(0x02, fake::DeviceAt(kPmic)->regs[0x10]);
}

void test_a_transfer_to_an_empty_address_fails(void) {
    i2cbus::Bus bus;
    BringUp(bus);  // nothing on the wire

    auto lease = bus.Acquire();
    TEST_ASSERT_EQUAL_INT(ESP_FAIL, lease.WriteRegister(kPmic, 0x10, 0x01));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_FOUND, lease.Probe(kPmic));
}

void test_probe_answers_for_a_chip_that_is_there(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    fake::AddDevice(kPmic);

    auto lease = bus.Acquire();
    TEST_ASSERT_EQUAL_INT(ESP_OK, lease.Probe(kPmic));
    // Probing does not open a device handle — it is the bus that answers, and
    // a probe that allocated a slot would fill the table with chips that are
    // not there.
    TEST_ASSERT_EQUAL_UINT(0, fake::P().open_handles);
}

// --- The device table ----------------------------------------------------

void test_a_device_is_opened_once_and_reused(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    fake::AddDevice(kPmic);

    auto lease = bus.Acquire();
    lease.WriteRegister(kPmic, 0x10, 0x01);
    TEST_ASSERT_EQUAL_UINT(1, fake::P().open_handles);

    lease.WriteRegister(kPmic, 0x11, 0x02);
    lease.WriteRegister(kPmic, 0x12, 0x03);
    // Still one handle: the table caches it, which is what makes the second
    // transfer cheap.
    TEST_ASSERT_EQUAL_UINT(1, fake::P().open_handles);
}

void test_speed_is_per_device(void) {
    // §10.14.3 reversed itself on this: the clock lives in the *device* config,
    // so the vendor's 100 kHz AXP2101 does not slow the rest of the wire down.
    i2cbus::Bus bus;
    BringUp(bus);
    fake::AddDevice(kPmic);
    fake::AddDevice(kRtc);

    TEST_ASSERT_EQUAL_INT(ESP_OK, bus.AddDevice(kPmic, 100000));

    auto lease = bus.Acquire();
    lease.WriteRegister(kPmic, 0x10, 0x01);
    TEST_ASSERT_EQUAL_UINT32(100000, fake::LastTransfer()->clock_hz);

    lease.WriteRegister(kRtc, 0x00, 0x00);
    TEST_ASSERT_EQUAL_UINT32(i2cbus::kClockHz, fake::LastTransfer()->clock_hz);
}

void test_changing_a_speed_reopens_the_device(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    fake::AddDevice(kPmic);

    TEST_ASSERT_EQUAL_INT(ESP_OK, bus.AddDevice(kPmic, 100000));
    TEST_ASSERT_EQUAL_UINT(1, fake::P().open_handles);

    // The handle carries the clock, so it cannot be adjusted — it has to be
    // closed and reopened, and the count is what says the old one was closed
    // rather than leaked.
    TEST_ASSERT_EQUAL_INT(ESP_OK, bus.AddDevice(kPmic, 400000));
    TEST_ASSERT_EQUAL_UINT(1, fake::P().open_handles);

    auto lease = bus.Acquire();
    lease.WriteRegister(kPmic, 0x10, 0x01);
    TEST_ASSERT_EQUAL_UINT32(400000, fake::LastTransfer()->clock_hz);
}

void test_asking_for_the_same_speed_twice_changes_nothing(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    fake::AddDevice(kPmic);

    TEST_ASSERT_EQUAL_INT(ESP_OK, bus.AddDevice(kPmic, 100000));
    TEST_ASSERT_EQUAL_INT(ESP_OK, bus.AddDevice(kPmic, 100000));
    TEST_ASSERT_EQUAL_UINT(1, fake::P().open_handles);
}

void test_the_device_table_is_bounded_and_refuses_rather_than_grows(void) {
    // Eight slots for five chips (§10.14.1: full is a designed state). Running
    // out is a wiring mistake, not load, and it says so instead of allocating.
    i2cbus::Bus bus;
    BringUp(bus);

    auto lease = bus.Acquire();
    for (uint8_t i = 0; i < 8; ++i) {
        const uint8_t address = static_cast<uint8_t>(0x20 + i);
        fake::AddDevice(address);
        lease.WriteRegister(address, 0x00, 0x00);
    }
    TEST_ASSERT_EQUAL_UINT(8, fake::P().open_handles);

    fake::AddDevice(0x30);
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NO_MEM, lease.WriteRegister(0x30, 0x00, 0x00));
    TEST_ASSERT_EQUAL_UINT(8, fake::P().open_handles);
}

// --- Recovery ------------------------------------------------------------

void test_recover_clocks_the_bus_free_and_brings_it_back(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    fake::AddDevice(kPmic);

    {
        auto lease = bus.Acquire();
        lease.WriteRegister(kPmic, 0x10, 0x01);
    }
    TEST_ASSERT_EQUAL_UINT(1, fake::P().open_handles);

    TEST_ASSERT_EQUAL_INT(ESP_OK, bus.Recover());

    // The driver was torn down and rebuilt around the pulses.
    TEST_ASSERT_EQUAL_UINT(1, fake::P().bus_delete_count);
    TEST_ASSERT_EQUAL_UINT(2, fake::P().bus_open_count);
    TEST_ASSERT_TRUE(bus.Ready());

    // Nine clocks on SCL: the longest a slave can be mid-byte, and the whole
    // reason this works at all.
    TEST_ASSERT_EQUAL_UINT(9, fake::P().rising_edges[kScl]);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(GPIO_MODE_OUTPUT_OD),
                          static_cast<int>(fake::P().last_mode[kScl]));

    // And the device handles went with the old bus rather than being left
    // dangling — a handle from before the teardown is not usable after it.
    TEST_ASSERT_EQUAL_UINT(0, fake::P().open_handles);
}

void test_the_bus_works_again_after_a_recovery(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *pmic = fake::AddDevice(kPmic);

    TEST_ASSERT_EQUAL_INT(ESP_OK, bus.Recover());

    auto lease = bus.Acquire();
    TEST_ASSERT_TRUE(lease.Held());
    TEST_ASSERT_EQUAL_INT(ESP_OK, lease.WriteRegister(kPmic, 0x40, 0x5A));
    TEST_ASSERT_EQUAL_HEX8(0x5A, pmic->regs[0x40]);
    TEST_ASSERT_EQUAL_UINT(1, fake::P().open_handles);
}

void test_recovering_a_bus_that_never_came_up_is_refused(void) {
    i2cbus::Bus bus;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, bus.Recover());
    TEST_ASSERT_EQUAL_UINT(0, fake::P().rising_edges[kScl]);
}

void RegisterI2cBusTests(void) {
    RUN_TEST(test_init_opens_the_bus_once);
    RUN_TEST(test_a_bus_that_never_came_up_hands_out_dead_leases);

    RUN_TEST(test_the_lease_releases_itself_on_every_path);
    RUN_TEST(test_a_busy_bus_is_a_skip_and_not_a_block);
    RUN_TEST(test_a_failed_acquire_does_not_release_somebody_elses_lease);
    RUN_TEST(test_a_sequence_under_one_lease_is_uninterrupted);

    RUN_TEST(test_write_register_is_one_write_of_two_bytes);
    RUN_TEST(test_read_register_is_a_single_write_read_not_two_transfers);
    RUN_TEST(test_a_nack_is_handed_back_and_does_not_poison_the_lease);
    RUN_TEST(test_a_transfer_to_an_empty_address_fails);
    RUN_TEST(test_probe_answers_for_a_chip_that_is_there);

    RUN_TEST(test_a_device_is_opened_once_and_reused);
    RUN_TEST(test_speed_is_per_device);
    RUN_TEST(test_changing_a_speed_reopens_the_device);
    RUN_TEST(test_asking_for_the_same_speed_twice_changes_nothing);
    RUN_TEST(test_the_device_table_is_bounded_and_refuses_rather_than_grows);

    RUN_TEST(test_recover_clocks_the_bus_free_and_brings_it_back);
    RUN_TEST(test_the_bus_works_again_after_a_recovery);
    RUN_TEST(test_recovering_a_bus_that_never_came_up_is_refused);
}
