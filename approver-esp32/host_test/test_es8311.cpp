// The ES8311 codec (CLAUDE.md §10.8.1, §10.13), against the fake wire.
//
// §10.13 gives this chip one job — a short chirp on a new request — and §10.15
// makes its volume the first setting that round-trips through `config.json`.
// So the interesting surface is small and entirely arithmetic-and-bits: the
// volume mapping (where 0 has to mean silence rather than the quietest step),
// the mute the driver leaves the codec in at boot, and the sample rates it
// refuses rather than approximates.

#include "es8311.h"
#include "fake_platform.h"
#include "i2c_bus.h"
#include "unity.h"

namespace {

constexpr uint8_t kAddr = 0x18;
constexpr uint8_t kAddrCeHigh = 0x19;

constexpr uint8_t kRegDacMute31 = 0x31;
constexpr uint8_t kRegDacVolume32 = 0x32;
constexpr uint8_t kRegChipId1 = 0xFD;
constexpr uint8_t kRegChipId2 = 0xFE;

constexpr uint8_t kDacMuteBits = 0x60;

fake::Device *PutOnWire(uint8_t address = kAddr) {
    fake::Device *chip = fake::AddDevice(address);
    chip->regs[kRegChipId1] = 0x83;
    chip->regs[kRegChipId2] = 0x11;
    return chip;
}

void BringUp(i2cbus::Bus &bus) {
    TEST_ASSERT_EQUAL_INT(ESP_OK, bus.Init(GPIO_NUM_7, GPIO_NUM_8));
}

}  // namespace

// --- Identity ------------------------------------------------------------

void test_codec_comes_up_at_its_address(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    PutOnWire();

    audio::Es8311 codec;
    TEST_ASSERT_EQUAL_INT(ESP_OK, codec.Init(bus, 16000));
    TEST_ASSERT_TRUE(codec.Present());
    TEST_ASSERT_EQUAL_HEX8(kAddr, codec.Address());
    TEST_ASSERT_EQUAL_UINT32(16000, codec.SampleRate());
}

void test_codec_is_also_found_with_ce_high(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    PutOnWire(kAddrCeHigh);

    audio::Es8311 codec;
    TEST_ASSERT_EQUAL_INT(ESP_OK, codec.Init(bus, 16000));
    TEST_ASSERT_EQUAL_HEX8(kAddrCeHigh, codec.Address());
}

void test_codec_absent_is_reported(void) {
    i2cbus::Bus bus;
    BringUp(bus);

    audio::Es8311 codec;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, codec.Init(bus, 16000));
    TEST_ASSERT_FALSE(codec.Present());
}

void test_codec_refuses_a_chip_with_the_wrong_id(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();
    chip->regs[kRegChipId2] = 0x12;

    audio::Es8311 codec;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, codec.Init(bus, 16000));
    TEST_ASSERT_FALSE(codec.Present());
}

void test_codec_that_never_identified_refuses_to_be_set(void) {
    // **`address_` is 0 until `Init` finds the chip**, so a setter that only
    // checked for a bus would send `SetVolume` to I²C address 0x00 — the
    // general-call address — and open a device slot in the bus's fixed table
    // for a chip that is not there. Every other driver on this board gates on
    // `present_`; this one did not, and nothing said so.
    i2cbus::Bus bus;
    BringUp(bus);  // nothing on the wire

    audio::Es8311 codec;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, codec.Init(bus, 16000));

    // Both addresses `Init` tried are open — a device handle is configuration
    // rather than a transaction, and the fake models the real driver in
    // opening one for an address nothing answers at. What must not happen is a
    // *third*, for address 0x00.
    const size_t before = fake::P().transfer_count;
    const size_t handles = fake::P().open_handles;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, codec.SetVolume(50));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, codec.Mute(false));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, codec.SetSampleRate(48000));
    TEST_ASSERT_EQUAL_UINT(before, fake::P().transfer_count);
    TEST_ASSERT_EQUAL_UINT(handles, fake::P().open_handles);

    // And a codec that was never handed a bus at all.
    audio::Es8311 fresh;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, fresh.SetVolume(50));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, fresh.Mute(true));
}

void test_codec_rate_support_is_askable_without_the_chip(void) {
    // `Speaker::Reconfigure` has to know before it stops the I²S channel, not
    // after — the header says why, and the speaker suite has the failure this
    // prevents. Static, so it is answerable with no bus and no codec.
    TEST_ASSERT_TRUE(audio::Es8311::RateSupported(8000));
    TEST_ASSERT_TRUE(audio::Es8311::RateSupported(16000));
    TEST_ASSERT_TRUE(audio::Es8311::RateSupported(44100));
    TEST_ASSERT_TRUE(audio::Es8311::RateSupported(48000));
    TEST_ASSERT_FALSE(audio::Es8311::RateSupported(22050));
    TEST_ASSERT_FALSE(audio::Es8311::RateSupported(0));
    TEST_ASSERT_EQUAL_UINT(0, fake::P().transfer_count);
}

// --- Boot state ----------------------------------------------------------

void test_codec_comes_up_muted(void) {
    // **The amplifier rail is up whenever the board is** (§10.1: it is the
    // PMIC's ALDO2, left on so a chirp does not cost a settling delay and a
    // pop), so an unmuted idle codec is audible hiss on a desk object.
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();

    audio::Es8311 codec;
    TEST_ASSERT_EQUAL_INT(ESP_OK, codec.Init(bus, 16000));

    TEST_ASSERT_TRUE(codec.Muted());
    TEST_ASSERT_EQUAL_HEX8(kDacMuteBits, chip->regs[kRegDacMute31] & kDacMuteBits);
}

void test_codec_init_does_not_take_a_lease_per_register(void) {
    // **This test found a real one, and now guards the fix.** `WriteRegister`
    // used to acquire the bus per call, so a two-dozen-register init was two
    // dozen separate leases — the per-call locking §10.14.3 argues against and
    // quotes the house firmware for. The helpers take a `Lease &` now.
    //
    // The assertion is a small bound rather than exactly one, because there is
    // a legitimate reason to let go partway: see the test below.
    i2cbus::Bus bus;
    BringUp(bus);
    PutOnWire();

    const size_t before = fake::P().transfer_count;
    audio::Es8311 codec;
    TEST_ASSERT_EQUAL_INT(ESP_OK, codec.Init(bus, 16000));

    TEST_ASSERT_TRUE(fake::P().transfer_count - before > 20);
    TEST_ASSERT_EQUAL_UINT(2, fake::CountLeasesSince(before));
}

void test_codec_never_sleeps_holding_the_bus(void) {
    // The other half of §10.14.3, and the reason the init is two leases rather
    // than one: the reset dance needs 20 ms for the chip to come back, and a
    // nap with the wire held is a dropped touch read and a skipped clock tick
    // for nothing. **Invisible on hardware** — it still works — which is what
    // makes it worth a test at all.
    i2cbus::Bus bus;
    BringUp(bus);
    PutOnWire();

    audio::Es8311 codec;
    TEST_ASSERT_EQUAL_INT(ESP_OK, codec.Init(bus, 16000));

    TEST_ASSERT_TRUE(fake::P().delay_ms_total >= 20);   // it really did wait
    TEST_ASSERT_EQUAL_UINT32(0, fake::P().delay_ms_while_held);
}

void test_codec_read_modify_write_is_one_lease(void) {
    // The half that matters, and it holds: `Mute` reads 0x31, changes two bits
    // and writes it back without letting go — a split there would lose whatever
    // another task had set in the same register.
    i2cbus::Bus bus;
    BringUp(bus);
    PutOnWire();

    audio::Es8311 codec;
    codec.Init(bus, 16000);

    const size_t before = fake::P().transfer_count;
    TEST_ASSERT_EQUAL_INT(ESP_OK, codec.Mute(false));

    TEST_ASSERT_EQUAL_UINT(2, fake::P().transfer_count - before);
    TEST_ASSERT_TRUE(fake::OneLeaseSince(before));
}

// --- Volume (§10.15's round trip) ----------------------------------------

void test_codec_volume_zero_is_silence_not_the_quietest_step(void) {
    // The reference driver's mapping is `reg = volume * 256 / 100 - 1`, which
    // at 0 would underflow to 0xFF — full scale. Zero has to mean what it says.
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();

    audio::Es8311 codec;
    codec.Init(bus, 16000);

    TEST_ASSERT_EQUAL_INT(ESP_OK, codec.SetVolume(0));
    TEST_ASSERT_EQUAL_HEX8(0x00, chip->regs[kRegDacVolume32]);
    TEST_ASSERT_EQUAL_UINT8(0, codec.Volume());
}

void test_codec_volume_maps_the_way_the_reference_driver_does(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();

    audio::Es8311 codec;
    codec.Init(bus, 16000);

    TEST_ASSERT_EQUAL_INT(ESP_OK, codec.SetVolume(100));
    TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(0xFF), chip->regs[kRegDacVolume32]);

    TEST_ASSERT_EQUAL_INT(ESP_OK, codec.SetVolume(50));
    TEST_ASSERT_EQUAL_HEX8(127, chip->regs[kRegDacVolume32]);

    // The number §10.15 uses as its worked example of a setting that survives
    // a reboot.
    TEST_ASSERT_EQUAL_INT(ESP_OK, codec.SetVolume(45));
    TEST_ASSERT_EQUAL_HEX8(114, chip->regs[kRegDacVolume32]);
    TEST_ASSERT_EQUAL_UINT8(45, codec.Volume());
}

void test_codec_volume_is_clamped_rather_than_wrapped(void) {
    // A `config.json` written by hand, or by a newer firmware, can carry
    // anything. 200 must not become a quiet codec through an overflow.
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();

    audio::Es8311 codec;
    codec.Init(bus, 16000);

    TEST_ASSERT_EQUAL_INT(ESP_OK, codec.SetVolume(200));
    TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(0xFF), chip->regs[kRegDacVolume32]);
    TEST_ASSERT_EQUAL_UINT8(100, codec.Volume());
}

// --- Mute ----------------------------------------------------------------

void test_codec_mute_touches_only_its_own_bits(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    fake::Device *chip = PutOnWire();

    audio::Es8311 codec;
    codec.Init(bus, 16000);

    // Something else in the same register that must survive both directions.
    chip->regs[kRegDacMute31] = static_cast<uint8_t>(kDacMuteBits | 0x01);

    TEST_ASSERT_EQUAL_INT(ESP_OK, codec.Mute(false));
    TEST_ASSERT_FALSE(codec.Muted());
    TEST_ASSERT_EQUAL_HEX8(0x00, chip->regs[kRegDacMute31] & kDacMuteBits);
    TEST_ASSERT_EQUAL_HEX8(0x01, chip->regs[kRegDacMute31] & 0x01);

    TEST_ASSERT_EQUAL_INT(ESP_OK, codec.Mute(true));
    TEST_ASSERT_TRUE(codec.Muted());
    TEST_ASSERT_EQUAL_HEX8(kDacMuteBits, chip->regs[kRegDacMute31] & kDacMuteBits);
    TEST_ASSERT_EQUAL_HEX8(0x01, chip->regs[kRegDacMute31] & 0x01);
}

// --- Sample rate ---------------------------------------------------------

void test_codec_accepts_the_rates_whose_dividers_it_has(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    PutOnWire();

    audio::Es8311 codec;
    codec.Init(bus, 16000);

    const uint32_t rates[] = {8000, 16000, 32000, 44100, 48000};
    for (uint32_t rate : rates) {
        TEST_ASSERT_EQUAL_INT(ESP_OK, codec.SetSampleRate(rate));
        TEST_ASSERT_EQUAL_UINT32(rate, codec.SampleRate());
    }
}

void test_codec_refuses_a_rate_it_cannot_clock(void) {
    // **Refused rather than approximated.** A file played at the wrong rate is
    // a chirp that sounds broken and a cause that takes an hour to find — the
    // same call `speaker.h` makes about container formats.
    i2cbus::Bus bus;
    BringUp(bus);
    PutOnWire();

    audio::Es8311 codec;
    codec.Init(bus, 16000);

    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, codec.SetSampleRate(22050));
    // And the rate it *was* running at is unchanged, so a refused call leaves
    // a working codec rather than a half-configured one.
    TEST_ASSERT_EQUAL_UINT32(16000, codec.SampleRate());
}

void test_codec_init_refuses_an_impossible_rate_up_front(void) {
    i2cbus::Bus bus;
    BringUp(bus);
    PutOnWire();

    audio::Es8311 codec;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, codec.Init(bus, 12345));
}

void RegisterEs8311Tests(void) {
    RUN_TEST(test_codec_comes_up_at_its_address);
    RUN_TEST(test_codec_is_also_found_with_ce_high);
    RUN_TEST(test_codec_absent_is_reported);
    RUN_TEST(test_codec_refuses_a_chip_with_the_wrong_id);
    RUN_TEST(test_codec_that_never_identified_refuses_to_be_set);
    RUN_TEST(test_codec_rate_support_is_askable_without_the_chip);

    RUN_TEST(test_codec_comes_up_muted);
    RUN_TEST(test_codec_init_does_not_take_a_lease_per_register);
    RUN_TEST(test_codec_never_sleeps_holding_the_bus);
    RUN_TEST(test_codec_read_modify_write_is_one_lease);

    RUN_TEST(test_codec_volume_zero_is_silence_not_the_quietest_step);
    RUN_TEST(test_codec_volume_maps_the_way_the_reference_driver_does);
    RUN_TEST(test_codec_volume_is_clamped_rather_than_wrapped);

    RUN_TEST(test_codec_mute_touches_only_its_own_bits);

    RUN_TEST(test_codec_accepts_the_rates_whose_dividers_it_has);
    RUN_TEST(test_codec_refuses_a_rate_it_cannot_clock);
    RUN_TEST(test_codec_init_refuses_an_impossible_rate_up_front);
}
