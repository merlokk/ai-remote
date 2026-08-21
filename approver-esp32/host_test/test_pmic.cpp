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

void test_pmic_leaves_a_rail_alone_when_it_is_already_at_its_voltage(void) {
    // **The vendor's `if (getXxxVoltage() != 3300)` guard, and it is not
    // cosmetic**: DCDC1 supplies the C6 itself, so a write that changes
    // nothing is a write that cannot disturb the rail the firmware is running
    // on. 3300 mV encodes as (3300-1500)/100 = 18 for DC1 and (3300-500)/100 =
    // 28 for an ALDO.
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();
    chip->regs[0x82] = 18;  // DC1 already at 3300 mV
    chip->regs[0x92] = 28;  // ALDO1..4 likewise
    chip->regs[0x93] = 28;
    chip->regs[0x94] = 28;
    chip->regs[0x95] = 28;

    pmic::Axp2101 axp;
    TEST_ASSERT_EQUAL_INT(ESP_OK, axp.Init(bus));

    // Read, yes; written, no.
    TEST_ASSERT_NULL(fake::FindWrite(kAddr, 0x82));
    TEST_ASSERT_NULL(fake::FindWrite(kAddr, 0x92));
    TEST_ASSERT_NULL(fake::FindWrite(kAddr, 0x95));
}

void test_pmic_init_writes_a_rail_that_is_at_the_wrong_voltage(void) {
    // The other side of the guard — otherwise "never writes" would pass by
    // never configuring anything, which is the defect §10.1 records finding in
    // this driver against the vendor's `pmicpower`.
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();
    chip->regs[0x94] = static_cast<uint8_t>(0xE0 | 10);  // ALDO3 at 1500 mV, plus bits to keep

    pmic::Axp2101 axp;
    TEST_ASSERT_EQUAL_INT(ESP_OK, axp.Init(bus));

    // 3300 mV, and the three bits above the voltage field untouched — they are
    // not this driver's to clear.
    TEST_ASSERT_EQUAL_HEX8(28, chip->regs[0x94] & 0x1F);
    TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(0xE0), chip->regs[0x94] & 0xE0);
}

void test_pmic_init_programs_this_batterys_charge_currents(void) {
    // §10.1: "the charge currents left at power-on defaults rather than this
    // battery's 50/500/50 mA" was one of four things reading the vendor's
    // component found missing from a driver that already worked. Each value
    // shares a register with something else, so each is a masked
    // read-modify-write and a blind write would be a way to change a
    // neighbouring field by accident.
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();
    chip->regs[0x61] = 0xFC;  // the bits each mask promises to keep…
    chip->regs[0x62] = 0xE0;
    chip->regs[0x63] = 0xF0;

    pmic::Axp2101 axp;
    pmic::Config config;
    TEST_ASSERT_EQUAL_INT(ESP_OK, axp.Init(bus, config));

    TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(0xFC | static_cast<uint8_t>(config.precharge)),
                           chip->regs[0x61]);
    TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(0xE0 | static_cast<uint8_t>(config.charge)),
                           chip->regs[0x62]);
    TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(0xF0 | static_cast<uint8_t>(config.termination)),
                           chip->regs[0x63]);
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

// --- The power key, written rather than read (§10.1) ---------------------
//
// **The registers that decide whether the button on the case can switch this
// board on.** The driver used to print what it found in them; these say it puts
// them there. The one that matters most is the last but one: configuring the key
// must never write COMMON_CONFIG bit 0, because that bit is the soft power-off
// and a device that switches itself off inside `Init` is a device that goes dark
// and stays there.

void test_pmic_init_writes_the_power_key_when_the_chip_holds_something_else(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();
    // Press-on 2 s, press-off 10 s: a board whose button looks dead to anybody
    // who presses it the way a button is pressed.
    chip->regs[kRegKeyLevelCtrl] = 0b0000'1111;

    pmic::Axp2101 axp;
    TEST_ASSERT_EQUAL_INT(ESP_OK, axp.Init(bus));

    // 128 ms on, 6 s off.
    TEST_ASSERT_EQUAL_HEX8(0b0000'0100, chip->regs[kRegKeyLevelCtrl]);
}

void test_pmic_init_leaves_a_power_key_that_is_already_right_alone(void) {
    // The vendor's rail guard, applied here: a write that changes nothing is a
    // write that cannot go wrong, and the count is how that is checked.
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();
    chip->regs[kRegKeyLevelCtrl] = 0b0000'0100;
    chip->regs[kRegCommonConfig] = 0b0000'0100;

    pmic::Axp2101 axp;
    TEST_ASSERT_EQUAL_INT(ESP_OK, axp.Init(bus));

    TEST_ASSERT_NULL(fake::FindWrite(kAddr, kRegKeyLevelCtrl));
    TEST_ASSERT_NULL(fake::FindWrite(kAddr, kRegCommonConfig));
}

void test_pmic_init_keeps_the_bits_of_0x27_it_does_not_own(void) {
    // Four bits of that register are this driver's and the rest are the chip's.
    // A blind write is how a field nobody thought about gets cleared.
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();
    chip->regs[kRegKeyLevelCtrl] = 0b1011'0011;

    pmic::Axp2101 axp;
    TEST_ASSERT_EQUAL_INT(ESP_OK, axp.Init(bus));

    // The cast is Unity's fault rather than the test's: `TEST_ASSERT_EQUAL_HEX8`
    // takes a signed byte, and anything above 127 is a truncation warning that
    // `/W4 /WX` turns into a build error.
    TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(0b1011'0100), chip->regs[kRegKeyLevelCtrl]);
}

void test_pmic_init_turns_the_long_press_shutdown_on(void) {
    // Without this bit the chip measures the six-second press and does nothing,
    // which is a board that cannot be switched off by its own button.
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();
    chip->regs[kRegCommonConfig] = 0b0010'0000;  // some other bit set, and not ours

    pmic::Axp2101 axp;
    TEST_ASSERT_EQUAL_INT(ESP_OK, axp.Init(bus));

    TEST_ASSERT_EQUAL_HEX8(0b0010'0100, chip->regs[kRegCommonConfig]);
}

void test_pmic_init_never_writes_the_soft_power_off_bit(void) {
    // **The one that matters.** COMMON_CONFIG bit 0 switches the board off, and
    // it shares its register with the long-press enable this now writes. A
    // read-modify-write that preserved it would take the device down inside
    // `Init` — and the operator would see a board that goes dark on every boot.
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();
    chip->regs[kRegCommonConfig] = 0;  // the long-press bit is clear, so it writes

    pmic::Axp2101 axp;
    TEST_ASSERT_EQUAL_INT(ESP_OK, axp.Init(bus));

    TEST_ASSERT_EQUAL_HEX8(0, chip->regs[kRegCommonConfig] & 0x01);
}

void test_pmic_init_clears_a_soft_power_off_bit_it_finds_set(void) {
    // Finding it set at boot should not happen — it should have taken the board
    // off. Leaving it there would arm the next write of that register.
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();
    chip->regs[kRegCommonConfig] = 0b0000'0101;  // long press already right, plus the bit

    pmic::Axp2101 axp;
    TEST_ASSERT_EQUAL_INT(ESP_OK, axp.Init(bus));

    TEST_ASSERT_EQUAL_HEX8(0b0000'0100, chip->regs[kRegCommonConfig]);
}

void test_pmic_the_power_key_can_be_configured_the_other_way(void) {
    // It is a `Config` field rather than a constant, so a board that wants a
    // longer press does not need this driver edited.
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();
    chip->regs[kRegKeyLevelCtrl] = 0;
    chip->regs[kRegCommonConfig] = 0b0000'0100;

    pmic::Config config;
    config.press_on = pmic::PressOnTime::k1s;
    config.press_off = pmic::PressOffTime::k10s;
    config.long_press_shutdown = false;

    pmic::Axp2101 axp;
    TEST_ASSERT_EQUAL_INT(ESP_OK, axp.Init(bus, config));

    TEST_ASSERT_EQUAL_HEX8(0b0000'1110, chip->regs[kRegKeyLevelCtrl]);
    TEST_ASSERT_EQUAL_HEX8(0, chip->regs[kRegCommonConfig]);
}

// --- The currents, which are settings and not measurements ---------------
//
// **The AXP2101 cannot measure a current.** Its ADC channel register (0x30) has
// five channels — battery, TS, VBUS, system, die — and none of them is an
// ammeter, which is why `XPowersLib`'s AXP2101 class has no `getBattChargeCurrent`
// where its AXP192 one does. So what the readouts can honestly show is what the
// charger is *set to*, read back off the chip rather than remembered from what
// `Init` wrote — and these tests are about that difference, because a register
// somebody else reset is exactly the case a remembered value cannot see.

void test_pmic_reads_the_charge_currents_back_off_the_chip(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();

    pmic::Axp2101 axp;
    TEST_ASSERT_EQUAL_INT(ESP_OK, axp.Init(bus));

    // What `Init` put there — the vendor's 50/500/50 mA and a 2 A input limit.
    pmic::Status status = {};
    TEST_ASSERT_EQUAL_INT(ESP_OK, axp.Read(&status));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(pmic::ChargeCurrent::k500mA),
                            status.charge_limit_code);
    TEST_ASSERT_EQUAL_UINT16(500, pmic::ChargeCurrentMa(status.charge_limit_code));
    TEST_ASSERT_EQUAL_UINT16(50, pmic::PrechargeCurrentMa(status.precharge_code));
    TEST_ASSERT_EQUAL_UINT16(50, pmic::TerminationCurrentMa(status.termination_code));
    TEST_ASSERT_EQUAL_UINT16(2000, pmic::VbusCurrentLimitMa(status.vbus_limit_code));

    // And a chip that came back from somewhere holding something else says so,
    // rather than repeating what this driver believes it wrote once.
    chip->regs[0x62] = 0xE0 | static_cast<uint8_t>(pmic::ChargeCurrent::k100mA);
    chip->regs[0x16] = static_cast<uint8_t>(pmic::VbusCurrentLimit::k500mA);
    TEST_ASSERT_EQUAL_INT(ESP_OK, axp.Read(&status));
    TEST_ASSERT_EQUAL_UINT16(100, pmic::ChargeCurrentMa(status.charge_limit_code));
    TEST_ASSERT_EQUAL_UINT16(500, pmic::VbusCurrentLimitMa(status.vbus_limit_code));
}

void test_pmic_the_current_fields_are_the_ones_they_share_a_register_with(void) {
    // Each of the four lives in a field of a register that holds something else,
    // so a decode that forgot its mask would read a neighbour's bits. The high
    // bits are set here for exactly that reason.
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();

    pmic::Axp2101 axp;
    TEST_ASSERT_EQUAL_INT(ESP_OK, axp.Init(bus));

    chip->regs[0x61] = 0xFC | static_cast<uint8_t>(pmic::PrechargeCurrent::k75mA);
    chip->regs[0x62] = 0xE0 | static_cast<uint8_t>(pmic::ChargeCurrent::k1000mA);
    chip->regs[0x63] = 0xF0 | static_cast<uint8_t>(pmic::TerminationCurrent::k100mA);
    chip->regs[0x16] = 0xF8 | static_cast<uint8_t>(pmic::VbusCurrentLimit::k1500mA);

    pmic::Status status = {};
    TEST_ASSERT_EQUAL_INT(ESP_OK, axp.Read(&status));
    TEST_ASSERT_EQUAL_UINT16(75, pmic::PrechargeCurrentMa(status.precharge_code));
    TEST_ASSERT_EQUAL_UINT16(1000, pmic::ChargeCurrentMa(status.charge_limit_code));
    TEST_ASSERT_EQUAL_UINT16(100, pmic::TerminationCurrentMa(status.termination_code));
    TEST_ASSERT_EQUAL_UINT16(1500, pmic::VbusCurrentLimitMa(status.vbus_limit_code));
}

void test_pmic_a_charge_current_code_nobody_documented_is_not_guessed(void) {
    // The charge-current field is not a dense enum: it goes 0 mA at 0 and then
    // jumps to 100 mA at 4, and `XPowersLib` lists nothing for 1, 2 and 3. A
    // decoder that filled the gap with 25/50/75 mA would be inventing a
    // datasheet — so it answers 0, and the raw code travels next to it so a
    // readout can say `code 2` instead of a number that is not true.
    TEST_ASSERT_EQUAL_UINT16(0, pmic::ChargeCurrentMa(0));
    TEST_ASSERT_EQUAL_UINT16(0, pmic::ChargeCurrentMa(1));
    TEST_ASSERT_EQUAL_UINT16(0, pmic::ChargeCurrentMa(3));
    TEST_ASSERT_EQUAL_UINT16(100, pmic::ChargeCurrentMa(4));
    TEST_ASSERT_EQUAL_UINT16(0, pmic::ChargeCurrentMa(17));
    TEST_ASSERT_EQUAL_UINT16(0, pmic::ChargeCurrentMa(0xFF));

    // The other three are dense, and every step of each is pinned: these are
    // the numbers that make a `mA` on the glass mean anything.
    TEST_ASSERT_EQUAL_UINT16(125, pmic::ChargeCurrentMa(5));
    TEST_ASSERT_EQUAL_UINT16(200, pmic::ChargeCurrentMa(8));
    TEST_ASSERT_EQUAL_UINT16(300, pmic::ChargeCurrentMa(9));
    TEST_ASSERT_EQUAL_UINT16(1000, pmic::ChargeCurrentMa(16));

    TEST_ASSERT_EQUAL_UINT16(0, pmic::PrechargeCurrentMa(0));
    TEST_ASSERT_EQUAL_UINT16(25, pmic::PrechargeCurrentMa(1));
    TEST_ASSERT_EQUAL_UINT16(75, pmic::PrechargeCurrentMa(3));

    TEST_ASSERT_EQUAL_UINT16(0, pmic::TerminationCurrentMa(0));
    TEST_ASSERT_EQUAL_UINT16(50, pmic::TerminationCurrentMa(2));
    TEST_ASSERT_EQUAL_UINT16(100, pmic::TerminationCurrentMa(4));
    TEST_ASSERT_EQUAL_UINT16(0, pmic::TerminationCurrentMa(7));

    TEST_ASSERT_EQUAL_UINT16(100, pmic::VbusCurrentLimitMa(0));
    TEST_ASSERT_EQUAL_UINT16(900, pmic::VbusCurrentLimitMa(2));
    TEST_ASSERT_EQUAL_UINT16(2000, pmic::VbusCurrentLimitMa(5));
    TEST_ASSERT_EQUAL_UINT16(0, pmic::VbusCurrentLimitMa(6));
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

    RUN_TEST(test_pmic_reads_the_charge_currents_back_off_the_chip);
    RUN_TEST(test_pmic_the_current_fields_are_the_ones_they_share_a_register_with);
    RUN_TEST(test_pmic_a_charge_current_code_nobody_documented_is_not_guessed);
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

    RUN_TEST(test_pmic_init_writes_the_power_key_when_the_chip_holds_something_else);
    RUN_TEST(test_pmic_init_leaves_a_power_key_that_is_already_right_alone);
    RUN_TEST(test_pmic_init_keeps_the_bits_of_0x27_it_does_not_own);
    RUN_TEST(test_pmic_init_turns_the_long_press_shutdown_on);
    RUN_TEST(test_pmic_init_never_writes_the_soft_power_off_bit);
    RUN_TEST(test_pmic_init_clears_a_soft_power_off_bit_it_finds_set);
    RUN_TEST(test_pmic_the_power_key_can_be_configured_the_other_way);
}
