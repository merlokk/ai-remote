// The QMI8658C (CLAUDE.md §10.13), against the fake wire.
//
// §10.13 gives this chip no job — nothing in the approval path may read it, and
// no gesture ever approves anything. What it has is a console readout, and the
// readout's whole value is that six plausible numbers cannot be told apart from
// six correct ones by looking. So the tests are about the two traps §10.13
// records finding on the real board — the address being the inverse of the
// habit, and CTRL1's auto-increment being **off** by default, which turns a
// fourteen-byte burst into TEMP_L fourteen times — plus the scale arithmetic
// that turns raw counts into g.

#include "fake_platform.h"
#include "i2c_bus.h"
#include "qmi8658.h"
#include "unity.h"

namespace {

constexpr uint8_t kSa0Low = 0x6B;   // this board: SA0 pulled down
constexpr uint8_t kSa0High = 0x6A;  // the floating one

constexpr uint8_t kRegWhoAmI = 0x00;
constexpr uint8_t kRegRevision = 0x01;
constexpr uint8_t kRegCtrl1 = 0x02;
constexpr uint8_t kRegCtrl2 = 0x03;
constexpr uint8_t kRegCtrl3 = 0x04;
constexpr uint8_t kRegStatus0 = 0x2E;
constexpr uint8_t kRegTempL = 0x33;

constexpr uint8_t kCtrl1AutoIncrement = 1 << 6;
constexpr uint8_t kCtrl1BigEndian = 1 << 5;
constexpr uint8_t kCtrl1Int1 = 1 << 4;
constexpr uint8_t kCtrl1Int2 = 1 << 3;

fake::Device *PutOnWire(uint8_t address) {
    fake::Device *chip = fake::AddDevice(address);
    chip->regs[kRegWhoAmI] = 0x05;
    chip->regs[kRegRevision] = 0x7C;
    return chip;
}

void BringUp(i2cbus::Bus &bus) {
    TEST_ASSERT_EQUAL_INT(ESP_OK, bus.Init(GPIO_NUM_7, GPIO_NUM_8));
}

// Little-endian int16 into the fourteen-byte block from TEMP_L.
void SetWord(fake::Device *chip, uint8_t reg, int16_t value) {
    chip->regs[reg] = static_cast<uint8_t>(value & 0xFF);
    chip->regs[reg + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

// Full-scale ±8 g over a signed 16-bit count.
int16_t CountsForG(float g) { return static_cast<int16_t>(g * (32768.0f / 8.0f)); }

}  // namespace

// --- Which address, and it is the inverse of the habit --------------------

void test_imu_answers_at_0x6b_on_this_board(void) {
    // §10.13: SA0 pulled down. Worth a test of its own because 0x6A is the
    // number a reader expects, and a driver that only tried one would have
    // reported "no IMU" on a board where the chip is fine.
    i2cbus::Bus bus;
    BringUp(bus);
    PutOnWire(kSa0Low);

    imu::Qmi8658 motion;
    TEST_ASSERT_EQUAL_INT(ESP_OK, motion.Init(bus));
    TEST_ASSERT_TRUE(motion.Present());
    TEST_ASSERT_EQUAL_HEX8(kSa0Low, motion.Address());
}

void test_imu_also_finds_it_at_the_other_address(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    PutOnWire(kSa0High);

    imu::Qmi8658 motion;
    TEST_ASSERT_EQUAL_INT(ESP_OK, motion.Init(bus));
    TEST_ASSERT_EQUAL_HEX8(kSa0High, motion.Address());
}

void test_imu_absent_is_reported_and_not_read(void) {
    i2cbus::Bus bus;
    BringUp(bus);  // neither address answers

    imu::Qmi8658 motion;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, motion.Init(bus));
    TEST_ASSERT_FALSE(motion.Present());

    imu::Sample sample = {};
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, motion.Read(&sample));
}

// --- CTRL1, and the trap it holds ----------------------------------------

void test_imu_turns_on_address_auto_increment(void) {
    // **The trap §10.13 records.** Without this bit a burst read returns
    // TEMP_L fourteen times: six axes that are all the same believable number,
    // which is exactly the failure that looks like working hardware.
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire(kSa0Low);

    imu::Qmi8658 motion;
    motion.Init(bus);

    TEST_ASSERT_EQUAL_HEX8(kCtrl1AutoIncrement, chip->regs[kRegCtrl1] & kCtrl1AutoIncrement);
    // And little-endian, which is what `Combine` assumes.
    TEST_ASSERT_EQUAL_HEX8(0, chip->regs[kRegCtrl1] & kCtrl1BigEndian);
}

void test_imu_leaves_the_interrupt_pins_off_by_default(void) {
    // §10.13: nothing polls them and no ISR is installed, so toggling a pin a
    // couple of hundred times a second would be current spent on nobody.
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire(kSa0Low);

    imu::Qmi8658 motion;
    motion.Init(bus);

    TEST_ASSERT_EQUAL_HEX8(0, chip->regs[kRegCtrl1] & (kCtrl1Int1 | kCtrl1Int2));
}

void test_imu_turns_the_interrupt_pins_on_when_asked(void) {
    // The flag exists so the choice is visible rather than an omission — and
    // this is what says the flag is wired to the bits it claims.
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire(kSa0Low);

    imu::Config config;
    config.interrupt_pins = true;

    imu::Qmi8658 motion;
    motion.Init(bus, config);

    TEST_ASSERT_EQUAL_HEX8(kCtrl1Int1 | kCtrl1Int2,
                           chip->regs[kRegCtrl1] & (kCtrl1Int1 | kCtrl1Int2));
}

void test_imu_writes_the_configured_ranges(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire(kSa0Low);

    imu::Config config;
    config.accel_range = imu::AccelRange::k2g;
    config.gyro_range = imu::GyroRange::k256dps;
    config.accel_rate = imu::OutputRate::k125Hz;
    config.gyro_rate = imu::OutputRate::k125Hz;

    imu::Qmi8658 motion;
    motion.Init(bus, config);

    // Range in bits 6:4, rate in 3:0 — the layout the scale below depends on.
    TEST_ASSERT_EQUAL_HEX8(0x06, chip->regs[kRegCtrl2] & 0x0F);
    TEST_ASSERT_EQUAL_HEX8(0x00, chip->regs[kRegCtrl2] & 0x70);
    TEST_ASSERT_EQUAL_HEX8(0x06, chip->regs[kRegCtrl3] & 0x0F);
    TEST_ASSERT_EQUAL_HEX8(0x40, chip->regs[kRegCtrl3] & 0x70);
}

// --- Reading the axes ----------------------------------------------------

void test_imu_reads_fourteen_registers_in_one_burst(void) {
    // Six axes that describe one moment, which two transfers cannot promise.
    i2cbus::Bus bus;
    BringUp(bus);
    PutOnWire(kSa0Low);

    imu::Qmi8658 motion;
    motion.Init(bus);

    const size_t before = fake::P().transfer_count;
    imu::Sample sample = {};
    TEST_ASSERT_EQUAL_INT(ESP_OK, motion.Read(&sample));

    // STATUS0 then the block: two transfers, one lease.
    TEST_ASSERT_EQUAL_UINT(2, fake::P().transfer_count - before);
    TEST_ASSERT_TRUE(fake::OneLeaseSince(before));

    const fake::Transfer *block = fake::LastTransfer();
    TEST_ASSERT_EQUAL_HEX8(kRegTempL, block->write[0]);
    TEST_ASSERT_EQUAL_UINT(14, block->read_length);
}

void test_imu_scales_counts_into_g(void) {
    // At rest, flat on the desk, gravity is along +Z and the magnitude is 1 g —
    // the single line §10.7 says makes the other six mean something.
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire(kSa0Low);

    imu::Qmi8658 motion;
    motion.Init(bus);  // default range is 8 g

    SetWord(chip, kRegTempL + 2, CountsForG(0.0f));   // AX
    SetWord(chip, kRegTempL + 4, CountsForG(0.0f));   // AY
    SetWord(chip, kRegTempL + 6, CountsForG(1.0f));   // AZ

    imu::Sample sample = {};
    TEST_ASSERT_EQUAL_INT(ESP_OK, motion.Read(&sample));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, sample.accel_g[0]);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, sample.accel_g[1]);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, sample.accel_g[2]);
}

void test_imu_counts_are_signed(void) {
    // Turned over: gravity along −Z. A driver reading the block as unsigned
    // gives +7.99 g here, which is a number nobody would question on a table
    // of six.
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire(kSa0Low);

    imu::Qmi8658 motion;
    motion.Init(bus);

    SetWord(chip, kRegTempL + 6, CountsForG(-1.0f));

    imu::Sample sample = {};
    motion.Read(&sample);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -1.0f, sample.accel_g[2]);
}

void test_imu_the_range_changes_the_scale(void) {
    // The same counts mean a different acceleration at a different full scale.
    // A range bit that did not take produces a steady, believable table — this
    // is what says the two are actually tied together.
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire(kSa0Low);

    imu::Config config;
    config.accel_range = imu::AccelRange::k2g;

    imu::Qmi8658 motion;
    motion.Init(bus, config);

    // Counts that are 1 g at ±8 g are 0.25 g at ±2 g.
    SetWord(chip, kRegTempL + 6, CountsForG(1.0f));

    imu::Sample sample = {};
    motion.Read(&sample);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.25f, sample.accel_g[2]);
}

void test_imu_die_temperature_is_256_lsb_per_degree(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire(kSa0Low);

    imu::Qmi8658 motion;
    motion.Init(bus);

    SetWord(chip, kRegTempL, static_cast<int16_t>(28 * 256));

    imu::Sample sample = {};
    motion.Read(&sample);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 28.0f, sample.celsius);
}

void test_imu_reports_whether_the_data_is_new(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire(kSa0Low);

    imu::Qmi8658 motion;
    motion.Init(bus);

    chip->regs[kRegStatus0] = 0x03;
    imu::Sample sample = {};
    motion.Read(&sample);
    TEST_ASSERT_TRUE(sample.accel_fresh);
    TEST_ASSERT_TRUE(sample.gyro_fresh);

    chip->regs[kRegStatus0] = 0x00;
    motion.Read(&sample);
    TEST_ASSERT_FALSE(sample.accel_fresh);
    TEST_ASSERT_FALSE(sample.gyro_fresh);
}

// --- Tilt ----------------------------------------------------------------

void test_imu_tilt_is_zero_when_the_board_is_flat(void) {
    imu::Sample flat = {};
    flat.accel_g[0] = 0.0f;
    flat.accel_g[1] = 0.0f;
    flat.accel_g[2] = 1.0f;

    float pitch = 99.0f;
    float roll = 99.0f;
    imu::Qmi8658::Tilt(flat, &pitch, &roll);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, pitch);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, roll);
}

void test_imu_tilt_on_edge_is_ninety_degrees(void) {
    // Stood on its USB connector: §10.13 read this off the board, gravity along
    // −Y. Roll is the axis that moves, and its sign is the half of this that is
    // invisible on a desk and exactly wrong once the thing is turned over.
    imu::Sample on_edge = {};
    on_edge.accel_g[0] = 0.0f;
    on_edge.accel_g[1] = -1.0f;
    on_edge.accel_g[2] = 0.0f;

    float pitch = 0.0f;
    float roll = 0.0f;
    imu::Qmi8658::Tilt(on_edge, &pitch, &roll);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, pitch);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -90.0f, roll);
}

void test_imu_never_sleeps_holding_the_bus(void) {
    // §10.14.3: a lease is held briefly and nothing sleeps under it. This
    // driver waits 15 ms for the chip to come back from its reset, and used to
    // do it with the wire held — a dropped touch read and a skipped clock tick
    // for nothing. **Invisible on hardware**, because it works either way,
    // which is exactly why it needs a test.
    i2cbus::Bus bus;
    BringUp(bus);
    PutOnWire(kSa0Low);

    imu::Qmi8658 motion;
    TEST_ASSERT_EQUAL_INT(ESP_OK, motion.Init(bus));

    TEST_ASSERT_TRUE(fake::P().delay_ms_total >= 15);  // it really did wait
    TEST_ASSERT_EQUAL_UINT32(0, fake::P().delay_ms_while_held);
}

void test_imu_configures_in_one_sequence_after_the_reset(void) {
    // The writes after the wait are one lease: CTRL7 turns the sensors on last,
    // so the chip must not begin sampling against a half-written config.
    i2cbus::Bus bus;
    BringUp(bus);
    PutOnWire(kSa0Low);

    const size_t before = fake::P().transfer_count;
    imu::Qmi8658 motion;
    TEST_ASSERT_EQUAL_INT(ESP_OK, motion.Init(bus));

    // Three, and each is a sequence that has to be uninterrupted on its own:
    // identify (WHO_AM_I then the revision), the reset write, and the six
    // configuration writes after the wait. Not one per register, and not one
    // spanning the sleep.
    TEST_ASSERT_TRUE(fake::P().transfer_count - before > 5);
    TEST_ASSERT_EQUAL_UINT(3, fake::CountLeasesSince(before));
}

void RegisterImuTests(void) {
    RUN_TEST(test_imu_answers_at_0x6b_on_this_board);
    RUN_TEST(test_imu_also_finds_it_at_the_other_address);
    RUN_TEST(test_imu_absent_is_reported_and_not_read);

    RUN_TEST(test_imu_turns_on_address_auto_increment);
    RUN_TEST(test_imu_leaves_the_interrupt_pins_off_by_default);
    RUN_TEST(test_imu_turns_the_interrupt_pins_on_when_asked);
    RUN_TEST(test_imu_writes_the_configured_ranges);
    RUN_TEST(test_imu_never_sleeps_holding_the_bus);
    RUN_TEST(test_imu_configures_in_one_sequence_after_the_reset);

    RUN_TEST(test_imu_reads_fourteen_registers_in_one_burst);
    RUN_TEST(test_imu_scales_counts_into_g);
    RUN_TEST(test_imu_counts_are_signed);
    RUN_TEST(test_imu_the_range_changes_the_scale);
    RUN_TEST(test_imu_die_temperature_is_256_lsb_per_degree);
    RUN_TEST(test_imu_reports_whether_the_data_is_new);

    RUN_TEST(test_imu_tilt_is_zero_when_the_board_is_flat);
    RUN_TEST(test_imu_tilt_on_edge_is_ninety_degrees);
}
