#pragma once

// The pin map of the one board this firmware runs on (CLAUDE.md §10.1).
//
// **Sources: the vendor's schematic and pinout, kept in `docs/` next to this
// firmware** — `YD-ESP32-S3-SCH-V1.4.pdf`, `pinout.avif`, `board config.avif`,
// with the two web sources they came from in `docs/links.txt` — plus what the
// chip itself reported over the programming port. §10.1's rule is that a pin
// number invented from memory costs a bricked evening; this file is the one
// place pins are written down, and every number in it has one of those
// provenances.
//
// Two of them are not this board's decision at all: GPIO19/20 are the S3's
// native USB, fixed in silicon, and GPIO43/44 are UART0's defaults — which the
// chip announces at boot (`cpu_start: GPIO 44 and 43 are used as console UART
// I/O pins`).
//
// **What the chip said**, read with `esptool flash-id` on the board on this desk
// rather than taken off a product page, because this board ships in several
// memory variants and guessing is what `sdkconfig.defaults` would then be wrong
// about:
//
//     ESP32-S3 (QFN56) revision v0.2
//     Wi-Fi, BT 5 (LE), dual core + LP core, 240 MHz
//     Embedded PSRAM 8 MB (AP_3v3)  -> octal, and CONFIG_SPIRAM_MODE_OCT
//     Flash 16 MB, quad, 3.3 V
//
// ## The three facts that shape everything above this file
//
//   * **there are two USB-C connectors and they are not interchangeable**
//     (§10.18.4). One is a CH343P USB-to-UART bridge — that is the programming
//     port, the console port, and the one `idf.py -p COM6 monitor` talks to. The
//     other is the S3's *native* USB on GPIO19/20, and this firmware runs it as a
//     **host** so a security key can be plugged into it. On the C6 board of the
//     sibling folder there was one connector doing both jobs, which is why that
//     board could never have done this;
//   * **the console is therefore on UART0, not on USB Serial/JTAG.** The sibling
//     board sets `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` because it has no bridge;
//     this one must not, because the native USB has another job. `sdkconfig.defaults`
//     says so at the line that does it;
//   * **there is one button and it is GPIO0**, which is also the ROM's download
//     strap. Held across a reset it means "flash me", so this firmware may only
//     ever sample it *after* boot — §10.15's restore window is built on that
//     distinction and `config.h` states it.
//
// **No I²C bus, and no chips on one.** The C6 board had five; this one has a
// serial LED, a button and a USB socket. That absence is most of why this
// firmware is a third of the size of its sibling's.

#include <cstddef>

#include "buttons.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"

namespace board {

inline constexpr const char *kName = "YD-ESP32-S3 (vcc-gnd)";

// --- The one button ------------------------------------------------------
//
// `BOOT` is the only one the firmware can read: `RST` is wired to the chip's
// enable line and is gone before any code runs.
//
// It has two jobs, and both are deliberately small (§10.10 — the button cannot
// approve anything on its own):
//
//   * a **short press is a deny** on a pending request, when
//     `config::Approval::deny_button` allows it. There is no press that means
//     allow: that needs the key (§10.18);
//   * **held five seconds after boot** it restores `config.json` (§10.15). After
//     boot, never through the reset — see the note at the top of this file.
namespace button {
inline constexpr gpio_num_t kBoot = GPIO_NUM_0;

// The index it has in `board::Buttons()`. The driver is a table of pins that
// knows no names (§10.14.2); this is where the table gets its meaning, and it is
// the only place that may assume the order.
enum Index : size_t {
    kBootIndex = 0,
    kCount = 1,
};
}  // namespace button

// --- The one output ------------------------------------------------------
//
// A WS2812 addressable RGB LED. `led.h` explains how it is driven and §10.17 is
// what each colour means.
namespace led {
inline constexpr gpio_num_t kData = GPIO_NUM_48;

// **UART1, and the choice is forced rather than preferred.** The encoding needs
// a UART (§10.17.1); UART0 is the console on the CH343P bridge and is not
// negotiable; UART2 does not exist as a separate peripheral to reach for on
// every S3 package. That leaves one.
inline constexpr uart_port_t kUart = UART_NUM_1;
}  // namespace led

// --- The console ---------------------------------------------------------
//
// The CH343P bridge. The numbers are here because they are wired and because
// somebody will eventually ask why `esp_console` works without any code naming a
// pin: it works because these are UART0's defaults on this chip and the bridge
// is soldered to them. The board's two other LEDs — TX on GPIO43 and RX on
// GPIO44 — are the bridge's activity lights and are **not** software-controlled;
// §10.13 lists them among the parts with no job.
namespace console {
inline constexpr gpio_num_t kTx = GPIO_NUM_43;
inline constexpr gpio_num_t kRx = GPIO_NUM_44;
inline constexpr uart_port_t kUart = UART_NUM_0;
}  // namespace console

// --- The native USB, running as a host (§10.18.4) ------------------------
//
// D- and D+ of the S3's own USB peripheral. **Recorded and never configured**:
// the USB Host Library takes the pins from the peripheral itself, and a GPIO
// matrix entry pointed at either of them would break enumeration. They are here
// so that the answer to "which pins are these" is in the same file as every
// other pin, and so that a future addition cannot claim one by accident — which
// is what the `static_assert` below makes into a build failure.
namespace usb {
inline constexpr gpio_num_t kDataMinus = GPIO_NUM_19;
inline constexpr gpio_num_t kDataPlus = GPIO_NUM_20;

// **VBUS is not switchable on this board.** The OTG connector's 5 V is tied to
// the board's own rail, so a key has power whenever the board does and this
// firmware cannot power-cycle one that has wedged. `fido_usb.h` says what
// follows from that; the constant exists so the assumption is written down
// somewhere a future revision would have to change.
inline constexpr bool kVbusIsSwitchable = false;
}  // namespace usb

// --- Pins that are not ours ----------------------------------------------
//
// GPIO35, 36 and 37 are the octal PSRAM's, on the R8 part this board carries.
// Listed for the assertion below, which is the whole reason to write them down:
// claiming one of them is not a compile error anywhere else, and the symptom is
// a device that boots until something touches PSRAM.
namespace reserved {
inline constexpr gpio_num_t kPsram0 = GPIO_NUM_35;
inline constexpr gpio_num_t kPsram1 = GPIO_NUM_36;
inline constexpr gpio_num_t kPsram2 = GPIO_NUM_37;
}  // namespace reserved

// --- What the map has to hold, checked by the compiler -------------------
static_assert(led::kData != button::kBoot, "the LED and the button cannot share a pin");
static_assert(led::kData != console::kTx && led::kData != console::kRx,
              "the LED would be driven by console traffic");
static_assert(led::kUart != console::kUart, "the console owns UART0 (see the note above)");
static_assert(led::kData != usb::kDataMinus && led::kData != usb::kDataPlus,
              "the LED must not sit on the native USB pins");
static_assert(led::kData != reserved::kPsram0 && led::kData != reserved::kPsram1 &&
                  led::kData != reserved::kPsram2,
              "GPIO35..37 belong to the octal PSRAM on this part");

// Names and numbers for one log line at boot. Cheap, and the fastest way to find
// out that a board revision moved something.
void LogPinout();

// --- The board as an object ----------------------------------------------
//
// One level rather than the sibling board's two, because there is no bus to own:
// this layer brings up the pieces and says whether each answered. Everything is a
// static object with a trivial constructor and an explicit Init, in an order
// written down rather than left to the linker (§10.14.1).

// **The button alone, and this exists because of one caller** (§10.15): the
// restore has to be readable before `config::Init()` parses the file. On the
// sibling board that split existed because `Init()` needed an I²C bus; here it
// exists because `Init()` starts the LED, and the LED's brightness comes out of
// the config that has not been read yet.
//
// Idempotent: `Init()` calls it too.
esp_err_t InitButtons();

// Brings up the LED. Not fatal if it fails (§10.10) — a device with no readout
// can still approve; it just cannot say what it is doing.
esp_err_t Init();

buttons::Buttons &Buttons();

// Whether `BOOT` is down right now, undebounced. For the restore window, which
// samples it directly rather than through a poll loop.
bool BootPressed();

}  // namespace board
