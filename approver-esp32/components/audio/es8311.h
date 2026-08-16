#pragma once

// ES8311 — the board's audio codec (CLAUDE.md §10.1), on the same I²C bus as
// everything else, with its I²S side driven by `audio::Speaker`.
//
// §10.13 gives the audio hardware exactly one job: **one short chirp on a new
// request** (§10.8.1). This driver is the playback half of that and nothing
// more — no microphone, no ES7210, no recording. The chip can do all three; the
// firmware does not.
//
// **Where the numbers come from**, and the split matters because the two
// sources answer different questions:
//
//   * `docs/ES8311.DS.pdf` — Everest's own datasheet: the I²C address
//     (`0011 00 CE`, so 0x18 with CE tied low), the register map, and the chip
//     identity in 0xFD/0xFE (0x83, 0x11).
//   * **Espressif's `es8311` component in `esp-bsp`** — the register *sequence*:
//     which registers to write, in what order, with which values, to get from
//     reset to sound. That is engineering knowledge the datasheet does not
//     spell out, and it is read there the way §10.1 reads XPowersLib for the
//     PMIC: **a source, not a dependency.** Nothing links against it, and it is
//     not in `idf_component.yml` — which is also what keeps root §1's approved
//     list unchanged.
//
// **The clock table collapses to one row, and that is a real simplification.**
// The reference driver carries ~95 coefficient rows for every MCLK/rate pair.
// But every row where MCLK is exactly 256×fs — 8 k, 16 k, 32 k, 44.1 k, 48 k —
// holds *identical* dividers. ESP-IDF's I²S drives MCLK at 256×fs by default,
// so this driver fixes that ratio and writes one set of dividers for any of
// those rates, instead of carrying a table it would use one line of.
//
// Library layer (§10.14.2): it drives a chip, and knows nothing about approvals.

#include <cstdint>

#include "esp_err.h"
#include "i2c_bus.h"

namespace audio {

// 7-bit. The datasheet gives the address as `0011 00 CE`; this board ties CE
// low, and Init checks the chip identity rather than trusting that.
inline constexpr uint8_t kCodecAddress = 0x18;
inline constexpr uint8_t kCodecAddressCeHigh = 0x19;

// The ratio this driver requires between MCLK and the sample rate. It is what
// makes the coefficient table collapse to one row, and it is ESP-IDF's own I²S
// default, so the two agree without anything being configured to match.
inline constexpr uint32_t kMclkMultiple = 256;

class Es8311 {
   public:
    Es8311() = default;
    Es8311(const Es8311 &) = delete;
    Es8311 &operator=(const Es8311 &) = delete;

    // Trivial constructor, separate Init (§10.14.1). Leaves the codec **muted**
    // — the amplifier rail is up whenever the board is (§10.1: it is the PMIC's
    // ALDO2), so an unmuted idle codec is audible hiss on a desk object.
    esp_err_t Init(i2cbus::Bus &bus, uint32_t sample_rate);
    bool Present() const { return present_; }
    uint8_t Address() const { return address_; }

    // Whether this driver can clock a rate at all, **askable without touching
    // the chip**. `Speaker::Reconfigure` needs exactly that: it stops the I²S
    // channel before it can retune it, so finding out from the codec's error
    // return would mean finding out with the channel already stopped — which
    // is how one unplayable file used to leave the speaker silent until the
    // next reboot.
    static bool RateSupported(uint32_t rate);

    // One of the rates whose dividers are the 256×fs row: 8000, 16000, 32000,
    // 44100, 48000. Anything else is refused rather than approximated —
    // playing a file at the wrong rate is a chirp that sounds broken and a
    // cause that takes an hour to find.
    esp_err_t SetSampleRate(uint32_t rate);
    uint32_t SampleRate() const { return sample_rate_; }

    // 0..100, mapped the way the reference driver maps it: `reg32 =
    // volume * 256 / 100 - 1`, and 0 is silence rather than the quietest step.
    esp_err_t SetVolume(uint8_t percent);
    uint8_t Volume() const { return volume_; }

    esp_err_t Mute(bool muted);
    bool Muted() const { return muted_; }

   private:
    // A codec that identified itself on a bus that is up. **The public setters
    // gate on this rather than on the bus alone**: after a failed `Init`,
    // `address_` is still 0, so a `SetVolume` that only checked for a bus
    // would go out to I²C address 0x00 and open a device slot for a chip that
    // does not exist. Every other driver on this board checks `present_`; this
    // one did not, and nothing said so.
    bool Ready() const { return present_ && bus_ != nullptr; }

    // **Every one of these takes the lease rather than acquiring it**, and that
    // is the whole shape of this class (§10.14.3). The public methods above own
    // one `Acquire()` each and hand it down; a helper that took the bus itself
    // would make a twenty-register configuration sequence twenty separate
    // leases, which is the per-call locking §10.14.3 argues against and which
    // this driver did until the host tests noticed.
    esp_err_t Identify(i2cbus::Lease &lease, uint8_t address);
    esp_err_t WriteRegister(i2cbus::Lease &lease, uint8_t reg, uint8_t value);
    esp_err_t ReadRegister(i2cbus::Lease &lease, uint8_t reg, uint8_t *value);
    esp_err_t UpdateRegister(i2cbus::Lease &lease, uint8_t reg, uint8_t keep_mask,
                             uint8_t set_bits);

    // The register work behind the public setters, so `Init` can do all of it
    // under the lease it already holds instead of re-entering through them.
    // **Re-entering would deadlock, not just churn**: the bus mutex is a plain
    // FreeRTOS mutex, not a recursive one.
    esp_err_t ApplySampleRate(i2cbus::Lease &lease, uint32_t rate);
    esp_err_t ApplyVolume(i2cbus::Lease &lease, uint8_t percent);
    esp_err_t ApplyMute(i2cbus::Lease &lease, bool muted);

    i2cbus::Bus *bus_ = nullptr;
    uint8_t address_ = 0;
    bool present_ = false;
    bool muted_ = true;
    uint8_t volume_ = 0;
    uint32_t sample_rate_ = 0;
};

}  // namespace audio
