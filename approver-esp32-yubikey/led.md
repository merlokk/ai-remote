# The light (§10.17)

**One WS2812 on GPIO48 is the entire user interface of this device.** Not a
reduced screen — a different kind of thing, and this document is the design
rather than a list of colours.

The sibling board answers "what is going on" with seven screens, each of which can
show several facts at once. One emitter can show exactly one, so the question has
to be answered by **ranking**: at any moment a handful of things are true about
this device — no internet, not registered, a request waiting — and precisely one
of them gets the light.

That ranking is `components/indicator/indicator_policy.cpp`, it has no ESP-IDF in
it, and §10.11's host tier runs every branch of it. That is deliberate: a
fourteen-way decision that could only be checked by unplugging things and watching
a desk would be a decision nobody ever checked.

## 10.17.1 What each state looks like

Three of these were asked for by name by the repository owner — **red on boot,
yellow flashing until NATS, green breathing when working** — and the rest are
built around them.

| State | Colour | Rhythm | Brightness | What it means |
|-------|--------|--------|-----------|---------------|
| `booting` | **red** | solid | full | `app_main` is still composing |
| `restore-window` | white | solid | full | BOOT is down; keep holding to restore `config.json` |
| `signing` | blue | solid | full | a decision is being signed right now |
| `pending` | **white** | fast (200/200 ms) | full | **a request is waiting for you** |
| `fault` | red | fast | full | something that should work did not |
| `no-storage` | **yellow** | fast | full | the filesystem would not mount |
| `no-device-key` | **yellow** | fast | full | no Ed25519 identity |
| `no-wifi` | **yellow** | fast | full | no link |
| `no-internet` | **yellow** | fast | full | associated, and nothing answers |
| `no-bus` | **yellow** | fast | full | no NATS connection |
| `not-registered` | magenta | 500/500 ms | full | run `register <token>` |
| `not-enrolled` | cyan | fast | full | run `key enrol` |
| `no-fido-key` | cyan | 500/500 ms | idle | plug a key into the OTG port |
| `watching` | cyan | beacon (100 ms / 2 s) | idle | connected, not yet on `approvals.*` |
| `ready` | **green** | breathe (~9 s) | idle | nothing to do, and able to do it |

And one that is not a state at all: **a verdict flashes for 1.5 s** — green solid
for an allow, red solid for a deny — over whatever is underneath, and then the
ranking comes back. It is pushed rather than polled, because a verdict is an event
and by the next tick the device is already idle again.

### The three decisions in that table

**Red on boot is not decoration.** It is the only state that appears before
anything has had a chance to go wrong, so it proves the emitter, the UART and the
encoding all work. A board that comes up dark has a hardware fault; a board that
comes up red has a firmware that is running. It is also the answer to "did it
reset" from across a room.

**Five states share one yellow, and that is the design rather than a shortcut.**
Storage, key, Wi-Fi, internet and bus are a *stack*: each makes the next possible,
and the operator has one thing to do about all of them — plug the device into a
network that has the bus on it. Five yellows told apart by blink rate would be
five things to memorise for one action. What is lost is diagnosis at a glance, and
it is not lost far: `status` on the console names the exact rung, `led` prints it
in words, and every transition between them is a log line. **The light says *not
yet*; the console says *why*.**

**Past the bus the differences matter again**, which is why `not-registered`,
`not-enrolled` and `no-fido-key` get colours of their own. Each has a *different*
thing for the operator to do — mint a token, enrol a key, or find the one in their
pocket — and unlike the yellow stack, none of them is fixed by the action that
fixes the others.

### Two rules the table obeys

**A pending request outranks everything except boot — including a fault.** This
is the one place the "report the lowest broken rung first" rule is deliberately
broken, and the reason is deadlines: a request has one and a fault does not. A
device that showed a fault instead of a waiting request would be letting the
request expire in order to say something the console could have said at any time.

**No two states may look alike**, and a test enforces it
(`test_indicator_no_two_states_look_alike_outside_the_yellow_stack`). With one
emitter, two states sharing a colour *and* a rhythm is two states the operator
cannot tell apart. The five yellows are the one permitted collision, because they
share an *action* as well as an appearance. Nothing is ever dark, either — a state
that showed nothing would be a device that looks unplugged.

## 10.17.2 Which colour means what is not a setting

`config.json` has two LED fields and both are **brightness**:

```json
"led": { "brightness": 15, "idleBrightness": 7 }
```

The palette is compiled into `led_frames.h` and cannot be reached from the
console, the settings file or the bus. **An operator who could recolour `denied`
could build a device that lies about what it did**, and on a device whose only
output is one emitter, which colour means what *is* the protocol.

The two numbers are a ceiling and a resting level, not a level and an override:

* **`brightness`** is what a state that wants a human is allowed to reach —
  `pending`, `fault`, `booting`, a verdict flash. **15 % by default**;
* **`idleBrightness`** is what the resting states settle to — `ready`,
  `watching`, `no-fido-key`. **7 %**: findable in a dark room, not read from
  across it. A device that is *fine* should not be a lamp.

**Both numbers are lower than they look, and §10.17.5 is why.** They went
40 → 70 → 50 → 15 over four sittings with the board on a desk, and the last step
was not a change of taste — it was noticing that the top three quarters of the
range do almost nothing.

A request never uses the idle ceiling. That is a rule in `LookOf` rather than a
convention, and it is what stops a quiet-brightness setting from making a request
invisible.

`config set led <0..100>` and `config set ledidle <0..100>` change them, and both
are applied **at once** rather than at the next state change — a brightness you
cannot see the effect of is one you cannot judge. **That it took four sittings to
settle them is the argument for their being file fields rather than constants**: a
brightness cannot be chosen on paper.

## 10.17.5 Why 15 and 7 are not as dim as they sound

An observation from the desk, worth writing down because it will otherwise be
re-discovered as a bug: **turning this LED up stops doing anything at about 20 %,
and the range from there to 100 is nearly wasted.**

Two things cause it and they compound.

**1. `Scale` is linear in duty cycle and the eye is not.** Perceived lightness
goes as roughly the cube root of radiated power — CIE L\* is `116·Y^(1/3) − 16` —
so the numbers this device takes and the brightness a person sees pull apart
badly:

| duty (`config set led`) | 2 % | 5 % | 7 % | 10 % | 15 % | 20 % | 30 % | 50 % | 70 % | 100 % |
|---|---|---|---|---|---|---|---|---|---|---|
| **seen** (CIE L\*, 0–100) | 15.5 | 26.7 | 31.8 | 37.8 | 45.6 | 51.8 | 61.7 | 76.1 | 87.0 | 100 |

**Half of everything a person can see is in the bottom fifth of the numbers.**
20 % already looks like slightly over half of full; the remaining 80 % of the
range buys the other 48 points, and the last half of it — 50 to 100 — buys 24.

So 15 % is not "dim". It is a bit under half of what this emitter can look like,
and 7 % is a bit over two thirds of *that* rather than the one half the
arithmetic suggests.

**2. Above roughly a fifth the colour goes.** This is a bare RGB die with no
diffuser, at arm's length. Past that point the eye reads a point source that
bright as glare rather than as a hue, and a saturated colour turns into a white
dot with a tint. On a device where **which colour it is** *is* the interface
(§10.17.2), losing hue discrimination costs more than the extra light is worth.

Between them, the useful range on this part is about **5 to 25**, and both
defaults sit inside it.

### Why the scale is left linear anyway

The obvious fix is to gamma-correct `Scale`, so that `config set led 50` means
"half as bright as it looks at 100" rather than "half the duty cycle". It is
deliberately not done, for the reason the function's own comment states: **this is
the operator's ceiling, and a ceiling that is not proportional to the number typed
is a ceiling nobody can reason about** — a `led` readout saying `50%` next to an
emitter running at 12 % duty is a readout that has to be explained every time.

The perceptual curve *is* applied, in the one place it belongs: `kBreathRamp`,
the sixty-step Weber–Fechner table the breath walks (§10.17.3). There it is not a
setting anybody reads back — it is the shape of an animation, and a linear ramp
there looks like a lamp that snaps on and then does nothing for most of its
travel, which is exactly the effect this section is about.

If the numbers ever want to mean perceived brightness instead, the change is one
line in `Scale` plus a re-derivation of both defaults — and this section is the
note to read first.

## 10.17.3 The wire: a WS2812 driven from a UART

**This is the house firmware's trick, not a new idea here** — it comes from
`E:\projects\Zesec.ModuleX.Firmware.v3`, `main/led.cpp`, the same place §10.14.4
borrows shapes from, and the numbers came with it because they were chosen against
a real emitter.

A WS2812 reads a 1.25 µs bit cell whose duty cycle carries the value. A UART at
**3,333,333 baud with six data bits**, one start bit and one stop bit puts eight
bit-times of 300 ns each on the wire — two WS2812 bit cells per character — and
four characters spell one byte of colour. With **TX inverted** (the line idles low,
which is what WS2812 wants) each pair of colour bits is one of four fixed
characters:

| bits | character |
|------|-----------|
| `00` | `0x37` |
| `01` | `0x07` |
| `10` | `0x34` |
| `11` | `0x04` |

Twelve bytes per pixel, **green first** — that is the part order, and getting it
backwards swaps red and green, which on this device swaps `allow` and `deny` on
the verdict flash. It is the single worst way `EncodePixel` could be wrong, and
there is a test that decodes a frame back by hand for exactly that reason.

The divisor off the 80 MHz APB clock is exactly 24, so there is no rounding error
to accumulate over twelve bytes.

**What it buys over RMT** is a peripheral this firmware was not otherwise going to
use, no managed component, and a write with no ISR of our own. **What it costs is
a UART**, and on this board that is UART1: UART0 is the console on the CH343P
bridge and is not negotiable.

Two numbers in `led.cpp` look like mistakes and are not:

* **the receive buffer is 256 bytes on a line that is only ever driven.**
  `uart_driver_install` refuses a receive buffer at or below the 128-byte hardware
  FIFO — it answers `uart rx buffer length error`, which is exactly how this was
  found: on the board, with the LED dark and one line in the boot log;
* **the transmit buffer is zero**, which makes `uart_write_bytes` blocking. Twelve
  bytes at 3.33 Mbaud is 29 µs; a ring buffer would add an interrupt and a copy to
  save a delay shorter than the scheduler's tick.

Each frame is followed by `uart_wait_tx_done`, because the next write must not
start inside the 50 µs of idle line the part uses to latch — a second frame that
arrives too early is a second *pixel* rather than a new colour for the first.

## 10.17.4 How the light finds out

`components/indicator` knows about a light and about a struct of booleans, and
about nothing else. It has never heard of a radio, a bus, a key or a request.

The facts arrive through a **gatherer** that `main` registers — the same hook
shape `config::OnChanged` uses, and for the same reason: the list of who to ask
would otherwise be duplicated in every caller that wants the light refreshed.
`main` is the one file in this firmware allowed to depend on everything, so that
is where the list lives.

The test that this layering is real: **deleting `responder` must leave a working
indicator, and deleting `indicator` must leave a working responder.** Neither
includes the other; `responder` calls into `indicator` twice (a poke when a
request arrives, a flash when a verdict leaves) and the direction never reverses.

The task ticks twice a second, which is fast enough for a network event or a
human. A request does not wait for it — `indicator::Poke()` is called the moment
one is queued, because half a second of a deadline spent on nothing is half a
second wasted.

**The state is re-applied every tick, not only on a change.** That costs a
comparison — `Animator::Set` is a no-op when nothing moved, which is why it takes
a clock — and it buys the case where a verdict flash has just expired and the
state underneath has to be put back without anything having changed.
