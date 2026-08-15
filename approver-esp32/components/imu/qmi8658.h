#pragma once

// QMI8658C — the board's 6-axis IMU (CLAUDE.md §10.1): a 3-axis accelerometer
// and a 3-axis gyroscope on the same I²C bus as everything else.
//
// **§10.13 gives this chip no job, and this driver does not change that.** It
// exists so the console can answer "which way up is it, and is the part even
// alive", which is a diagnostic — the same class of thing `power` and `date`
// are. Nothing in the approval path reads it, and the rule it must never cross
// is stated once, in §10.13: *no gesture ever approves anything*. A tilt is not
// a press.
//
// **Where the register map comes from.** `docs/QMI8658C_datasheet_rev_0.9.pdf`,
// QST's own — Table 22 (the UI register overview), Table 26 (CTRL1..CTRL9),
// Table 28 (STATUS0) and Table 29 (the output registers). Read out of the file
// in the repository, not from memory, the same rule §10.1 states for pins. Two
// things in there are the opposite of the habit and are the reason this comment
// names its tables:
//
//   * **the addresses are inverted** — SA0 pulled *down* gives 0x6B, and 0x6A
//     is the one you get with SA0 high or floating (there is a weak 200 kΩ
//     pull-up inside). So this driver probes both and says which answered;
//   * **address auto-increment is off by default** (CTRL1, bit 6). Without
//     setting it, a burst read of the six acceleration registers returns the
//     same byte six times — a plausible, wrong, perfectly stable reading.
//
// Library layer (§10.14.2): it reads a chip, and knows nothing about approvals.

#include <cstdint>

#include "esp_err.h"
#include "i2c_bus.h"

namespace imu {

// 7-bit addresses, per the datasheet's I²C section. Which one this board uses
// is not written down anywhere trustworthy, so `Init` tries both.
inline constexpr uint8_t kAddressSa0Low = 0x6B;
inline constexpr uint8_t kAddressSa0High = 0x6A;

inline constexpr uint8_t kChipId = 0x05;  // WHO_AM_I, Table 25

// CTRL2 bits 6:4 / CTRL3 bits 6:4. The values are the datasheet's encodings,
// and each name carries its range, so nothing else needs a conversion table.
enum class AccelRange : uint8_t {
    k2g = 0,
    k4g = 1,
    k8g = 2,
    k16g = 3,
};

enum class GyroRange : uint8_t {
    k16dps = 0,
    k32dps = 1,
    k64dps = 2,
    k128dps = 3,
    k256dps = 4,
    k512dps = 5,
    k1024dps = 6,
    k2048dps = 7,
};

// CTRL2 / CTRL3 bits 3:0. Only the rates this firmware would ever pick are
// named; the encoding is the datasheet's, and in 6DOF mode the real rate is the
// gyroscope-derived one in brackets (Table 26, note 13).
enum class OutputRate : uint8_t {
    k1000Hz = 0x03,  // 940 Hz in 6DOF
    k500Hz = 0x04,   // 470 Hz
    k250Hz = 0x05,   // 235 Hz
    k125Hz = 0x06,   // 117.5 Hz
};

struct Config {
    // The ranges the vendor's own QMI8658 example picks for this board
    // (`02_Example/ESP-IDF-v5.5.3/02_I2C_QMI8658`), kept because they are a
    // deliberate choice about this hardware rather than this author's taste.
    AccelRange accel_range = AccelRange::k8g;
    GyroRange gyro_range = GyroRange::k512dps;

    // The rate is **not** the vendor's 1000 Hz. Nothing here consumes a stream:
    // the console reads a sample when asked, and the most this chip could ever
    // be asked for later is "did the desk get knocked". 250 Hz is already far
    // past that, and the lower rate is current not spent on a battery device.
    OutputRate accel_rate = OutputRate::k250Hz;
    OutputRate gyro_rate = OutputRate::k250Hz;

    // CTRL5's low-pass filters, at their gentlest setting (2.66 % of ODR). A
    // readout of a thing sitting on a desk wants the noise gone.
    bool low_pass = true;

    // The two INT pins (§6.1 of the datasheet): INT1 is general purpose — a
    // ~4 ms chip-ready pulse after reset, the CTRL9 handshake, wake-on-motion —
    // and INT2 means data-ready, *pulsed* at the output rate because this
    // driver leaves `syncSmpl` clear in CTRL7.
    //
    // **Off, and that is a decision rather than an omission.** Nothing polls
    // these lines and no ISR is installed (§10.13), so enabling them would toggle
    // a pin a couple of hundred times a second for nobody. Turning it on is one
    // flag away for whoever first wants a data-ready edge instead of a poll —
    // and `imu` in the console counts the edges, which is how the enable bits
    // were found in the first place.
    bool interrupt_pins = false;
};

struct Sample {
    float accel_g[3];    // x, y, z in g
    float gyro_dps[3];   // x, y, z in degrees per second
    float celsius;       // the die, 256 LSB/°C per the datasheet's Table 12
    bool accel_fresh;    // STATUS0 aDA: new data since the last read
    bool gyro_fresh;     // STATUS0 gDA
};

class Qmi8658 {
   public:
    Qmi8658() = default;
    Qmi8658(const Qmi8658 &) = delete;
    Qmi8658 &operator=(const Qmi8658 &) = delete;

    // Trivial constructor, separate Init (§10.14.1). Probes both addresses,
    // checks WHO_AM_I, soft-resets, then writes the configuration — in that
    // order, because a reset returns every CTRL register to its default and
    // would undo the configuration if it came second.
    esp_err_t Init(i2cbus::Bus &bus, const Config &config = {});
    bool Present() const { return present_; }

    // Which of the two addresses answered, once Init has run.
    uint8_t Address() const { return address_; }
    uint8_t Revision() const { return revision_; }
    const Config &Configuration() const { return config_; }

    // Temperature, acceleration and rotation in **one** burst from 0x33, under
    // one lease (§10.14.3) — fourteen contiguous registers, so the six axes are
    // one moment rather than six.
    esp_err_t Read(Sample *out);

    // Pitch and roll in degrees, from gravity alone. Pure arithmetic on a
    // sample, so it is `static` and testable without a chip: pitch is the
    // rotation of X away from horizontal, roll that of Y. Meaningless while the
    // device is being moved — with acceleration in it, this is not gravity.
    static void Tilt(const Sample &sample, float *pitch_deg, float *roll_deg);

    // The magnitude of the acceleration vector. At rest it is 1 g, and that is
    // the cheapest check that the numbers mean anything at all.
    static float Magnitude(const Sample &sample);

   private:
    esp_err_t Identify(uint8_t address);
    esp_err_t Configure(const Config &config);

    i2cbus::Bus *bus_ = nullptr;
    uint8_t address_ = 0;
    uint8_t revision_ = 0;
    bool present_ = false;
    Config config_ = {};
    float accel_lsb_per_g_ = 1.0f;
    float gyro_lsb_per_dps_ = 1.0f;
};

}  // namespace imu
