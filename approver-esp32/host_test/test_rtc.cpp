// The PCF85063A (CLAUDE.md §10.8.2), against the fake wire.
//
// A clock is the worst thing to get quietly wrong: a BCD nibble swapped, or an
// OS flag believed, produces a time that looks like a time. Two of these tests
// are about §10.8.2's rule that the device would rather show `--:--` than a
// plausible lie, and two are about the burst that keeps a read from mixing two
// moments together.

#include "fake_platform.h"
#include "i2c_bus.h"
#include "pcf85063.h"
#include "unity.h"

namespace {

constexpr uint8_t kAddr = 0x51;
constexpr uint8_t kRegControl1 = 0x00;
constexpr uint8_t kRegSeconds = 0x04;
constexpr uint8_t kOsFlag = 1 << 7;
constexpr uint8_t kStop = 1 << 5;

fake::Device *PutOnWire() { return fake::AddDevice(kAddr); }

void BringUp(i2cbus::Bus &bus) {
    TEST_ASSERT_EQUAL_INT(ESP_OK, bus.Init(GPIO_NUM_7, GPIO_NUM_8));
}

// The seven counters from 04h, as the chip holds them: BCD, and seconds
// carrying the OS flag in bit 7.
void SetClock(fake::Device *chip, uint8_t second, uint8_t minute, uint8_t hour, uint8_t day,
              uint8_t weekday, uint8_t month, uint8_t year_in_century, bool oscillator_ok) {
    auto bcd = [](uint8_t v) {
        return static_cast<uint8_t>(((v / 10) << 4) | (v % 10));
    };
    chip->regs[kRegSeconds] =
        static_cast<uint8_t>(bcd(second) | (oscillator_ok ? 0 : kOsFlag));
    chip->regs[kRegSeconds + 1] = bcd(minute);
    chip->regs[kRegSeconds + 2] = bcd(hour);
    chip->regs[kRegSeconds + 3] = bcd(day);
    chip->regs[kRegSeconds + 4] = weekday;
    chip->regs[kRegSeconds + 5] = bcd(month);
    chip->regs[kRegSeconds + 6] = bcd(year_in_century);
}

}  // namespace

void test_rtc_present_means_it_answered_its_address(void) {
    // This chip has no identity register, so that is all "present" can mean.
    i2cbus::Bus bus;
    BringUp(bus);
    PutOnWire();

    rtc::Pcf85063 clock;
    TEST_ASSERT_EQUAL_INT(ESP_OK, clock.Init(bus));
    TEST_ASSERT_TRUE(clock.Present());
}

void test_rtc_absent_chip_is_not_present_and_refuses_to_be_read(void) {
    i2cbus::Bus bus;
    BringUp(bus);  // nothing at 0x51

    rtc::Pcf85063 clock;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, clock.Init(bus));
    TEST_ASSERT_FALSE(clock.Present());

    rtc::DateTime now = {};
    bool valid = true;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, clock.Read(&now, &valid));
    // **`valid` is cleared before anything else can fail**, so a caller that
    // ignores the return code still cannot read a stale `true`.
    TEST_ASSERT_FALSE(valid);
}

// --- Reading -------------------------------------------------------------

void test_rtc_decodes_bcd_and_adds_the_century(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();
    SetClock(chip, 56, 12, 21, 15, 5, 8, 26, true);

    rtc::Pcf85063 clock;
    clock.Init(bus);

    rtc::DateTime now = {};
    bool valid = false;
    TEST_ASSERT_EQUAL_INT(ESP_OK, clock.Read(&now, &valid));
    TEST_ASSERT_TRUE(valid);
    TEST_ASSERT_EQUAL_UINT16(2026, now.year);
    TEST_ASSERT_EQUAL_UINT8(8, now.month);
    TEST_ASSERT_EQUAL_UINT8(15, now.day);
    TEST_ASSERT_EQUAL_UINT8(21, now.hour);
    TEST_ASSERT_EQUAL_UINT8(12, now.minute);
    TEST_ASSERT_EQUAL_UINT8(56, now.second);
    TEST_ASSERT_EQUAL_UINT8(5, now.weekday);
}

void test_rtc_reads_all_seven_counters_in_one_burst(void) {
    // §10.8.2: a read freezes the counters, so **one** access cannot catch a
    // carry — two can, and would hand back the minutes from one moment and the
    // hours from the next. Asserted as the transfer count, which is the only
    // place that difference is visible.
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();
    SetClock(chip, 0, 0, 0, 1, 0, 1, 26, true);

    rtc::Pcf85063 clock;
    clock.Init(bus);

    const size_t before = fake::P().transfer_count;
    rtc::DateTime now = {};
    bool valid = false;
    TEST_ASSERT_EQUAL_INT(ESP_OK, clock.Read(&now, &valid));

    TEST_ASSERT_EQUAL_UINT(1, fake::P().transfer_count - before);
    const fake::Transfer *t = fake::LastTransfer();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(fake::Transfer::Kind::kWriteRead),
                          static_cast<int>(t->kind));
    TEST_ASSERT_EQUAL_HEX8(kRegSeconds, t->write[0]);
    TEST_ASSERT_EQUAL_UINT(7, t->read_length);
}

void test_rtc_the_os_flag_makes_a_successful_read_untrustworthy(void) {
    // **The two answers are separate**: the transfer worked and the value is
    // not to be believed. That is what lets §10.8.2 show `--:--` instead of a
    // plausible wrong time, and it is why `Read` returns ESP_OK here.
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();
    SetClock(chip, 30, 45, 12, 20, 3, 6, 25, false);  // oscillator stopped

    rtc::Pcf85063 clock;
    clock.Init(bus);

    rtc::DateTime now = {};
    bool valid = true;
    TEST_ASSERT_EQUAL_INT(ESP_OK, clock.Read(&now, &valid));
    TEST_ASSERT_FALSE(valid);
}

void test_rtc_the_os_flag_is_masked_out_of_the_seconds(void) {
    // Bit 7 lives in the same register as the tens of seconds. Reading it as
    // part of the number gives 30 + 80, which is not obviously wrong at a
    // glance — and would have been believed if the flag had not also been
    // checked. Here the flag is set *and* the seconds still decode.
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();
    SetClock(chip, 30, 0, 0, 1, 0, 1, 26, false);

    rtc::Pcf85063 clock;
    clock.Init(bus);

    rtc::DateTime now = {};
    bool valid = true;
    clock.Read(&now, &valid);
    TEST_ASSERT_EQUAL_UINT8(30, now.second);
}

void test_rtc_a_date_that_is_not_a_date_is_not_valid(void) {
    // Corruption that the OS flag does not catch: the chip is running and the
    // counters are nonsense. Month 19 comes out of a BCD nibble nobody wrote.
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();
    SetClock(chip, 0, 0, 0, 1, 0, 19, 26, true);

    rtc::Pcf85063 clock;
    clock.Init(bus);

    rtc::DateTime now = {};
    bool valid = true;
    TEST_ASSERT_EQUAL_INT(ESP_OK, clock.Read(&now, &valid));
    TEST_ASSERT_FALSE(valid);
}

// --- Writing -------------------------------------------------------------

void test_rtc_write_stops_the_clock_around_the_counters(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();
    chip->regs[kRegControl1] = 0b0000'0001;  // something else set, must survive

    rtc::Pcf85063 clock;
    clock.Init(bus);

    const size_t before = fake::P().transfer_count;
    const rtc::DateTime when = {2026, 8, 16, 0, 9, 30, 15};
    TEST_ASSERT_EQUAL_INT(ESP_OK, clock.Write(when));

    // Read control_1, stop, write seven, start: four transfers, one lease.
    TEST_ASSERT_EQUAL_UINT(4, fake::P().transfer_count - before);
    TEST_ASSERT_TRUE(fake::OneLeaseSince(before));

    // Stopped, then started again — and the unrelated bit survived both.
    TEST_ASSERT_EQUAL_HEX8(0b0000'0001, chip->regs[kRegControl1]);

    const fake::Transfer &stop = fake::P().transfers[before + 1];
    TEST_ASSERT_EQUAL_HEX8(kRegControl1, stop.write[0]);
    TEST_ASSERT_EQUAL_HEX8(0b0010'0001, stop.write[1]);
    TEST_ASSERT_EQUAL_HEX8(kStop, stop.write[1] & kStop);
}

void test_rtc_write_encodes_bcd_and_clears_the_os_flag(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();
    SetClock(chip, 0, 0, 0, 1, 0, 1, 20, false);  // OS set before

    rtc::Pcf85063 clock;
    clock.Init(bus);

    const rtc::DateTime when = {2026, 12, 25, 4, 23, 59, 45};
    TEST_ASSERT_EQUAL_INT(ESP_OK, clock.Write(when));

    TEST_ASSERT_EQUAL_HEX8(0x45, chip->regs[kRegSeconds]);
    TEST_ASSERT_EQUAL_HEX8(0x59, chip->regs[kRegSeconds + 1]);
    TEST_ASSERT_EQUAL_HEX8(0x23, chip->regs[kRegSeconds + 2]);
    TEST_ASSERT_EQUAL_HEX8(0x25, chip->regs[kRegSeconds + 3]);
    TEST_ASSERT_EQUAL_HEX8(0x04, chip->regs[kRegSeconds + 4]);
    TEST_ASSERT_EQUAL_HEX8(0x12, chip->regs[kRegSeconds + 5]);
    TEST_ASSERT_EQUAL_HEX8(0x26, chip->regs[kRegSeconds + 6]);

    // **Writing seconds is what clears OS**, so a successful write is what
    // makes the clock trustworthy again — §10.8.2 leans on this.
    TEST_ASSERT_EQUAL_HEX8(0, chip->regs[kRegSeconds] & kOsFlag);
}

void test_rtc_the_counters_go_out_in_one_write(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    PutOnWire();

    rtc::Pcf85063 clock;
    clock.Init(bus);

    const size_t before = fake::P().transfer_count;
    const rtc::DateTime when = {2026, 8, 16, 0, 9, 30, 15};
    clock.Write(when);

    // Index 2 is the counter write: one transfer of a register address plus
    // seven values, not seven register writes that the clock could tick
    // between even while stopped.
    const fake::Transfer &counters = fake::P().transfers[before + 2];
    TEST_ASSERT_EQUAL_INT(static_cast<int>(fake::Transfer::Kind::kWrite),
                          static_cast<int>(counters.kind));
    TEST_ASSERT_EQUAL_UINT(8, counters.write_length);
    TEST_ASSERT_EQUAL_HEX8(kRegSeconds, counters.write[0]);
}

void test_rtc_a_write_that_fails_still_restarts_the_clock(void) {
    // Leaving it stopped would turn one bad write into a dead clock, which is
    // a much worse failure than the one that caused it.
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();
    chip->regs[kRegControl1] = 0;

    rtc::Pcf85063 clock;
    clock.Init(bus);

    // `Write` does four things: read control_1, stop, write the counters,
    // start. Let the first two through and fail the third — the state where
    // the clock has been stopped and the write it was stopped for did not
    // happen.
    fake::FailAfter(2, ESP_FAIL, kAddr);

    const rtc::DateTime when = {2026, 8, 16, 0, 9, 30, 15};
    TEST_ASSERT_EQUAL_INT(ESP_FAIL, clock.Write(when));

    // The failure is reported *and* the clock is running again.
    TEST_ASSERT_EQUAL_HEX8(0, chip->regs[kRegControl1] & kStop);
}

void test_rtc_refuses_a_date_that_is_not_a_date(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    PutOnWire();

    rtc::Pcf85063 clock;
    clock.Init(bus);

    const size_t before = fake::P().transfer_count;
    const rtc::DateTime bad = {2026, 13, 1, 0, 0, 0, 0};
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, clock.Write(bad));
    // Rejected before the bus was touched — the clock is not stopped for a
    // write that was never going to happen.
    TEST_ASSERT_EQUAL_UINT(before, fake::P().transfer_count);
}

void test_rtc_round_trips(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();
    SetClock(chip, 0, 0, 0, 1, 0, 1, 20, false);

    rtc::Pcf85063 clock;
    clock.Init(bus);

    const rtc::DateTime when = {2026, 8, 16, 0, 9, 30, 15};
    TEST_ASSERT_EQUAL_INT(ESP_OK, clock.Write(when));

    rtc::DateTime back = {};
    bool valid = false;
    TEST_ASSERT_EQUAL_INT(ESP_OK, clock.Read(&back, &valid));
    TEST_ASSERT_TRUE(valid);
    TEST_ASSERT_EQUAL_UINT16(when.year, back.year);
    TEST_ASSERT_EQUAL_UINT8(when.month, back.month);
    TEST_ASSERT_EQUAL_UINT8(when.day, back.day);
    TEST_ASSERT_EQUAL_UINT8(when.hour, back.hour);
    TEST_ASSERT_EQUAL_UINT8(when.minute, back.minute);
    TEST_ASSERT_EQUAL_UINT8(when.second, back.second);
}

void RegisterRtcTests(void) {
    RUN_TEST(test_rtc_present_means_it_answered_its_address);
    RUN_TEST(test_rtc_absent_chip_is_not_present_and_refuses_to_be_read);

    RUN_TEST(test_rtc_decodes_bcd_and_adds_the_century);
    RUN_TEST(test_rtc_reads_all_seven_counters_in_one_burst);
    RUN_TEST(test_rtc_the_os_flag_makes_a_successful_read_untrustworthy);
    RUN_TEST(test_rtc_the_os_flag_is_masked_out_of_the_seconds);
    RUN_TEST(test_rtc_a_date_that_is_not_a_date_is_not_valid);

    RUN_TEST(test_rtc_write_stops_the_clock_around_the_counters);
    RUN_TEST(test_rtc_write_encodes_bcd_and_clears_the_os_flag);
    RUN_TEST(test_rtc_the_counters_go_out_in_one_write);
    RUN_TEST(test_rtc_a_write_that_fails_still_restarts_the_clock);
    RUN_TEST(test_rtc_refuses_a_date_that_is_not_a_date);
    RUN_TEST(test_rtc_round_trips);
}
