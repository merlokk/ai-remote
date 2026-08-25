# The board (§10.1, §10.13)

## 10.1 What this runs on, and what the firmware may assume of it

**YD-ESP32-S3**, the vcc-gnd development board —
[`github.com/vcc-gnd/YD-ESP32-S3`](https://github.com/vcc-gnd/YD-ESP32-S3). An
ESP32-S3-WROOM-1 module on a breakout with two USB-C sockets, one addressable
LED, one button, and nothing else that matters.

**Every number below came from one of three places, and none of them is
memory.** §10.1's rule in the sibling folder is that a pin number invented from
memory costs a bricked evening; `components/boards/board.h` is the one place pins
are written down and it names its sources the same way.

| Source | In this folder | What it settled |
|--------|----------------|-----------------|
| The vendor's schematic and pinout | [`docs/YD-ESP32-S3-SCH-V1.4.pdf`](docs/YD-ESP32-S3-SCH-V1.4.pdf), [`docs/pinout.avif`](docs/pinout.avif), [`docs/board config.avif`](docs/board%20config.avif) | the LED on **GPIO48**, the two connectors, and **GPIO35/36/37 reserved** for the octal PSRAM |
| [`github.com/vcc-gnd/YD-ESP32-S3`](https://github.com/vcc-gnd/YD-ESP32-S3) and [mischianti's pinout](https://mischianti.org/vcc-gnd-studio-yd-esp32-s3-devkitc-1-clone-high-resolution-pinout-and-specs/) — both in [`docs/links.txt`](docs/links.txt) | the same, in words | that the LED is a **WS2812** rather than a plain one, and that the power LED is not software-controlled |
| **The chip itself**, on this desk | the two blocks below | the memory variant, and — in the boot log — the console pins |

Two of the pins are not the board's decision at all and are worth marking as
such: **GPIO19/20** are the S3's native USB and are fixed in silicon, and
**GPIO43/44** are UART0's defaults. The second of those the board announced
itself on the first boot:

```
I (874) cpu_start: GPIO 44 and 43 are used as console UART I/O pins
```

What the chip said:

```
> esptool -p COM6 flash-id
Chip type:          ESP32-S3 (QFN56) (revision v0.2)
Features:           Wi-Fi, BT 5 (LE), Dual Core + LP Core, 240MHz,
                    Embedded PSRAM 8MB (AP_3v3)
Crystal frequency:  40MHz
MAC:                7c:e8:b1:b0:95:04
Detected flash size: 16MB
Flash type set in eFuse: quad (4 data lines)
```

**8 MB of embedded PSRAM at 3.3 V is the R8 part, which means octal**, and 16 MB
of quad flash. Both are in `sdkconfig.defaults`, and both were read rather than
assumed because this board ships in several memory variants — a wrong PSRAM mode
is a device that boots until something touches the wrong end of a bus, which is
the worst kind of wrong to debug.

### The three facts that shape everything above the pin map

**1. There are two USB-C sockets and they are not interchangeable.**

| Socket | What it is | What it does here |
|--------|------------|-------------------|
| **UART** | a CH343P USB-to-UART bridge on GPIO43/44 | flashing, the log, and the §10.7 console. This is `COM6` on this machine |
| **OTG** | the S3's *native* USB peripheral, GPIO19/20 | a USB **host** for a security key (§10.18.3) |

This is the single reason this board was the right one for this design. The
sibling board has one USB-C wired to the chip's own USB, so its console *has* to
live there — and a device whose console and whose key want the same peripheral
would make you choose between talking to it and using it. Here both work at once,
and `sdkconfig.defaults` sets `CONFIG_ESP_CONSOLE_UART_DEFAULT` where that board
sets `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`.

**VBUS on the OTG socket is not switchable.** It is tied to the board's 5 V rail,
so a key has power whenever the board does. What follows is that this firmware
cannot power-cycle a key that has wedged — unplugging it is the only reset, and
`fido_usb.h` says so rather than pretending otherwise. It also means the board
has to be powered from the UART socket (or the 5 V pin) for the OTG socket to
have anything to give.

**2. There is one button, and it is also the ROM's download strap.**

`BOOT` is GPIO0. Held across a reset it puts the ROM into download mode, and no
code of ours runs at all. So this firmware may only ever sample it **after** boot
— which is why §10.15's config restore opens its window inside `app_main` rather
than through the reset the way the sibling board's does. That difference is not a
preference; a restore asked for the other way would be a restore nobody could
perform.

`RST` (labelled `EN`) is wired to the chip's enable line and is gone before any
code runs. It is not readable and never will be.

**3. There is no real-time clock, no PMIC, no IMU and no codec.**

The sibling board has five chips on an I²C bus and this one has none. Most of why
this firmware is a third of the size of that one is in that sentence. §10.13 has
what each absence costs.

### The pin map

Written down once, in `components/boards/board.h`, with `static_assert`s that
turn a collision into a build failure rather than a symptom.

| Pin | What |
|-----|------|
| GPIO0 | `BOOT` — the one button, and the download strap |
| GPIO48 | the WS2812 RGB LED (§10.17) |
| GPIO43 / GPIO44 | UART0 TX/RX, the CH343P bridge — the console |
| GPIO19 / GPIO20 | the native USB, D− and D+ — the host port (§10.18.3) |
| GPIO35, 36, 37 | **not ours** — the octal PSRAM on this part |

The three PSRAM pins are in the header for one reason: claiming one of them is
not a compile error anywhere else, and the symptom is a board that boots until
something touches PSRAM. The assertion makes it a build failure.

**GPIO19/20 are recorded and never configured.** The USB Host Library takes them
from the peripheral itself, and a GPIO matrix entry pointed at either would break
enumeration. They are in the map so the answer to "which pins are those" is in the
same file as every other pin.

## 10.13 The parts with no job

The sibling folder's §10.13 is a list of chips this firmware chooses not to use.
Here it is mostly a list of chips that **are not there**, and what their absence
costs — which is the more useful list, because an absence has no console command
to reveal it.

| Absent | What it would have done | What it costs, and what covers it |
|--------|-------------------------|-----------------------------------|
| **A real-time clock** (the sibling has a PCF85063) | keep the time across a power cut | **Nothing that matters.** §7's `ts` is *echoed from the request*, never re-derived from this device's clock (`protocol/signing.h` states it), so a device that thinks it is 1970 still produces signatures that verify. What is affected is log timestamps and `date` on the console, and both say so: `date` prints `never set - no RTC on this board, and no sync yet` until SNTP has spoken. `timesync::Init` takes no chip argument here, and its header explains the difference |
| **A PMIC** (AXP2101) | battery charge state, rail voltages, a power button | This board is mains-powered over USB and has no battery. `poweroff` does not exist as a console command, because there is nothing to switch off |
| **An IMU** (QMI8658) | orientation, which the sibling uses for an idle policy | There is no panel to dim, so there is nothing an orientation would decide |
| **A codec and speaker** (ES8311) | a chirp on a new request | The LED is the whole notification (§10.17). A device with one emitter and no sound is quieter than the other one on purpose — this is a thing that sits next to a person working |
| **A panel and touch** | seven screens | §10.17 is the replacement, and it is not a smaller version of a screen — it is a different design, because one emitter cannot show two things and a screen can |

And the two that *are* on the board and have no job:

| Present, unused | Why |
|-----------------|-----|
| **TX / RX LEDs** (GPIO43 / GPIO44) | They are the CH343P's activity lights, wired to the UART lines themselves. They are not software-controlled and they blink whenever the console does. Nothing to decide |
| **The second core** | ESP-IDF pins the tasks this firmware creates wherever FreeRTOS puts them, and nothing here is CPU-bound. The dual core is a fact about the chip rather than a feature this design uses |

**8 MB of PSRAM is the one thing this board is unambiguously better at**, and it
is worth a line even though this firmware allocates nothing (§10.14.1). The two
are not in tension: everything this code owns is static, and what asks for heap is
other people's — the Wi-Fi driver's TX buffers, lwIP's pools, the USB Host
Library's transfer descriptors, cJSON while it parses. On the sibling board those
competed for about 200 KB of internal RAM and its §10.16 has the post-mortem of
what happened when they lost. Here there are eight megabytes behind them.
