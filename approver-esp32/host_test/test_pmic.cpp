// The AXP2101 (CLAUDE.md §10.1, §10.13), against the fake wire.
//
// The driver is the first chip on this board that has to work — it holds the
// panel's reset and the amplifier's enable — and almost everything it does is
// arithmetic on register bits, which is exactly the class of thing that is
// plausible when wrong. A 13-bit field read as 14 bits gives a battery voltage
// that looks like a battery voltage.
//
// Two of these tests are about the lease rather than the chip, and they are the
// ones §10.14.3 is really about: `Read` is a snapshot, and `PowerOff` decides
// and writes without letting go in between.

#include "axp2101.h"
#include "fake_platform.h"
#include "i2c_bus.h"
#include "unity.h"

namespace {

constexpr uint8_t kAddr = 0x34;

constexpr uint8_t kRegStatus1 = 0x00;
constexpr uint8_t kRegStatus2 = 0x01;
constexpr uint8_t kRegChipId = 0x03;
constexpr uint8_t kRegAdcChannelCtrl = 0x30;
constexpr uint8_t kRegAdcBatteryHigh = 0x34;
constexpr uint8_t kRegAdcVbusHigh = 0x38;
constexpr uint8_t kRegAdcSystemHigh = 0x3A;
constexpr uint8_t kRegAdcDieHigh = 0x3C;
constexpr uint8_t kRegCommonConfig = 0x10;
constexpr uint8_t kRegKeyLevelCtrl = 0x27;
constexpr uint8_t kRegTsPinCtrl = 0x50;
constexpr uint8_t kRegLdoOnOff0 = 0x90;
constexpr uint8_t kRegBatteryPercent = 0xA4;

constexpr uint8_t kStatus1BatteryPresent = 1 << 3;
constexpr uint8_t kStatus1VbusGood = 1 << 5;
constexpr uint8_t kStatus2VbusUnusable = 1 << 3;

// A chip that answers as an AXP2101 and is otherwise blank. Tests set the
// registers they care about on top.
fake::Device *PutOnWire() {
    fake::Device *chip = fake::AddDevice(kAddr);
    chip->regs[kRegChipId] = 0x4A;
    return chip;
}

void BringUp(i2cbus::Bus &bus) {
    TEST_ASSERT_EQUAL_INT(ESP_OK, bus.Init(GPIO_NUM_7, GPIO_NUM_8));
}

}  // namespace

// --- Identity ------------------------------------------------------------

void test_pmic_init_identifies_the_chip(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    PutOnWire();

    pmic::Axp2101 axp;
    TEST_ASSERT_EQUAL_INT(ESP_OK, axp.Init(bus));
    TEST_ASSERT_TRUE(axp.Present());
}

void test_pmic_refuses_a_chip_that_is_not_an_axp2101(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();
    chip->regs[kRegChipId] = 0x4B;  // one bit off, which is the realistic case

    pmic::Axp2101 axp;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_SUPPORTED, axp.Init(bus));
    TEST_ASSERT_FALSE(axp.Present());

    // And a driver that is not present refuses to be read rather than handing
    // back a zeroed Status that reads as a flat battery.
    pmic::Status status = {};
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, axp.Read(&status));
}

void test_pmic_survives_an_empty_address(void) {
    i2cbus::Bus bus;
    BringUp(bus);  // nothing on the wire

    pmic::Axp2101 axp;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, axp.Init(bus));
    TEST_ASSERT_FALSE(axp.Present());
}

void test_pmic_runs_at_its_own_100khz(void) {
    // §10.14.3: the clock is per device, so the vendor's slow AXP2101 does not
    // bring the rest of the wire down with it.
    i2cbus::Bus bus;
    BringUp(bus);
    PutOnWire();

    pmic::Axp2101 axp;
    axp.Init(bus);
    TEST_ASSERT_EQUAL_UINT32(100000, fake::LastTransfer()->clock_hz);
}

// --- What Init actually configures ---------------------------------------

void test_pmic_init_silences_the_ts_pin(void) {
    // XPowersLib does this inside begin() and says why: with TS measurement on,
    // the pin is read as a battery thermistor and affects the charger. Nothing
    // is wired to it on this board.
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();
    chip->regs[kRegTsPinCtrl] = 0x0F;  // low nibble set, so the mask is visible

    pmic::Axp2101 axp;
    axp.Init(bus);

    // The high nibble is preserved and the low one becomes 0x10's bits — a
    // read-modify-write, not a blind write of 0x10.
    TEST_ASSERT_TRUE(fake::WroteRegister(kAddr, kRegTsPinCtrl, 0x10));
}

void test_pmic_init_turns_on_the_adc_channels_and_off_the_ts_one(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();
    // Something already enabled, including TS, so both halves are observable.
    chip->regs[kRegAdcChannelCtrl] = 0b0000'0010;

    pmic::Axp2101 axp;
    axp.Init(bus);

    const fake::Transfer *t = fake::FindWrite(kAddr, kRegAdcChannelCtrl);
    TEST_ASSERT_NOT_NULL(t);
    const uint8_t written = t->write[1];
    TEST_ASSERT_EQUAL_HEX8(0b0001'1101, written);
    // Said again as the two facts rather than as the constant, so a changed
    // channel list fails with a readable message.
    TEST_ASSERT_EQUAL_HEX8(0, written & 0b0000'0010);  // TS off
    TEST_ASSERT_EQUAL_HEX8(0b0001'1101, written & 0b0001'1101);
}

void test_pmic_init_is_one_uninterrupted_sequence(void) {
    // The read-modify-writes above are only safe because nothing else can get
    // the bus between the read and the write. `AddDevice` takes its own lease
    // first — that one is expected; everything after it is one.
    i2cbus::Bus bus;
    BringUp(bus);
    PutOnWire();

    const size_t before = fake::P().transfer_count;
    pmic::Axp2101 axp;
    TEST_ASSERT_EQUAL_INT(ESP_OK, axp.Init(bus));

    TEST_ASSERT_TRUE(fake::P().transfer_count > before);
    TEST_ASSERT_TRUE(fake::OneLeaseSince(before));
}

// --- Reading it ----------------------------------------------------------

void test_pmic_read_decodes_the_status_bits(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();
    chip->regs[kRegStatus1] = kStatus1BatteryPresent | kStatus1VbusGood;
    chip->regs[kRegStatus2] = (0x01 << 5) | 0x02;  // charging, constant current

    pmic::Axp2101 axp;
    axp.Init(bus);

    pmic::Status status = {};
    TEST_ASSERT_EQUAL_INT(ESP_OK, axp.Read(&status));
    TEST_ASSERT_TRUE(status.battery_present);
    TEST_ASSERT_TRUE(status.vbus_present);
    TEST_ASSERT_TRUE(status.charging);
    TEST_ASSERT_FALSE(status.discharging);
    TEST_ASSERT_EQUAL_UINT8(2, status.charge_code);
    TEST_ASSERT_EQUAL_STRING("constant current", pmic::Axp2101::ChargeStateName(2));
}

void test_pmic_vbus_needs_both_bits(void) {
    // "Good" alone is not enough: STATUS2 bit 3 says the supply is unusable,
    // and a cable that cannot deliver reads as present on STATUS1 only. This
    // is also what `PowerOff` leans on, so getting it wrong turns a refusal
    // into a shutdown.
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();
    chip->regs[kRegStatus1] = kStatus1VbusGood;
    chip->regs[kRegStatus2] = kStatus2VbusUnusable;

    pmic::Axp2101 axp;
    axp.Init(bus);

    pmic::Status status = {};
    TEST_ASSERT_EQUAL_INT(ESP_OK, axp.Read(&status));
    TEST_ASSERT_FALSE(status.vbus_present);
}

void test_pmic_battery_is_thirteen_bits_and_the_rest_are_fourteen(void) {
    // **The widths are not uniform**, and a battery read as 14 bits is a
    // plausible wrong voltage rather than an obvious one. Both high bytes are
    // 0xFF here, so only the mask decides the answer.
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();
    chip->regs[kRegStatus1] = kStatus1BatteryPresent | kStatus1VbusGood;

    chip->regs[kRegAdcBatteryHigh] = 0xFF;
    chip->regs[kRegAdcBatteryHigh + 1] = 0x34;
    chip->regs[kRegAdcVbusHigh] = 0xFF;
    chip->regs[kRegAdcVbusHigh + 1] = 0x12;
    chip->regs[kRegAdcSystemHigh] = 0xFF;
    chip->regs[kRegAdcSystemHigh + 1] = 0x56;

    pmic::Axp2101 axp;
    axp.Init(bus);

    pmic::Status status = {};
    TEST_ASSERT_EQUAL_INT(ESP_OK, axp.Read(&status));
    TEST_ASSERT_EQUAL_UINT16((0x1F << 8) | 0x34, status.battery_mv);
    TEST_ASSERT_EQUAL_UINT16((0x3F << 8) | 0x12, status.vbus_mv);
    TEST_ASSERT_EQUAL_UINT16((0x3F << 8) | 0x56, status.system_mv);
}

void test_pmic_die_temperature_converts(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();
    // 22 + (7274 - raw)/20, so raw 7274 is exactly 22 C and 6874 is 42 C.
    chip->regs[kRegAdcDieHigh] = static_cast<uint8_t>(6874 >> 8);
    chip->regs[kRegAdcDieHigh + 1] = static_cast<uint8_t>(6874 & 0xFF);

    pmic::Axp2101 axp;
    axp.Init(bus);

    pmic::Status status = {};
    TEST_ASSERT_EQUAL_INT(ESP_OK, axp.Read(&status));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 42.0f, status.die_celsius);
}

void test_pmic_reports_no_percentage_when_there_is_no_battery(void) {
    // -1 rather than 0: a device on USB with no cell has no charge level, and
    // printing 0 % is the same mistake §10.8.2 refuses to make with the clock.
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();
    chip->regs[kRegStatus1] = 0;                 // no battery
    chip->regs[kRegBatteryPercent] = 77;         // and the register lies anyway

    pmic::Axp2101 axp;
    axp.Init(bus);

    pmic::Status status = {};
    TEST_ASSERT_EQUAL_INT(ESP_OK, axp.Read(&status));
    TEST_ASSERT_EQUAL_INT(-1, status.battery_percent);
    TEST_ASSERT_EQUAL_UINT16(0, status.battery_mv);
}

void test_pmic_ignores_an_impossible_percentage(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();
    chip->regs[kRegStatus1] = kStatus1BatteryPresent;
    chip->regs[kRegBatteryPercent] = 200;

    pmic::Axp2101 axp;
    axp.Init(bus);

    pmic::Status status = {};
    TEST_ASSERT_EQUAL_INT(ESP_OK, axp.Read(&status));
    TEST_ASSERT_EQUAL_INT(-1, status.battery_percent);
}

void test_pmic_reads_the_button_timings_off_the_chip(void) {
    // §10.1 gets these numbers from the board rather than the datasheet's
    // defaults, which is only possible because they are read back.
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();
    chip->regs[kRegKeyLevelCtrl] = 0b0000'0100;  // press-off 01 (6 s), press-on 00 (128 ms)
    chip->regs[kRegCommonConfig] = 0b0000'0100;  // long press acts

    pmic::Axp2101 axp;
    axp.Init(bus);

    pmic::Status status = {};
    TEST_ASSERT_EQUAL_INT(ESP_OK, axp.Read(&status));
    TEST_ASSERT_EQUAL_UINT8(0, status.press_on_code);
    TEST_ASSERT_EQUAL_UINT8(1, status.press_off_code);
    TEST_ASSERT_TRUE(status.long_press_shutdown);
    TEST_ASSERT_EQUAL_STRING("128 ms", pmic::PressOnTimeName(0));
    TEST_ASSERT_EQUAL_STRING("6 s", pmic::PressOffTimeName(1));
}

void test_pmic_read_is_one_snapshot(void) {
    // **The point of the lease, asserted.** `power` prints a dozen numbers; if
    // they came from a dozen leases they would be a dozen moments, and a
    // battery voltage next to a charge state that disagrees with it is a bug
    // report nobody can reproduce.
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();
    chip->regs[kRegStatus1] = kStatus1BatteryPresent | kStatus1VbusGood;

    pmic::Axp2101 axp;
    axp.Init(bus);

    const size_t before = fake::P().transfer_count;
    pmic::Status status = {};
    TEST_ASSERT_EQUAL_INT(ESP_OK, axp.Read(&status));

    TEST_ASSERT_TRUE(fake::P().transfer_count - before > 8);
    TEST_ASSERT_TRUE(fake::OneLeaseSince(before));
}

void test_pmic_read_gives_up_when_the_bus_is_busy(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    PutOnWire();

    pmic::Axp2101 axp;
    axp.Init(bus);

    fake::TakeMutexFromAnotherTask();
    const size_t before = fake::P().transfer_count;

    pmic::Status status = {};
    TEST_ASSERT_EQUAL_INT(ESP_ERR_TIMEOUT, axp.Read(&status));
    // And it did not half-read: nothing went out at all.
    TEST_ASSERT_EQUAL_UINT(before, fake::P().transfer_count);
}

// --- The rails (§10.1: the panel's reset and the amplifier's enable) ------

void test_pmic_rails_set_only_their_own_bit(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();

    pmic::Axp2101 axp;
    axp.Init(bus);

    // Something else on the same register, which must survive: ALDO1 is a rail
    // this firmware does not own.
    chip->regs[kRegLdoOnOff0] = 0b0000'0001;

    TEST_ASSERT_EQUAL_INT(ESP_OK, axp.SetAldo3(true));
    TEST_ASSERT_EQUAL_HEX8(0b0000'0101, chip->regs[kRegLdoOnOff0]);

    TEST_ASSERT_EQUAL_INT(ESP_OK, axp.SetAldo2(true));
    TEST_ASSERT_EQUAL_HEX8(0b0000'0111, chip->regs[kRegLdoOnOff0]);

    // And off again, still without touching ALDO1 — this is the sequence the
    // panel's reset actually performs (§10.1).
    TEST_ASSERT_EQUAL_INT(ESP_OK, axp.SetAldo3(false));
    TEST_ASSERT_EQUAL_HEX8(0b0000'0011, chip->regs[kRegLdoOnOff0]);
}

void test_pmic_reports_which_rails_are_on(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();

    pmic::Axp2101 axp;
    axp.Init(bus);
    chip->regs[kRegLdoOnOff0] = 0b0000'0010;  // ALDO2 only

    pmic::Status status = {};
    TEST_ASSERT_EQUAL_INT(ESP_OK, axp.Read(&status));
    TEST_ASSERT_TRUE(status.aldo2_enabled);
    TEST_ASSERT_FALSE(status.aldo3_enabled);
}

// --- Power off (§10.7's refusal, which is a driver rule) -----------------

void test_pmic_power_off_is_refused_over_usb_and_writes_nothing(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();
    chip->regs[kRegStatus1] = kStatus1VbusGood;
    chip->regs[kRegStatus2] = 0;  // usable, so VBUS really is present

    pmic::Axp2101 axp;
    axp.Init(bus);
    const uint8_t common_before = chip->regs[kRegCommonConfig];

    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, axp.PowerOff());

    // **Writes nothing** — the half of the refusal that matters. A driver that
    // refused after arming the bit would be worse than one that did not refuse.
    TEST_ASSERT_EQUAL_HEX8(common_before, chip->regs[kRegCommonConfig]);
}

void test_pmic_power_off_arms_the_bit_on_battery(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();
    chip->regs[kRegStatus1] = kStatus1BatteryPresent;  // no VBUS
    chip->regs[kRegCommonConfig] = 0b0000'0100;        // long-press bit, must survive

    pmic::Axp2101 axp;
    axp.Init(bus);

    const size_t before = fake::P().transfer_count;
    TEST_ASSERT_EQUAL_INT(ESP_OK, axp.PowerOff());

    TEST_ASSERT_EQUAL_HEX8(0b0000'0101, chip->regs[kRegCommonConfig]);
    // The check and the write under one lease: a cable plugged in between them
    // would otherwise be a shutdown taken on a stale reading.
    TEST_ASSERT_TRUE(fake::OneLeaseSince(before));
}

void test_pmic_power_off_before_init_is_refused(void) {
    pmic::Axp2101 axp;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, axp.PowerOff());
    TEST_ASSERT_EQUAL_UINT(0, fake::P().transfer_count);
}

void RegisterPmicTests(void) {
    RUN_TEST(test_pmic_init_identifies_the_chip);
    RUN_TEST(test_pmic_refuses_a_chip_that_is_not_an_axp2101);
    RUN_TEST(test_pmic_survives_an_empty_address);
    RUN_TEST(test_pmic_runs_at_its_own_100khz);

    RUN_TEST(test_pmic_init_silences_the_ts_pin);
    RUN_TEST(test_pmic_init_turns_on_the_adc_channels_and_off_the_ts_one);
    RUN_TEST(test_pmic_init_is_one_uninterrupted_sequence);

    RUN_TEST(test_pmic_read_decodes_the_status_bits);
    RUN_TEST(test_pmic_vbus_needs_both_bits);
    RUN_TEST(test_pmic_battery_is_thirteen_bits_and_the_rest_are_fourteen);
    RUN_TEST(test_pmic_die_temperature_converts);
    RUN_TEST(test_pmic_reports_no_percentage_when_there_is_no_battery);
    RUN_TEST(test_pmic_ignores_an_impossible_percentage);
    RUN_TEST(test_pmic_reads_the_button_timings_off_the_chip);
    RUN_TEST(test_pmic_read_is_one_snapshot);
    RUN_TEST(test_pmic_read_gives_up_when_the_bus_is_busy);

    RUN_TEST(test_pmic_rails_set_only_their_own_bit);
    RUN_TEST(test_pmic_reports_which_rails_are_on);

    RUN_TEST(test_pmic_power_off_is_refused_over_usb_and_writes_nothing);
    RUN_TEST(test_pmic_power_off_arms_the_bit_on_battery);
    RUN_TEST(test_pmic_power_off_before_init_is_refused);
}
