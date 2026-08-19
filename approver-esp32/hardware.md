# approver-esp32 — the hardware, and the drivers over it

This file owns **§10.1**, **§10.13** and **§10.14.3** of the project docs: what
the board is and what the firmware may assume about it, which of its parts have
a job and which deliberately do not, and the leased I²C bus every driver on it
goes through. Section numbers are global and stable
([`../CLAUDE.md`](../CLAUDE.md) §2), so each keeps its number here — and they are
in **number order rather than narrative order**, so that a `§10.14.3` in a code
comment is found by scrolling rather than by reading.

Two things it does not carry, on purpose. The **pin numbers** are not here and
are not in any document: they live in `components/boards/board.h`, which names
Waveshare's own pinout sheet in `docs/` as their source (§10.1 says why). And the
**mechanics** — where ESP-IDF is installed, how to flash, how to open the port —
are [`working-with-code.md`](working-with-code.md).

Where the rest of §10 lives:

- [`CLAUDE.md`](CLAUDE.md) — what this device is, its status, and the map;
- [`firmware.md`](firmware.md) — the components above these drivers: Wi-Fi, the
  settings file, and the layering rules the rest of §10.14 states;
- [`protocol.md`](protocol.md) — the bus client, the key and the registration;
- [`screens.md`](screens.md) — the panel, the touch and everything drawn on them;
- [`web.md`](web.md) — the configuration site;
- [`tests.md`](tests.md) — how these drivers are tested with no board at all;
- [`build.md`](build.md) — the dependency set and what each part costs;
- [`commands.md`](commands.md) — every console command that reads this hardware.

### 10.1 The board — what the firmware may assume

**Waveshare ESP32-C6-Touch-AMOLED-2.16**
([product page](https://www.waveshare.com/esp32-c6-touch-amoled-2.16.htm),
[docs](https://docs.waveshare.com/ESP32-C6-Touch-AMOLED-2.16)):

| Part | What it is | Bus |
|------|------------|-----|
| ESP32-C6 | single RISC-V core @160 MHz, 512 KB HP SRAM + 16 KB LP SRAM, 16 MB flash, Wi-Fi 6 (2.4 GHz) / BLE 5 / 802.15.4 | — |
| CO5300 | 2.16″ AMOLED, 480×480, 16.7 M colours | QSPI |
| CST9220 | capacitive touch | I²C |
| QMI8658 | 6-axis IMU (accel + gyro) | I²C |
| PCF85063 | RTC, backed by the PMIC | I²C |
| AXP2101 | power management + charging (3.7 V Li-ion on MX1.25) | I²C |
| ES8311 + ES7210 | audio codec + echo-cancellation ADC, dual mic | I²C/I²S |
| BOOT / PWR / KEY | three buttons; `KEY` is the free one | GPIO |
| TF slot, USB-C, exposed I²C/UART pads | — | — |

**The GPIO map is not written here on purpose** — a pin number invented from
memory costs a bricked evening, and this document is not where numbers are
checked. It lives in exactly one place, **`components/boards/board.h`**, and
that file names its source: `docs/ESP32-C6-Touch-AMOLED-2.16-details-inter.jpg`,
Waveshare's own pinout sheet, kept in the repository so the numbers can be read
back against what they came from.

Two things it records that are not pins, and that shape boot order rather than
decorate it: **the panel's reset and the amplifier's enable are PMIC rails**
(ALDO3 and ALDO2), so the I²C bus and the AXP2101 driver have to be up before
the display can be brought up at all; and **the TF slot shares the panel's QSPI
wires** (only the chip select is its own), which is a second reason §10.13 gives
that slot no job.

**The `PWR` button is not the firmware's.** It is wired to the AXP2101's PWRON
pin (pressed = 0), and the chip acts on it whether or not any code is running.
Read off this board rather than off a datasheet — `power` prints all three:

| | This board |
|---|---|
| Power **on** | a short press: the threshold is **128 ms**, the shortest of the chip's four (128 ms / 512 ms / 1 s / 2 s, register `0x27` bits 1:0) — **or** plugging USB in, since VBUS insert is a power-on source in its own right, as is inserting a battery |
| Power **off** | a **6 s** long press (register `0x27` bits 3:2, of 4/6/8/10 s) — and it works only because `COMMON_CONFIG` (`0x10`) bit 2 is set. With that bit clear the chip measures the long press and does nothing. On this board it is set |
| Why it is awake | `PWRON_STATUS` (`0x20`), one bit per reason. A freshly cabled board reports `USB plugged in` |

**And the three rows above are written by `Init` now, not merely read.** The
driver used to print what it found in `0x27` and `COMMON_CONFIG` and call that
"how this board is configured"; `pmic::Config` carries them and `Axp2101::Init`
puts them there when what is on the chip differs. It is the same lesson this
section already draws about the TS pin and the charge currents — *a driver that
returns plausible numbers is not a driver that is configured* — and here the cost
of being wrong is the largest on the board: a chip holding a two-second press-on
threshold is a device whose button looks dead to anybody who presses it the way
a button is pressed, and one with `COMMON_CONFIG` bit 2 clear cannot be switched
off by its own button at all.

One rule inside that write, and it is the one worth stating twice: **configuring
the key must never write `COMMON_CONFIG` bit 0.** That bit is the soft power-off
and it shares a register with the long-press enable, so a read-modify-write that
preserved it would take the board down inside `Init` — a device that goes dark
every boot. The write clears it explicitly, `PowerOff` stays the only thing in
the driver that ever sets it, and both halves are tests.

Two consequences. **Power on and off are not features to implement** — they are
behaviour to avoid breaking, and GPIO18 exists so the firmware can *see* the
button, not so it can switch the board. And it is the same fact that makes
§10.7's `poweroff` refuse over USB: VBUS insert powers the chip on, so a soft
shutdown with the cable in is one the hardware immediately undoes.

**And GPIO18 sees it inverted, which the datasheet does not say and a board
does.** `PWR` at the PMIC is pressed = 0; at the ESP's pin it is the other way
round — GPIO18 rests at **0** (driven, not floating: it stays 0 with the
internal pull-up enabled) and goes high while the button is held. `BOOT` and
`KEY` are the ordinary way round, low when pressed. This was found by reading
the pin with the obvious polarity assumed and getting a button that was pressed
for the whole uptime, which is the argument for `buttons` (§10.7) existing at
all: a button driver that is never read back is a set of assumptions.

Where the rest comes from when it is needed — the TE line, backlight, the PMIC
and RTC interrupts, and the driver init sequences the sheet cannot carry:

- **The vendor's own examples**, which are the authority for this board:
  [`waveshareteam/ESP32-C6-Touch-AMOLED-2.16`, `02_Example/ESP-IDF-v5.5.3`](https://github.com/waveshareteam/ESP32-C6-Touch-AMOLED-2.16/tree/main/02_Example/ESP-IDF-v5.5.3).
  Note the folder name: the examples are built against **ESP-IDF v5.5.3**, which
  is the number §10.4 and §10.12 are arguing about.
- **`XPowersLib`, which that same repository vendors** — the register maps for
  the AXP2101 live there (`src/REG/AXP2101Constants.h`,
  `src/XPowersAXP2101.tpp`), and `components/pmic` cites them line by line
  rather than trusting anyone's memory. It is a source to read, not a
  dependency to add: nothing links against it.
- **`02_Example/ESP-IDF-v5.5.3/…/components/pmicpower`** — what the vendor
  actually *configures* on this board, which is a different question from what
  the registers mean. Reading it after the fact found four things missing from
  a driver that already worked: the TS pin left measuring (XPowersLib silences
  it inside `begin()`, and its own comment says the pin "will affect the
  charger"), the charge currents left at power-on defaults rather than this
  battery's 50/500/50 mA, the VBUS limit left below 2 A, and the rails never
  written to 3.3 V. **A driver that returns plausible numbers is not a driver
  that is configured** — the vendor's init sequence is worth diffing against
  even when nothing looks broken.
- The datasheets in `docs/` — CO5300, CST9220's family, AXP2101, PCF85063,
  QMI8658C, ES8311, and the C6 technical reference manual. I²C addresses are
  theirs, not `board.h`'s: they belong with each chip's driver (§10.14.2).

Anything taken from either goes into `board.h` with the source cited next to it,
never into a driver directly.

Two consequences that shape the firmware rather than decorate it:

- **No PSRAM, and no way to add it** — the ESP32-C6 has no external-RAM support
  at all. A full 480×480 framebuffer at 16 bpp is 480·480·2 = **460 800 bytes**
  against 512 KB of SRAM shared with lwIP, Wi-Fi and the TLS stack. So: LVGL
  draws into **partial buffers** (two of, say, 480×40 = 38.4 KB each), the panel
  is fed by DMA per flush, and "just render the whole screen" is not on the
  table. Any vendor demo that claims full double buffering on this chip is doing
  it with partial buffers too.
- **One core.** The UI, the NATS socket and the signature all live on the same
  CPU. Split them: an LVGL task (it owns the display and the touch), a bus task
  (socket, parse, reply), and a queue between them. A signature must never run
  inside an LVGL callback — the frame it stalls is the frame the operator is
  looking at.

### 10.13 Not in scope, but decided

- **Build it in this order** — after the library layer of §10.14, which comes
  before all of it. The screens themselves are not equally load-bearing.
  **The clock was built out of turn**, at the repository owner's request, and it
  is worth saying so rather than quietly renumbering: it is step 3 below and it
  arrived before step 1. What that bought was the AMOLED question — the drift and
  the dimness — answered in code on the panel that has the problem, rather than
  answered for the first time under the one screen that must not be got wrong.
  The **request screen** then followed, and step 1 is now **done**: the bus is
  open, the card is on the glass, the device is registered and a press is signed
  and published — `hook.verify_reply` says `trusted` for what comes off it. What
  none of that moved is the order for the rest, and step 2 is what is next:
  1. bus + registration + the **request** screen (§10.8.4) — the loop closes here,
     and until it does the rest is decoration;
  2. **Wi-Fi** (§10.9) and its screen — **both done now**, the screen in the
     reduced shape §10.8.6 records; a *password* is still set over the console,
     which is the one part of this step the keyboard would finish;
  3. **clock** (§10.8.2) and **settings** (§10.8.5);
  4. **limits** (§10.8.3) — last, because it is the only screen whose removal
     leaves everything else working, which is also the test that it stayed a
     readout.

  **Step 4 was built third**, at the repository owner's request and for the same
  reason step 3 was: the order above is about which screens are load-bearing, and
  the owner is entitled to want the desk object to show something. What it did not
  cost is the property that put it last — `components/watcher` and
  `components/responder` cannot name each other, so "delete the limits screen and
  the responder still works" is something you can perform rather than believe.
- **No authentication on the device.** Whoever can reach the screen can approve
  and have it signed — the key is bound to the chip, not to a person. Identical to
  `approver-web`'s position, and acceptable for the same reason (a single-operator
  tool on a desk) until it isn't.
- **Battery and sleep are a later question.** Light sleep with a socket open, or
  waking on Wi-Fi, changes the "is it connected" story (§10.8.1) and should not
  be designed around before the wired-and-plugged version works. Note what it
  collides with: a screen that blanks is fine, a *radio* that sleeps means
  requests arrive late or not at all — and a responder that is asleep is a
  responder that times out.
- **Of the rest of the board, three parts have a job and the others do not.**
  The PCF85063 keeps the clock (§10.8.2), the AXP2101 reports charge, and the
  codec plays the one chirp (§10.8.1). The IMU, the microphones and the TF slot
  do not: they are on the board, which is not a reason to use them. In
  particular, **no gesture ever approves anything** — a wrist-flick verdict is
  precisely the reflex §10.8.4 is built to prevent.

  **The IMU has a driver anyway, and that changes nothing above.**
  `components/imu` and the `imu` command exist so the chip can be read from the
  console — the same class of thing `power` and `date` are, and the only way to
  find out that a part of the board is alive. **It has a second reader now**, the
  status page of §10.8.5, and that changes nothing here: it is the same readout
  on glass instead of on a console, and the rule it has to keep is the one below.
  Nothing in the approval path may read it; a tilt is not a press. What reading it established, which the
  datasheet could not: this board's QMI8658C answers at **0x6B** (SA0 pulled
  down — and note the addresses are the inverse of the habit, 0x6A being the
  floating one) with revision 0x7c, and two of its three axes are now tied to
  the case — by putting the board in a known position and reading it, which is
  the only way this is ever known:

  | The board is | Gravity reads | So |
  |---|---|---|
  | flat on the desk, screen up | along **+Z** | +Z points out of the back; the screen faces −Z |
  | stood on its USB connector, buttons up | along **−Y** | +Y points at the button edge, −Y at the connector |
  | stood on its card slot, speaker up | along **+X** | +X points at the card-slot edge, −X at the speaker |

  Right-handed, and every row of it was read off the board in a known position
  rather than taken from a drawing — which is the only way the *sign* is ever
  right. **That table is code now**, in `ui/idle_policy.h`, with three readers:
  the console's tilt line, the motion status page of §10.8.5, and the panel's
  blank (§10.8.1), which asks it one question — is this the USB edge. It moved
  there the moment the second reader existed, because two copies of six
  positions is two chances to get a sign wrong on the one subject that can only
  be established by putting the board in each position and reading it.

  **And there is no magnetometer on this board, which is a question worth
  answering once**: the QMI8658C is a six-axis part, `docs/` holds no
  magnetometer datasheet, and the schematic's complete net list — 127 labels —
  has nothing of the kind on it, the sensor lines being exactly `QMI_INT1/2`,
  `RTC_INT`, `TP_INT` and `AXP_IRQ`. So there is no heading to display and no
  compass to add; the position line above is what an orientation readout can
  honestly be. The chip's register map has room for magnetometer data, which is
  where the expectation comes from — it is filled by an external magnetometer
  wired to the chip, and there is none here. Half of the work in `imu` is that sign: an accelerometer at rest reads
  +1 g along the axis pointing **up**, so the axis gravity acts along is the
  negation of the dominant reading, and getting it backwards is invisible until
  the thing is turned over.

  Numbers worth writing down before someone calls them a bug: the acceleration
  magnitude at rest reads **0.964 g** flat, **1.062 g** on the USB edge, **1.018
  g** on the card edge and **0.980 g** on the speaker edge, against 1.000 every
  time, and the gyroscope sits at about **−3 dps on X** while perfectly still.

  **The two interrupt lines do nothing, and finding out why produced the one
  fact the datasheet does not carry.** §6.1 says INT1 is general purpose (a
  ~4 ms chip-ready pulse after reset, the CTRL9 handshake, wake-on-motion) and
  INT2 means data-ready — *pulsed* at the output rate rather than held, because
  this driver leaves `syncSmpl` clear in CTRL7. Both read a steady low on this
  board, and not for want of anything to report: **CTRL1 bits 4 and 3 are the
  INT1 and INT2 pin enables**, which rev 0.9 calls reserved while its own
  revision history says it "updated the INT1/INT2 enable bit in CTRL1". Setting
  them starts INT2 pulsing and clearing them stops it — measured, both ways.

  Two things came out of that experiment. INT1 stays low **even enabled**, which
  is correct: nothing in this firmware runs a CTRL9 command, wake-on-motion or a
  motion engine, so there is nothing to raise it. And the pulse rate came out at
  ~225 Hz against a configured ODR of `0101` — which is the datasheet's **6DOF**
  column, 235 Hz, not the 250 Hz of its accelerometer-only one. Note 13 of
  Table 26 says the rate is derived from the gyroscope when both sensors are on;
  this is that note, confirmed on the board.

  The enables stay **off**, as a `Config` flag rather than an omission: toggling
  a pin a couple of hundred times a second for a line nobody polls is current
  spent on nothing. The pins are configured as inputs with a pull-down so that
  "the chip is not driving this" and "the pin is floating" do not read the same,
  and `imu` counts edges over 20 ms rather than sampling a level — the datasheet
  is explicit that the INT2 pulse width depends on the ODR and that a level
  read is not to be trusted.

  **X was read in both directions, and that pair separates offset from scale**
  — which no single reading can. Card-slot down gives −1.016 g, speaker down
  gives +0.978: half their difference is the sensitivity, **0.997 g**, and half
  their sum is the bias, **−0.019 g**. So the ±8 g range and the LSB-per-g
  conversion are right to 0.3 %, and every deviation above is zero offset of the
  ordinary uncalibrated kind. Doing the same for Y and Z means turning the board
  over twice more; nobody has needed it, because — per the rule above — nothing
  uses the IMU.

### 10.14.3 The I²C bus is shared, so it is leased

*A subsection of §10.14 ("how it is written"), kept here because it is the
contract every driver on this board obeys, and where three driver bugs were found. Its
siblings — the language, the layering and the house precedent — are in*
[`firmware.md`](firmware.md)*.*

Five of the board's chips hang off one I²C bus (§10.1): CST9220 touch, PCF85063
RTC, AXP2101 PMIC, QMI8658 IMU, and the ES8311/ES7210 codecs. Several tasks want
them at once — LVGL polls touch every frame, a slow timer reads the clock and
the charge, the codec gets configured for a chirp — on **one core** and one set
of wires. So there is exactly one owner of the bus, and everything else borrows
it:

**acquire → work → release.** Nobody calls `i2c_master_*` outside this library;
every driver takes a lease first. In C++ that lease is a scope guard, so
"release" is not a line anyone can forget to write (§10.14.1).

- **A lease exists to make a *sequence* atomic**, not a single transfer. One
  transaction needs no help; a read-modify-write on the PMIC, or a touch
  controller's register-select followed by a burst read, must not have another
  task's transfer land in the middle. That is the whole reason this is a lease
  and not a wrapper. **The alternative has been tried and its cost is visible**:
  the house firmware of §10.14.4 takes its lock *inside each method*, so a
  single read is safe and a sequence is not — and the API grew a
  `EEPROMMultiblockWrite` whose only reason to exist is to hold several
  transfers together under one lock. Per-call locking does not remove the
  problem; it moves each instance of it inside the bus class, one bespoke method
  at a time. A lease lets the device driver compose the sequence from outside,
  and the bus keeps four operations.
- **Acquire with a timeout, and a failure is a logged skip — never a block.**
  A wedged codec must not freeze touch, and a task waiting forever on a bus is a
  watchdog panic with a confusing name. The caller decides what a miss means:
  for the clock, use the last value; for touch, drop the frame's read.
- **Hold it briefly.** Nothing sleeps, retries a network, or draws while holding
  the bus. A lease held across an LVGL flush is a bug even when it works.
- **Recovery belongs to the owner — and takes the lease like everything else.**
  A slave holding SDA low is a known I²C failure with a known fix (clock out
  until it lets go, then re-init); it is handled once, in the bus, with a
  bounded number of attempts and one log line — not five times in five
  drivers. It is also the most destructive thing in the class: it removes
  every device handle and deletes the driver, so running it beside another
  task's transfer hands that task a handle that has been freed. It skipped the
  acquire at first, and on one core with preemption that is a use-after-free
  rather than a race that usually works. Now it waits — longer than an
  ordinary acquire, because the holder it is waiting on is by definition the
  stuck one — and if the bus does not come free it tears nothing down and says
  so. Which makes it the second thing on this bus that **must not be called
  from inside a lease**: the mutex is not recursive, and `AddDevice` is the
  first.
- **The speed is per device, and this reverses what this section used to say.**
  The old rule was one clock for the wire, the minimum the slowest chip
  tolerates — which is the right rule for the legacy driver, where the clock is
  a property of the port. `driver/i2c_master.h` puts `scl_speed_hz` in the
  **device** config, so a slow chip costs only its own transfers. The vendor's
  driver for this board proves the case: it opens the AXP2101 at 100 kHz while
  nothing else has to come down with it. So: the bus has a default (400 kHz),
  a driver declares its own with `Bus::AddDevice(address, hz)` if it needs to,
  and the owner is still the only one who opens a device. What has not changed
  is that a driver does not get to reconfigure *the bus* — the thing it may
  pick is its own line rate.
- **It is fake-able, and that is a requirement.** The backend is an interface
  with two implementations: the IDF driver, and a host-side fake that records
  transfers and can be told to time out or NACK. Without it, the lease
  semantics — contention, timeout, recovery — are only testable on hardware,
  and §10.11 says that tier is the opt-in one.
- **Use the new driver** (`driver/i2c_master.h`, bus and device handles), not
  the legacy `driver/i2c.h` the older house code is written against — it is
  deprecated on IDF 5.x, and the handle-per-device model is what a lease wants
  anyway.

**What is written, and what of this section is not.** `components/i2cbus` has
the lease as a scope guard, the timeout-that-skips, the per-address device table
(fixed at eight slots — five chips are on the wire), and `Recover()` clocking
SCL nine times with the bus torn down and rebuilt around it. `components/pmic`
is its first user and demonstrates the point: the ADC-enable read-modify-write
and the whole status read each happen under **one** lease, so the numbers are a
snapshot rather than five values from five moments.

The pins are **arguments to `Bus::Init`**, not an include of `board.h` — the
library layer knows about wires, not about which board they are on (§10.14.2),
and `components/boards` is what puts the two together.

**The touch controller is where this rule was nearly lost, and the save is
worth recording.** `esp_lcd_touch` opens its own I²C device on the bus handle
and talks to it directly; it has never heard of the lease and cannot be taught.
The short path is to hand its handle to `lvgl_port_add_touch` and let the LVGL
port poll it — which puts an I²C transfer in the LVGL task that no lease
covers, so a read-modify-write on the PMIC could be split by a touch read.
That is the exact failure this section exists to prevent, arriving through a
third-party component rather than through our own code.

So the port's touch integration is **not** used. `display::Touch::Read` polls
the controller itself, holding the lease across the call, and LVGL gets a
pointer input device whose read callback is that function — twenty lines, and
the vendor's transfers land inside a critical section it knows nothing about.
`i2cbus::Bus::Handle()` exists for this and says so: it hands out the bus
handle, not permission to skip the lease. A lease it could not get is a dropped
frame's read and a counter the `display` command prints, never a block — this
section already named touch as the case where that is the right answer.

**The fake exists now, and it is not the interface this section asked for.**
The requirement was "an interface with two implementations: the IDF driver, and
a host-side fake". What was built instead shadows ESP-IDF's *headers* on the
host include path (`host_test/fakes/`), so `i2c_bus.cpp` compiles unmodified
and the file under test is the file that ships. One fact decided it: **the
lease's mutex is a FreeRTOS object and is not behind any bus backend**, so a
`Backend` interface would have needed the FreeRTOS shims anyway and then added
a vtable on every transfer on top of them. The property this section wanted —
contention, timeout and recovery testable without hardware — is delivered
either way; this way costs no production code.

What it bought beyond the bus is the argument for it: every driver on this
board includes exactly `i2c_bus.h`, `esp_err.h`, `esp_log.h` and FreeRTOS, so
the same shims made the PMIC, the RTC, the IMU and the codec testable at no
extra cost. §10.11 lists what is covered.

**And it found three things, all now fixed.** None was visible on hardware,
which is the point of the tier existing at all — the board worked either way.

- **`Es8311` took the lease per call.** Its private helpers each did their own
  `Acquire()`, so a two-dozen-register init was two dozen separate leases:
  precisely the per-call locking this section argues against and quotes the
  house firmware for. They take a `i2cbus::Lease &` now and the public methods
  own one acquire each. Note the trap that shape carries: `Init` may not call
  its own `SetVolume`/`Mute`, because the bus mutex is **not recursive** — the
  `Apply…` helpers exist for exactly that, and the header says so.
- **Both `Es8311` and `Qmi8658` slept while holding the bus** — 20 ms and 15 ms
  respectively, waiting out a chip's reset. This section's other rule ("nothing
  sleeps, retries a network, or draws while holding the bus") had no way to be
  checked before: on a board it simply works, at the cost of a dropped touch
  read and a skipped clock tick nobody would ever trace back. Both drivers now
  cut the sequence exactly at the wait, so a lease is either side of it and
  never across it.
- **`Bus::Recover` did not take the lease at all** — the rule above, broken by
  the one function with the most to lose from it. Found by asking the question
  the other two tests had already taught: what does this look like when
  somebody else is holding the bus? It waits now, and refuses rather than
  tearing down.

The fake counts milliseconds slept with the mutex held, which is what makes the
second one an assertion rather than a code-review habit. The first is asserted
as a lease *count* — not "exactly one", because a driver that must let go
around a wait legitimately takes more than one, and a bound is the honest form
of the rule. The third is asserted the way §10.14.3's timeout rule already
is — as the tick count the acquire asked for — because a fake mutex that never
blocks is the only place "it waited, and bounded" is a visible fact.

