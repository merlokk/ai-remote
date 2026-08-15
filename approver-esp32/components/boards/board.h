#pragma once

// The pin map of the one board this firmware runs on (CLAUDE.md §10.1).
//
// **Source: `docs/ESP32-C6-Touch-AMOLED-2.16-details-inter.jpg`** — Waveshare's
// own pinout sheet for this product, kept in the repository next to the
// datasheets so the numbers below can be checked against what they were copied
// from. §10.1's rule is that a pin number invented from memory costs a bricked
// evening; this file is the one place pins are written down, and it is the only
// place that may cite that image.
//
// What the sheet does **not** give, and so is not here: the panel's TE line,
// backlight control, the PMIC's interrupt, the RTC's interrupt. When one of
// those is needed, it comes from the schematic or a vendor demo — with the
// source named, the same way this file names its own.
//
// I²C addresses are deliberately absent. They belong to each chip's driver
// (§10.14.2), verified against the datasheet in `docs/`; this file is wires.

#include "axp2101.h"
#include "buttons.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "i2c_bus.h"
#include "pcf85063.h"
#include "qmi8658.h"
#include "speaker.h"

namespace board {

inline constexpr const char *kName = "Waveshare ESP32-C6-Touch-AMOLED-2.16";

// The panel: CO5300, 480x480, driven over QSPI (§10.1 — no PSRAM, so LVGL gets
// partial buffers and this geometry is what sizes them).
inline constexpr int kScreenWidth = 480;
inline constexpr int kScreenHeight = 480;

// --- Buttons -------------------------------------------------------------
// `KEY` is the free one (§10.1), and §10.15 gives it its job: held at boot, it
// restores the config. `BOOT` and `PWR` belong to the bootloader and the PMIC.
//
// **`PWR` is wired to the AXP2101's PWRON pin** (pressed = 0), and the chip
// acts on it with no help from this firmware: a short press powers the board
// on, a long press powers it off. The pin is here so the firmware can *see*
// the button; it is not how the board is switched. §10.1 has the timings this
// board is actually configured with.
namespace button {
inline constexpr gpio_num_t kBoot = GPIO_NUM_9;
inline constexpr gpio_num_t kKey = GPIO_NUM_10;
inline constexpr gpio_num_t kPwr = GPIO_NUM_18;

// The index each button has in `board::Buttons()`. The driver is a table of
// pins that knows no names (§10.14.2); this is where the table gets its meaning,
// and it is the only place that may assume the order.
enum Index : size_t {
    kBootIndex = 0,
    kKeyIndex = 1,
    kPwrIndex = 2,
    kCount = 3,
};

// `BOOT` and `KEY` short their pin to ground: idle high, pressed 0. **`PWR`
// is inverted at this pin** — GPIO18 rests at 0 (driven, not floating: it
// stays 0 with the internal pull-up enabled) and goes high while the button is
// held, the opposite of the AXP2101's own PWRON pin. Measured on this board,
// not read off a sheet; `board.cpp` carries the note and the polarity.
//
// Reading any of them costs nothing and changes nothing — in particular,
// watching `PWR` is not how the board is switched (§10.1).
}  // namespace button

// --- The I²C bus ---------------------------------------------------------
// One bus, five chips: CST9220 touch, QMI8658 IMU, PCF85063 RTC, AXP2101 PMIC,
// ES8311/ES7210 codecs. This is the bus §10.14.3 leases rather than shares.
namespace i2c {
inline constexpr gpio_num_t kScl = GPIO_NUM_7;
inline constexpr gpio_num_t kSda = GPIO_NUM_8;
}  // namespace i2c

// --- CST9220 touch (I²C + two lines of its own) --------------------------
namespace touch {
inline constexpr gpio_num_t kReset = GPIO_NUM_11;
inline constexpr gpio_num_t kInterrupt = GPIO_NUM_5;
}  // namespace touch

// --- QMI8658 IMU ---------------------------------------------------------
// §10.13: the IMU has no job. The lines are recorded because they are wired,
// which is not a reason to use them — no gesture ever approves anything.
namespace imu {
inline constexpr gpio_num_t kInterrupt1 = GPIO_NUM_16;
inline constexpr gpio_num_t kInterrupt2 = GPIO_NUM_17;
}  // namespace imu

// --- CO5300 AMOLED, QSPI -------------------------------------------------
namespace display {
inline constexpr gpio_num_t kChipSelect = GPIO_NUM_15;
inline constexpr gpio_num_t kSclk = GPIO_NUM_0;
inline constexpr gpio_num_t kData0 = GPIO_NUM_1;
inline constexpr gpio_num_t kData1 = GPIO_NUM_2;
inline constexpr gpio_num_t kData2 = GPIO_NUM_3;
inline constexpr gpio_num_t kData3 = GPIO_NUM_4;

// **The panel's reset is not a GPIO.** It hangs off the PMIC's ALDO3 rail, so
// resetting the display means an I²C transaction to the AXP2101 first. That is
// an ordering constraint on boot, not a detail: the I²C bus and the PMIC driver
// have to be up before the display can be brought up at all.
inline constexpr bool kResetIsOnPmicAldo3 = true;
}  // namespace display

// --- ES8311 codec + ES7210 echo-cancellation ADC, I²S --------------------
// §10.13: of the audio hardware, only one chirp on a new request has a job.
namespace audio {
inline constexpr gpio_num_t kMclk = GPIO_NUM_19;
inline constexpr gpio_num_t kSclk = GPIO_NUM_20;
inline constexpr gpio_num_t kAsdout = GPIO_NUM_21;
inline constexpr gpio_num_t kLrck = GPIO_NUM_22;
inline constexpr gpio_num_t kDsdin = GPIO_NUM_23;

// Like the panel's reset: the amplifier's enable is the PMIC's ALDO2 rail, not
// a pin. No sound before the PMIC is talking.
inline constexpr bool kAmplifierIsOnPmicAldo2 = true;
}  // namespace audio

// --- TF slot -------------------------------------------------------------
// §10.13 gives it no job, and the wiring is the reason to keep it that way:
// SCK/MOSI/MISO are the panel's QSPI lines, with only a separate chip select.
// Using the card means sharing the bus the display is being flushed over.
namespace sdcard {
inline constexpr gpio_num_t kSck = GPIO_NUM_0;
inline constexpr gpio_num_t kMosi = GPIO_NUM_1;
inline constexpr gpio_num_t kMiso = GPIO_NUM_2;
inline constexpr gpio_num_t kChipSelect = GPIO_NUM_6;
}  // namespace sdcard

// --- What the map has to hold, checked by the compiler -------------------
static_assert(i2c::kScl != i2c::kSda, "the one I2C bus needs two wires");
static_assert(touch::kReset != touch::kInterrupt, "touch lines collide");
static_assert(imu::kInterrupt1 != imu::kInterrupt2, "IMU lines collide");
static_assert(display::kChipSelect != sdcard::kChipSelect,
              "the shared QSPI bus is only safe while the chip selects differ");

// And the overlap that is real, asserted so that correcting it "back" fails the
// build rather than quietly giving the TF slot its own wires that do not exist.
static_assert(sdcard::kSck == display::kSclk && sdcard::kMosi == display::kData0 &&
                  sdcard::kMiso == display::kData1,
              "the TF slot shares the panel's QSPI wires — see the note above");

// Names and numbers for one log line at boot. Cheap, and the fastest way to
// find out that a board revision moved something.
void LogPinout();

// --- The board as an object ----------------------------------------------
// Two levels, the shape §10.14.4 borrows from the house firmware: one class
// owns the bus, this layer owns the devices on it and whether they answered.
// Everything here is a static object with a trivial constructor and an
// explicit Init, in an order written down rather than left to the linker
// (§10.14.1).

// Brings up the I²C bus and the chips on it. Not fatal if a chip is missing —
// it is reported, and `Pmic().Present()` says so afterwards.
esp_err_t Init();

i2cbus::Bus &I2c();
pmic::Axp2101 &Pmic();
rtc::Pcf85063 &Clock();
buttons::Buttons &Buttons();

// §10.13 gives the IMU no job, and this accessor does not give it one: the
// console reads it, nothing else may.
//
// The `::` is not decoration: `board::imu` above is this file's own namespace
// of interrupt pins, and inside `namespace board` it hides the driver's `imu`
// completely. Leaving it off is a compile error today and would be a subtler
// one if the two ever held names in common.
::imu::Qmi8658 &Imu();

// The IMU's two interrupt lines, **observed and never acted on** (§10.13).
// `Init` configures them as plain inputs with a pull-down, so a line the chip
// is not driving reads a steady low instead of noise; nothing subscribes to
// them and no ISR is installed. They are here because the only way to answer
// "does INT2 actually pulse" is to look, and the datasheet's own revision
// history is ambiguous about whether these pins are enabled at all.
bool ImuInterrupt1();
bool ImuInterrupt2();

// The codec and the speaker in front of it. §10.13 gives this hardware one job
// — a short chirp on a new request (§10.8.1) — and `Init` brings it up far
// enough to do it: the amplifier's rail (ALDO2, §10.1) on, the codec
// configured and **muted**, the I²S channel running.
//
// The `::` is the same shadowing guard `Imu()` needs: `board::audio` is this
// file's namespace of I²S pins, and it hides the driver's `audio` inside
// `namespace board`.
::audio::Es8311 &Codec();
::audio::Speaker &Sound();

}  // namespace board
