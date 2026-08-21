# approver-esp32 — the screens, and what each of them refuses to do

This file owns **§10.8** of the project docs — all seven screens of this device
and the boot splash under them: the navigation model (§10.8.1), the clock
(§10.8.2), the limits (§10.8.3), the request card the device exists for
(§10.8.4), the settings list with the status pages, the touch test and the two
destructive rows (§10.8.5), and the Wi-Fi screen with the list of what is on the
air (§10.8.6). Section numbers are global and stable
([`../CLAUDE.md`](../CLAUDE.md) §2), so §10.8 keeps its number here.

The shape every one of them takes is the same, and it is `§10.14.2`'s split
([`firmware.md`](firmware.md)) applied per screen: a `ui::` half that decides and
includes `<cstdint>` and nothing else, and a `screens::` half that paints and
decides nothing. Which is why the deciding half of each screen is under the host
tests of [`tests.md`](tests.md) and the painting half is not.

Where the rest of §10 lives:

- [`CLAUDE.md`](CLAUDE.md) — what this device is, its status, and the map;
- [`hardware.md`](hardware.md) — the panel, the touch, the buttons and the IMU
  these screens read;
- [`firmware.md`](firmware.md) — the Wi-Fi manager and the settings file behind
  them;
- [`protocol.md`](protocol.md) — the bus, the key and the registration that turn a
  press on the request card into a reply;
- [`web.md`](web.md) — the same device configured from a phone instead;
- [`tests.md`](tests.md) — every rule below, as an assertion;
- [`build.md`](build.md) — LVGL's version, its pool, and what a font costs;
- [`working-with-code.md`](working-with-code.md) — how to photograph the panel.

### 10.8 The screens

Seven now, and two of them are not navigated to — one arrives with a message and
one arrives with the numbers:

| # | Screen | Reached by | Exists to |
|---|--------|-----------|-----------|
| 10.8.2 | **Clock** — home | the screen it returns to from everywhere | be the thing on the desk for 99 % of its life, and admit in one glyph whether it could answer a request right now |
| 10.8.3 | **Limits** | **a `status` document arriving** — it comes up on its own and leaves a minute after the stream stops; a swipe left/right from the clock still reaches it, and nothing depends on that | the §9.7 `status` document: which model is answering and how much of the 5h / 7d windows is spent — present only while a Claude Code session is actually publishing. Plus one line of §9.10's `activity`: what that session is *doing* right now |
| 10.8.4 | **Request** | **a message on `approvals.*`** | the one screen the device exists for |
| 10.8.5 | **Settings** | a swipe up, or `KEY` held two seconds — from the clock **and from the limits**, which arrive on their own | a short list of places to go — Wi-Fi, the status pages, the touch test — and three things to do: save the settings, reload them, and restart. Seven rows, five of them on the glass at a time |
| 10.8.5 | **Status** | from Settings | three pages of what the board is doing — power, system, motion — for the questions a console answers and a desk object cannot |
| 10.8.5 | **Touch** | from Settings | show where the finger is, and correct it — the one screen that has to work while the thing it tests does not |
| 10.8.6 | **Wi-Fi** | from Settings | the mode, and one record at a time: the access point's own name and password, or one remembered network's (the machinery is §10.9) |
| 10.8.6 | **Networks** | from Wi-Fi | what is on the air, and a name picked out of it into the record that was on the glass |

The last two are one level deeper than the rest — settings, then Wi-Fi, then the
list — and each `PWR` is one level of it, which is the rule §10.8.5 already keeps
for the status pages.

None of them needs a board to be drawn: §10.12.1 renders LVGL on the host and
returns a picture — with the caveats stated there about what a picture proves.

**And one picture that is not a screen: the boot splash.** White katakana,
Matrix-fashion, on the glass from the moment the panel is up until LVGL takes
it over. It is deliberately *not* an LVGL screen and not in the table above —
it exists in the window before LVGL owns anything, which is also the only
window in which the panel is up and there is nothing to show.

- **The boot sound plays under it, and that is what decides how long it is
  up.** `Speaker::PlayWav` blocks for the length of the file, the picture needs
  no CPU to stay on the glass, so the three seconds the chime costs are three
  seconds of splash rather than three seconds after it. The two used to
  stack — a device that lit up silently and then beeped at a clock — and
  unstacking them made the boot 1.7 s shorter as a side effect. The `2000` in
  `main.cpp` is a floor for a device with no codec, not a duration.
- **It is a file, not code**: `spiffs_image/splash.bin`, generated on the host
  by `tools/make-splash.ps1` and flashed with the SPIFFS image. Raw RGB565 in
  the panel's byte order, no header, no decoder — `components/display/rawimage.h`
  argues it, and the argument is `speaker.h`'s about WAV, applied to pixels.
  460 800 bytes of an 11 MB partition.
- The generator is **Windows PowerShell 5.1** rather than Python, and that is
  the dependency ledger rather than a preference: rasterising a glyph needs a
  font engine, `System.Drawing` is in the box on every Windows machine, and
  every other route meant a new entry on root §1's list.

#### 10.8.1 The model — priority, not a stack

Navigation is a small explicit state machine, not "whatever LVGL screen was
loaded last", because two of the transitions are not the operator's:

- **The request overlay outranks everything.** It appears over any screen —
  mid-scroll, mid-password, mid-anything — and while it is up, navigation is
  gone: no swipe, no held button, no back. There is nothing to reach that is more
  urgent than the card, and a device where a stray swipe can hide a pending
  request is a device that will silently time one out.
- **It is not modal in the other direction either.** The card cannot be
  *dismissed*, only answered or expired. There is no ✕, because a ✕ would be a
  third verdict that §7 does not have.
- What was underneath is restored exactly when the card goes: the settings
  screen keeps its scroll position and any half-typed password. Losing a
  password to an arriving request is how an operator learns to resent the
  device.
- **Everything else is quiet.** No screen may steal focus for a readout: fresh
  limits, a Wi-Fi reconnect and a finished registration all change what a screen
  *shows*, never which screen is up. `approver-web` states the same rule about
  its plaque, for the same reason.

**One LVGL task owns the display.** LVGL is not thread-safe: every widget touch
happens in the UI task, and the bus task hands work across a queue (or takes
`lvgl_port_lock()` and gets out fast). A signature, a socket read and a JSON
parse never run inside an LVGL callback — §10.1 already says why, and this is
where it becomes a code rule rather than an observation.

**Shared across all seven:**

- **A link indicator, always visible.** The dot from `statusline` (§9.8) is the
  precedent: the operator must be able to tell "nothing is being asked" from "I
  am not connected". Three states, not two — bus up / bus down / not registered
  — because a connected device with no key is just as unable to answer, and
  looks identical from the outside otherwise.
- **AMOLED: black is free, static is expensive.** An unlit pixel costs no power
  and no lifetime, so these screens are mostly black by design rather than by
  taste. Anything permanent (the clock, the dot) shifts by a few pixels on a
  slow timer, brightness drops on an idle timeout and the panel blanks after
  it — waking on touch, and unconditionally on a request. Burn-in on a device
  showing one layout for months is an outcome, not a risk.

  **Both halves of that sentence are true now, and for a long time neither
  was.** `dimSeconds` and `blankSeconds` were in `config.json`, were parsed into
  `config::Data`, were written back by `config save`, were printed by `config`,
  had three host tests each — and were **read by nothing at all**. The panel
  never dimmed and never blanked. That is the finding §10.15 records about the
  brightness arriving a second time on the two fields next to it, and it is the
  worse instance of it: a brightness that does nothing is a number somebody
  notices, and a dim that does nothing is a promise about the life of the glass.

  So the two are gone, replaced by three under **new names** — `dimAfterSeconds`,
  `dimPercent`, `sleepAfterSeconds` — because a `config.json` already on a device
  would otherwise carry a 30-second dim into firmware where 30 seconds means
  something else. `ui/idle_policy.h` is where the decisions live and
  `screens.cpp` is what tells the panel; the shipped numbers are the repository
  owner's: **dim to 30 % after fifteen minutes, off after twenty-five.**

  Three rules that are not the obvious ones:

  - **dimming is unconditional and the blank is not.** The panel only goes off
    with the board standing on its USB edge, buttons up — one of §10.13's six
    measured positions, and the one nobody reads a clock in. This device's whole
    value is that a glance at the desk says the loop is alive (§10.8.2), and a
    black square says nothing; so lying flat it dims and stops there, however
    long it is left. That was checked on the board rather than reasoned about:
    fifteen seconds past a fifteen-second threshold, standing on the *card-slot*
    edge, and it stayed dimmed.
  - **a `status` document is activity**, which is the operator's own definition —
    "nobody presses anything and no messages arrive". §9.7 publishes on every
    render, so the screen stays lit for as long as a Claude Code session is
    spending and gives up a quarter of an hour after it stops. Visible in the
    log of the run above: the panel came back to full brightness every few
    seconds without anybody touching it, which is this repository's own status
    line arriving.
  - **`config set` is not activity.** It is typed over USB, which is not
    somebody looking at the glass — so a `dim` shortened on a screen that has
    already been idle ten minutes takes effect at once instead of starting the
    wait again. Also confirmed on the board: the idle counter climbed straight
    through two console commands.

  And one hazard that needed its own answer: **the finger that wakes the panel
  from the blank must not also press what is under it.** With the display off
  the operator cannot see what they are touching, and the worst thing under it is
  the reboot row of §10.8.5. `display::SwallowTouch` is a latch in LVGL's own
  read callback — it counts the press, so the device wakes, and reports a release
  to LVGL until the finger lifts. It is armed by the blank and never by the dim,
  because a dimmed screen is one the operator can still read.
- **The queued touch.** A card appears while a finger is already on its way
  down, and on a 480×480 panel Allow may be exactly where the operator was about
  to tap. Ignore presses for the first ~300 ms of any newly presented card, and
  discard any touch whose press began before it appeared. A console prompt and a
  browser tab get this guard for free; a desk object does not.
- **The alert.** There is a codec and a speaker here: one short sound on a new
  request. `approver-web`'s "The new-request alert" applies — never chirp for a
  card that was already there.

  **Written, and it needed a task of its own.** This section originally said "no
  asset" and `screens.h` originally said the chirp was the caller's job; both are
  now the other way round, for one reason: `Speaker::PlayWav` blocks for the
  length of the file, and `alert.wav` is about three and a half seconds. There is
  no task in this firmware that can afford that — the screen task would miss the
  press it exists to see, the bus task would stop reading the socket, and the
  responder task would hold up the signature. That was an argument against playing
  it *on any of those tasks*, not against playing it. So it has 4 KB and a
  semaphore, and the worst a stalled chirp can do is delay the next chirp.

  It lives in `screens::Inject` rather than in whoever raised the card, which is
  the reversal that matters: a card going up is a property of the card, so a
  request off the bus and a `request test` make the same noise and neither caller
  has to remember. The semaphore is **binary**, so four cards arriving together
  are one sound rather than four — which is this section's "never chirp for a card
  that was already there", from the other end.

#### 10.8.2 Clock — the home screen

Big time, small everything else, and it must not lie about the one thing the
device is for. On it: the time, the date, and the three indicators — Wi-Fi, the
bus dot, and the battery with its percentage under it.

**That is the whole of it, and the list is now closed.** An earlier draft of this
section also named `key_id` and a gear; both are struck rather than owed. The
`key_id` is what `keys` on the console answers and what the front page of §10.16
shows, and a device with one identity does not need to caption itself on the screen
it spends 99 % of its life on — the point of that screen is that a glance says the
time and whether the loop is alive. The gear went for the reason §10.8.5 already
records from the other end: settings is reached by a swipe up, by holding `KEY`, and
from the limits screen, so a gear would have been a fourth way in and a permanent
object on an AMOLED that §10.8.1 spends a section keeping dark.

- **Time comes from SNTP, is kept by the PCF85063, and survives a reboot.** The
  RTC is the source at boot (instant, offline); SNTP corrects it once the network
  is up and writes the corrected value back. A device that has never had either
  shows `--:--`, not `00:00` — a plausible wrong time is worse than an obviously
  unset one, the same call §10.7 makes about `ts`.

  **Both halves exist now.** `components/rtc` and `board::Init()` adopt the RTC
  into the system clock at boot; the console reads and writes it with `date`
  (§10.7); the zones below are there and the RTC holds UTC rather than whatever
  was typed. And `components/timesync` is the network half — the split below
  says how it is shaped and what it refuses to do.

  The chip makes the `--:--` rule cheap rather than a convention to remember:
  the **OS flag** in its seconds register says the oscillator stopped or never
  started, and `Pcf85063::Read` reports that as `valid = false` instead of
  handing back a number. A read that succeeds and a time that can be trusted
  are separate answers. Two details that follow from the datasheet and are
  worth not rediscovering: the seven counters are read and written in **one**
  burst (a read freezes them, so a burst cannot catch a carry — two accesses
  can, and would mix minutes from one moment with hours from the next), and a
  write stops the clock around itself for the same reason. Writing seconds is
  also what clears OS, so a successful `date set` is what makes the clock
  trustworthy again.
- **This is where the repo finally has to know about timezones.** §9.1 avoided
  them by printing countdowns; a clock cannot.

  **The clock is UTC, and a zone is presentation.** The RTC holds UTC, `time_t`
  is UTC, and every conversion happens at the edge where a time is shown or
  typed. That invariant is what makes changing zones free: nothing stored
  moves, so `config set tz Asia/Tokyo` changes one line of `date` and no data
  at all. It is also what `board::AdoptClock` had wrong at first — it read the
  RTC with `mktime`, which treats the counters as *local*, and was therefore
  correct only while the zone was UTC. It uses `timegm` now.

  **Named zones, from a table compiled into the firmware.** libc understands
  POSIX `TZ` strings and nothing else, and ESP-IDF ships no IANA database (v6
  checked, not assumed) — so `components/timezone` maps `Europe/Kyiv` to
  `EET-2EEST,M3.5.0/3,M10.5.0/4`. The EU's three zone families are in there
  under their own names as well (`WET`, `CET`, `EET`), because "I am on Eastern
  European Time" is how some operators know where they are; they sit first in
  the table, which is also what makes the reverse lookup name a shared rule
  after its family instead of after whichever city was listed first. And
  `config.json` keeps **both**: `time.zone`
  is what a person reads, `time.posix` is what libc is given. That pair is the
  house firmware's shape (§10.14.4, its `TimeUtil`), and it is what lets a zone
  whose transitions moved be corrected on the device — write `posix` — without
  waiting for a firmware whose table knows the new dates. A raw POSIX rule is
  accepted anywhere a name is, and is stored as `Custom`; what is refused is a
  string that is neither, because libc reads a misspelled zone as UTC and says
  nothing.

  **That last promise was not being kept, and the host tests are what
  noticed.** `LooksLikePosix` rejected any string containing a `/`, on the
  reasoning that a zone name has one and a rule does not — which is wrong
  about most of this table, because a transition *time* is written `M3.5.0/3`.
  So `EET-2EEST,M3.5.0/3,M10.5.0/4` was refused as "not a POSIX rule", and the
  escape hatch above did not exist for exactly the zones most likely to need
  it. The test that found it asked the question nobody had: do the table's own
  rules pass the check the console gates on? The real distinction is where the
  slash sits — in a rule every one of them follows a digit, in a name they
  separate letters.

  Two costs, stated rather than discovered later: the table is **curated**, so
  a missing zone means typing a rule; and transition rules **change**, so a
  country that moves its dates needs the table edited and the firmware
  reflashed. Neither is fixable without shipping tzdata, which is hundreds of
  kilobytes for a device with one clock face. Not a lookup by IP either.

  The console side: `date` prints RTC and system in UTC and the same instant
  local; `date set <date> <time>` reads **local** time (what is on the wall)
  and stores UTC; `date set utc …` is the escape hatch for when the zone is
  wrong and the clock still has to be right; `config zones [filter]` lists what
  the table knows.
- **A clock that cannot approve says so on the clock.** Unregistered, or the
  boot self-test (§10.6) failed, or no bus — it is on this screen, in words, not
  buried in settings. The device's whole value is that a glance at the desk tells
  you the loop is alive.

##### SNTP — when the clock asks, and when it does not

The RTC is right across a power cut and cannot be *accurate*: it is a watch
crystal, it drifts, and nothing on this board can say by how much. So when
there is a network the device asks a server, sets the system clock and writes
the answer back to the chip — which is what makes the *next* boot start from a
good number rather than from a drifted one.

**Three moments, and they are one rule.** At boot, whenever the internet comes
back, and every `time.syncHours` after that. There is no separate boot case in
the code because there does not need to be: a device that has never synced is
*due*, and "due plus an internet" is the whole trigger. `components/timesync`
is the same split as §10.9's — `sync_policy.h` includes `<cstdint>` and nothing
else and holds every decision (host-tested, §10.11); `timesync.cpp` resolves a
name and waits on a packet, and has none.

- **It reads `wifimgr` rather than asking the network itself** — the seam
  §10.9 said this would hang off. What it needs is narrower than that section's
  three states, though, and the mapping is the interesting part: a client link
  with an address and an internet state that is **not `kOffline`** is enough to
  try. `kUnknown` counts as yes, deliberately — the check being switched off is
  not a statement that there is no internet, and an operator who turned the
  ping off would otherwise have quietly lost the clock as well. An SNTP
  exchange is its own reachability test.
- **A link that flaps is not four reasons to sync.** Coming back after a week
  off the air is; coming back four times in a minute is not, and the guard is a
  minimum gap between *successful* syncs (five minutes) rather than a debounce
  on the link. Inside it, a reconnection changes nothing at all — not even the
  schedule.
- **A failure retries far sooner than the interval, growing and capped** — six
  hours is the gap between good answers, not a penalty for a server that was
  busy. A minute, doubling, to fifteen. Without the cap a device with no route
  to any NTP server would ask sixty times an hour forever; without the growth
  it would do the same at a constant rate.
- **The exchange is bounded** (§10.5's rule about somebody else's wait): fifteen
  seconds for a DNS lookup and an answer, and the client is deinitialised on
  every path — it keeps a socket and a semaphore, and refuses a second `init`
  over a live one, so leaking it once would mean never syncing again.
- **An answer outside 2024..2099 is refused and counted as a failure.**
  §10.8.2's rule about an obviously unset clock beating a plausible wrong one,
  applied to an answer rather than to a chip — and 2099 is the RTC's own limit,
  since it stores two digits and no century.
- **No server named is off, and so is `syncHours: 0`.** Two spellings of "there
  is nothing to ask", not two switches that can disagree — and the compiled-in
  default for the server is **empty**, alone among the string fields, because
  it names somebody else's machine. A device syncing against a host the
  operator never wrote down is the mistake `internet.targets` already refuses;
  a device retrying a server that is not there is an error line every interval
  for the life of the board. The shipped `config.init.json` names
  `pool.ntp.org`, so a device that can read its filesystem does sync.
- **Nothing about this is persisted, and that is a decision.** "When did it
  last sync" lives in RAM: writing a timestamp to `config.json` every six hours
  would be flash wear for a fact the next boot re-establishes in seconds
  anyway, since a fresh device is due the moment it has an internet. What
  *does* survive a reboot is the corrected time itself, in the RTC — which is
  the point of writing it there.
- **`date` says where the time came from**, which a clock face cannot: the
  server, when it last worked, how far it moved the clock, and when the next
  one is due. `date sync` asks now and waits for the answer.

  Two details of that readout, both of which started out wrong. The last sync
  is printed **whether or not syncing is still on** — it is a fact about the
  time on screen rather than about the schedule, and switching the schedule off
  is exactly when it becomes the only thing that says where that time came
  from. And it is printed **in the configured zone**, like the `local` line
  above it: the device keeps UTC and a zone is presentation, and "when did it
  last sync" is a question people ask in wall-clock time.

**And the step is the number worth having, which is why getting it wrong
mattered.** A device corrected by several seconds every time has an RTC to be
suspicious of, and nothing else on this board would ever say so. The first
version computed it as `after - before` around the exchange — and `time()` runs
normally during the seconds spent resolving a name and waiting for a packet, so
it reported the correction *plus the duration*. The board said so immediately:
**`+5 s` twenty-six seconds after a sync that had already set the clock**,
which is a drift rate no crystal has. `esp_timer_get_time()` is monotonic and
no `settimeofday` touches it, so subtracting the elapsed time leaves the
correction alone; the same two syncs then read `+0 s`. Rounded rather than
truncated, or a 4.6-second exchange counted as 4 leaves 0.6 s of itself in the
answer — which is how a device with a perfect clock came to report `+1 s`.

  A **boot** sync showing `+1` or `+2` is expected and not drift: the RTC holds
  whole seconds, so adopting it at boot loses the fraction and always lands
  *behind* the true time. Systematically positive, and about a second of it is
  arithmetic rather than the crystal.

##### What is written, and the five decisions inside it

The split is §10.9's again, and this is the third component pair to take it —
which is what makes the first screen in this firmware mostly a host-tested file:

| File | What it is |
|---|---|
| `components/ui/clock_face.h/.cpp` | **`ui::ClockFace`** — every decision on this screen, and `<cstdint>` and nothing else. The sixth subject in this firmware to manage that, after the navigator, the Wi-Fi policy, the internet check, the sync schedule and the bus link |
| `components/screens/clock_screen.h/.cpp` | the pixels: two custom-drawn objects, two labels, and a palette. No decisions |
| `components/screens/screens.h/.cpp` | the task that reads the world at 10 Hz, runs the face, and applies the answer under a bounded LVGL lock. The counterpart of `wifimgr::Init` and `nats::Init`, and where the other four screens will join |

**What the board has actually done**: come up with the panel at the brightness
`config.json` asked for, shown `14:12` in green seven-segment digits with the
water flowing through them, moved from `-21,-33` to `-18,-30` over three and a
half seconds, reported the radio off as hollow bars, the bus as red — there is a
`nats.url` and no network — and a full battery on a cable, in 1,128 bytes of a
4 KB stack with **no** frame given up waiting for the display, and a heap that
drifted 150 bytes in half a minute.

Five things are decisions rather than plumbing:

- **The digits are drawn as seven segments, not typed.** LVGL's built-in fonts
  stop at Montserrat 48 and a clock on a 480×480 panel wants three times that;
  the alternatives were generating a font — tens of kilobytes of flash and
  `lv_font_conv`, a new host-side tool under root §1 — or shapes. Shapes won on
  the dependency ledger and then paid a second time: §10.8.2's `--:--` is not a
  missing glyph in this scheme, it is the middle segment on its own, and an
  **unlit segment is not drawn at all** rather than shown as a ghost, because a
  ghost is a permanently lit pixel on a panel that charges for those.
- **The water is two interfering waves, and the interesting part is that it has
  a floor.** One travelling wave is a wipe; two at different wavelengths going
  opposite ways is water. The ramp it drives never reaches either end of the
  scale — the top because a pure `0x00FF00` is what wears an AMOLED subpixel out
  and is harsher to look at across a dark room, and the bottom because a trough
  at zero breaks the digits into floating bands the eye reads as a fault. The
  first version had no floor and looked exactly like that.
- **The drift is a walk the model owns, not a modulo of the clock.** `now_ms %
  period` would be one line shorter and would jump by most of the box once every
  ~49 days when the millisecond counter wrapped, on a device whose whole job is
  to sit on a desk being looked at. So the cycle is accumulated from elapsed
  time, every period divides it exactly — `static_assert`ed — and the two axes
  are at 3:5 so the path is a lattice of diagonals rather than a line it
  retraces. The cost is stated: it **repeats** every sixteen minutes, which is
  the trade taken for having no discontinuity at all.
- **Each indicator has a state that is not a fault, and that is the whole design
  of the three of them.** No `nats.url` is a hollow ring rather than a red one;
  no battery is a shell with a bolt in it and the word `USB` rather than an empty
  gauge; a radio switched off is hollow bars rather than a warning. §10.9 made
  the same call about `unknown` being the honest answer, and a screen that spells
  "nothing was asked of me" the same way as "I am broken" is a screen nobody
  reads twice. The bus dot's green-with-a-hole — something arrived inside two
  minutes — uses §10.8.3's staleness threshold rather than a new one.
- **The load is split so that the water costs what the water costs.** The digits
  are one draw target and the indicators are another, so a shimmer at 10 Hz
  repaints 318×146 and not the battery; the PMIC is read once every two seconds
  and **from the task, never from an LVGL callback** (§10.8.1), with a failed
  lease leaving the last reading standing rather than blinking the icon out; and
  the whole face is one object, so the drift is one `lv_obj_set_pos` and nothing
  below it knows the panel is an AMOLED.

What is **not** here, and this paragraph has outlived its first reason. It used
to say the navigator (`ui/navigator.h`) was wired to nothing, because four of the
five screens it can name did not exist and a swipe reaching a blank screen is
worse than a swipe that does nothing. Both halves are spent: the navigator is
wired (`screens.cpp`, one `Apply(ui::Nav)` that a gesture, a button and the
console all reach) and all seven screens are on the glass.

**And nothing on this screen is owed any more.** This paragraph used to end with
two items of §10.8.2's own list outstanding — `key_id` and a gear — held back first
by there being no registration to name and nothing to open, and then by neither
excuse surviving. The list itself is what changed: both are struck at the
repository owner's decision, for the reasons the opening of this section now gives,
so the screen is complete rather than three quarters built.

#### 10.8.3 Limits — the `status` document, when there is one

A port of `approver-web`'s "model and limits plaque" onto the panel: the model
name, `effort.level`, the `5h` and `7d` gauges with countdowns, `ctx`, and the
`cwd` the session is in — and under all of it, one line of what that session is
*doing* (§9.10, at the end of this section).

**It arrives rather than being navigated to**, which is the one rule below that
did not survive contact with the repository owner, and the change is the whole
character of the screen. The table above leads with that now; the swipe it used
to lead with still works, because the navigator still honours it, and nothing
depends on it either way. What actually happens:

| | |
|---|---|
| a `status` document lands | the screen comes up — unless a request card is up, which outranks it (§10.8.1), or the operator is in settings, which arriving numbers must not take them out of |
| a minute with no document | back to the clock |
| `PWR` | back to the clock now |
| **reached by hand while the stream is already quiet** — the swipe below, or `screen limits` on the console | it stays a minute, then back to the clock. And the numbers carry `stopped` next to their age for as long as it is up |

Why it is better than the swipe on this device: §9.7 publishes on **every render**
of the status line, so documents arrive every few seconds while Claude Code is
working and stop dead when it is idle. The minute is what turns that into "the
screen follows the work" — a desk object that shows what the session is spending
while there is a session, and a clock the rest of the time. On a device where
three of five screens do not exist, it is also the only way anybody was going to
reach this one.

**And one place the instruction could not be taken literally.** "Back on `PWR`
until the next document" would be undone within seconds, because the next document
is seconds away. So a dismissal lasts until the stream goes **quiet**: the screen
stays away until the minute expires and something arrives after that. `PWR`
therefore means "not for this burst", which is the only reading in which the
button does anything at all. `ui/limits_view.h` carries that argument next to the
code.

**And one case this section had no rule for, found on a board left overnight.**
Every rule above is about a *burst*, and the cue that ends one fires exactly once
— deliberately, because a second one would be a second navigation. Which left the
screen nobody arrived at: the carousel still works, so a swipe reaches the limits
when the stream has been quiet for hours, and there was then nothing left to take
the screen away. The device was found in the morning parked on the previous
evening's numbers with `4000 s ago` under them, drawn exactly as they are drawn two
seconds after they land. Two things were wrong and both are fixed:

- **the visit gets a minute of its own.** Not less than an arrival gets — somebody
  who swiped here did it to read the last numbers — and not a second timer over a
  live one, because a screen the documents are holding up must not be cut short at
  a minute while a session is still spending. `screen limits` on the console arms
  the same minute, which is also how this state became reachable from a script at
  all: the one bug in this firmware that a person found before a test did was also
  the one no test could set up.
- **the age says the stream has stopped**, in that word, and reads in units a
  person reads — `66 m ago, stopped` rather than `4000 s ago`. The bars cannot
  carry that caveat, since a greyed `2 %` is a different number rather than an
  older one, so the line that already carries the age carries it too.

- **It is a pair of subscriptions on the connection that is already open, neither
  is ever answered, and the request path must not read either.** `status` for the
  numbers, `activity` for the line under them (§9.10) — same test `approver-web`
  states: deleting this screen must leave a working responder. No queue group on
  either (§10.5) — a broadcast current value is meant to reach everyone.
- **"Connected" means a document arrived recently, and nothing more.** §9.7
  publishes a current value with no stream: an idle session simply stops
  publishing and there is nothing to catch up on. So the screen shows the
  document's **age**, marks it stale instead of dropping the numbers, and says "no
  session" only when it has never had one or the last is long dead. A stale
  percentage is still the best available answer as long as it does not claim to
  be current.

  **The threshold is one minute here, and not `approver-web`'s two.** This
  sentence used to say "keep them equal", and the overnight bug above is what
  changed it: the minute that concludes the stream has stopped and takes the
  screen away cannot also be a minute in which the numbers are still presented as
  current. Two thresholds would leave a window — 60 to 120 seconds — in which this
  firmware has decided the session is gone and the glass has not said so. The page
  and the device still answer the same question the same way; what differs is that
  on the device that number is also a navigation rule.
- **The traffic light is §9.2's, not a new one.** A rate-limit window is green to
  50 % spent and yellow to 80 %; the context window green to 20 % and yellow to
  45 %. Three implementations of these two scales now exist (`render.rs`,
  `statusline.ts`, this) — each pins them in a test, and they must not drift into
  each other.
- **The countdown depends on whose clock is trustworthy.** The document carries
  both `resets_at` and the publisher's own resolution (`resets_in`,
  `resets_in_text`) precisely for a subscriber that cannot trust its clock. So:
  before SNTP has synced, display the published text and age it by the time since
  arrival; after, recompute from `resets_at` like the plaque does. `countdown()`
  is a port of `render.rs::countdown` and is tested against the same cases —
  `1h59m`, `<1m`, `now`.
- **One subject, every session.** Every Claude Code session on the machine
  publishes to `status`, so this is whichever rendered last, not necessarily the
  session whose request is on the card. Hence the `cwd` line — without it the
  screen reads as belonging to the request.
- Junk is dropped and the last good document stays: a payload that is not JSON,
  or is missing `ts`/`line`, or carries a percentage that is not a number → one
  log line, previous document kept. Percentages are clamped 0–100 a second time
  here, because a bar drawn from someone else's `130` overflows its track.

##### What is written, and the four decisions inside it

The split is the same one for the fourth time, and by now it is the shape of this
firmware rather than a choice made per screen:

| File | What it is |
|---|---|
| `protocol/status.h/.cpp` | §9.7's document into fields. cJSON only, host-tested |
| `ui/limits_view.h/.cpp` | **`ui::LimitsView`** — the two scales, the countdown, when the screen comes and goes. `<cstdint>` and the navigator, so all of it is host-tested |
| `screens/limits_screen.h/.cpp` | three bars, a model and an age. No decisions |
| `components/watcher` | the subscription. A component of its own, and the next paragraph is why |

**What the board has actually done**, and it is the whole of this screen's
behaviour rather than a sample of it: come up on its own when this repository's
own status line published, shown `Opus 5 (1M context) · high` over three gauges —
61 % and 76 % in yellow, 72 % of the context window in red, which is §9.2's two
scales disagreeing exactly as they should — and gone back to the clock 73 seconds
after the last document, keeping the numbers for `limits` to print.

Then the button, in the three steps that are the only way to tell this rule from
the literal one: `PWR` put it back on the clock and the readout said `dismissed
until the stream goes quiet`; ten further documents arrived and it stayed on the
clock; eighty seconds of silence cleared the flag — the note disappeared and the
readout said `the stream has stopped` — and the very next document brought the
screen back. A dismissal that lasted one message would have failed the second
step, and one that never expired would have failed the third.

And one payload worth having photographed: a document with `five_hour` and no
`seven_day`, which is what §9.7 publishes for an API key rather than a
subscription. `7d` draws as `--` over an empty track with **no fill at all**,
distinguishable at a glance from a window at 0 % — which is the difference that
paragraph is about. The `5h` countdown read `now` in the same frame, because that
reset was already in the past: `render.rs`'s own rule, reached by a device that
had never heard of it.

Four things are decisions rather than plumbing:

- **The subscription is its own component, because §10.8.3 states a test rather
  than a preference**: deleting the limits screen must leave a working responder.
  `components/watcher` and `components/responder` cannot name each other —
  neither is on the other's `REQUIRES` line — so that is something you can
  perform. The one thread between them is a scheduling favour: `watcher` has no
  task, because it has nothing to do between deliveries, and the responder's tick
  calls `Maintain()` so a reconnect is noticed. A second task ticking twice a
  second to watch a boolean would be 3 KB of stack for nothing.
- **It subscribes with no queue group, and as soon as there is a socket.** No
  group because a broadcast current value is meant to reach every subscriber and
  joining one would take it from `approver-web` (§10.5). No key and no
  registration because *reading* needs neither — a device that cannot approve
  anything still shows what the session is spending.
- **Junk keeps the last good document**, which is the opposite of the approval
  path and the reason the parser assembles into a local and copies out at the end.
  There is something worth keeping here; a bad `approvals.*` message has nothing
  behind it. For the same reason a field that is too long is **truncated** rather
  than refused — the one place in this firmware where that is the right way round,
  and §10.8.4 is the contrast: a shortened command is one somebody approves by
  reflex, a shortened model name is a readout that is slightly less specific.
- **The countdown comes from whichever clock is trustworthy.** After SNTP it is
  computed from `resets_at`; before, it is what the publisher resolved, aged by
  the time since it arrived — which is why `resets_in` travels on the wire at all
  (§9.7). A device whose clock is wrong by hours would otherwise print a countdown
  wrong by hours with nothing to say it was.

And the number this screen exists to be honest about: **its age**, in seconds,
under everything else. §9.7 is a current value with no stream behind it, so these
numbers are as true as they are recent and nothing else here would say so.

**One later change, and it came with a measurement worth keeping.** The gauge
labels and the countdowns shipped at 14 point, which is a size for something you
lean in to read — wrong for an object looked at from across a room, and the
repository owner said so. They are 28 now, and the countdowns are right-aligned
against the bar's own margin rather than placed, because that field's width
changes with its value: `now` is three characters and `23h59m` is six, and at 28
point that is sixty pixels of drift.

The percentage went to 48 with them and came back. `sdkconfig.defaults` *enables*
Montserrat 48 for the clock, but §10.8.2's digits are seven drawn segments and
never reference the font — so the linker had been dropping it, and the first line
to name it cost **97,280 bytes of flash**, measured either side. §10.8.2 refused a
generated font on exactly that ground ("tens of kilobytes of flash"), and paying
nearly a hundred for one size step would be that decision reversed for a smaller
reason. So the hierarchy is carried by colour and by the bar instead — the
percentage is the bright thing in its row, the label and the countdown are faint,
and the coloured length underneath is what the eye reads the magnitude from. The
app came back to within 32 bytes of where it started.

The general fact, which is not obvious and will catch somebody else: **a font
being enabled in `sdkconfig` costs nothing until something references it.** Three
are enabled here and two were being linked.

##### The activity line — what the session is *doing* (§9.10)

One line under the bars, at the size the numbers are: `Bash - py -m pytest -q`,
`Explore > Grep - TODO`, `thinking` or `idle`. It is the `activity` document — published by
the same status-line binary from Claude Code's `PreToolUse` / `PostToolUse` /
`Stop` hooks — and on this device it is the fastest-changing thing on the glass,
which is why it is 28 point and not a footnote at 14.

| Claude Code event | `event` | `state` | On the line |
|---|---|---|---|
| `PreToolUse` | `pre_tool` | `running` | bright: the tool about to run, and its one-line summary |
| `PostToolUse` | `post_tool` | `thinking` | bright: `thinking` — the tool that just ran is **not** named (below); the turn is not over |
| `Stop` | `stop` | `idle` | faint: `idle`, the session is waiting for a human |

**It is a passenger on this screen, not a screen of its own**, and that is the
decision worth arguing rather than the pixels. §9.7's documents and §9.10's come
from the same binary and the same session — one on every render, one on every tool
call — and this screen already arrives when the numbers do and leaves a minute
after they stop. That window is exactly the window in which "what is it doing" is
worth reading. So `ui::ActivityView` holds a document and answers questions about
it, and owns **no arrival rule, no quiet timer and no dismissal**: there is one
screen with one of each, and a second publisher racing to raise and drop it would
make both of them untestable.

The cost is stated rather than hidden: with `activity: false` in
`statusline-config.json` (§9.9), or `subject` renamed so no numbers arrive, there
would be no screen and therefore no line however many activity documents landed.
The alternative — this raising a screen of its own — is a bigger change to the
navigation of §10.8 than the line is worth today.

Six things that are decisions rather than plumbing:

- **Only a *running* tool is named**, and this is the one to argue with. A
  `post_tool` document carries the tool that just returned, and drawing it costs
  nothing — but `Edit - main.cpp` for a turn that finished editing a minute ago is
  the same line as for one editing right now, and a readout that cannot be told
  from a true one is the thing this screen must not be. It is the clip bug below in
  a different spelling. So `running` names the tool and its summary, and
  `thinking` and `idle` are their own word: the document's tool is read, kept, and
  not drawn. Eight characters of the twenty-eight this line has are cheap for the
  one thing a glance at a desk object is for — is it working, or is it my turn.
  Colour then says the same thing a second time rather than instead of the text:
  bright while there is a turn, faint once it has ended, and **nothing red** — a
  busy session is not a problem, and red on this screen belongs to a gauge.
- **The line scrolls when it does not fit, and that replaced a clip.** The first
  version used `LV_LABEL_LONG_MODE_CLIP` and the panel read
  `PowerShell - cd E:\projects\ai-` — a command that looks like it ended there. A
  readout that cannot be told from a shorter true one is the one thing this screen
  must not be. `LV_LABEL_LONG_MODE_DOTS` is not the fix either, and the reason is
  LVGL's own: it writes its ellipsis *into the text buffer*, so
  `lv_label_set_dots` returns immediately for a static string (`lv_label.c`:
  `if(label->static_txt != 0) return;`) — the mode would be set, do nothing, and
  leave a line drawing past its own width. Every string on this screen is static
  (§10.14.1). So it scrolls, only when it has to, and on an AMOLED that is the
  cheap direction to be wrong in — §10.8.2 moves the clock's digits around the
  panel on purpose.
- **Ten minutes of staleness, where the numbers get one.** §9.7 publishes on every
  render, so silence there means something stopped; §9.10 publishes on tool calls,
  so a session thinking hard — or parked on a request this device is showing on a
  card — legitimately says nothing for minutes. Ten is where "running Bash" stops
  being believable. And an `idle` document **never** goes stale: it stays true
  until something else happens, and fading it would suggest the session vanished
  when it is simply done.
- **`v` is what makes a document ours.** §9.7's document is recognisable because it
  always carries `ts` *and* `line`; this one has no such pair — every field but
  three may be absent — so `v == 1` is both the version check and the "this is
  ours" test on a subject as open as every other. A `v: 2` from a newer publisher
  is refused rather than half-understood, and so is an `event` or a `state` this
  firmware has no word for: everything downstream then takes an enum, and no
  screen has to decide what to draw for a word it has never seen.
- **The truncation backs off to a UTF-8 boundary**, which `ParseStatus` next door
  does not need to. §9.10 cuts the summary to 80 *characters* and a character is up
  to four bytes, so a path with Cyrillic in it arrives longer than the 128-byte
  field — and a byte-counted cut through the middle of a sequence draws as a
  placeholder box next to a perfectly good line of text. Cheaper to end one
  character early. (The panel's font is Montserrat's ASCII subset, so such a path
  is boxes either way; this is about not adding one of our own.)
- **48 bytes for the tool name is not generosity.** `Bash` is four characters and
  an MCP tool is called `mcp__claude_ai_Atlassian__searchConfluenceUsingCql`; a
  readout that shows half of that says less than one that shows all of it.

`session_id`, `cwd` and `tool_use_id` are deliberately **not** kept: this screen
already carries the session's directory from §9.7's document, and the id exists to
pair a `post_tool` with its `pre_tool`, which is a subscriber's job and not a
readout's.

**What it cost in layout, since the panel was already full.** The gauge stride went
from 104 pixels to 98 — three rows now end at 396 instead of 408, and nothing
inside a row moved — and the two 14-point lines at the bottom became one
(`3 s ago  -  E:\projects\ai-remote`). Both facts they carried are kept, which was
the condition: the directory because without it the screen reads as belonging to
whatever request is on the card, and the age because §9.7 is a current value with
no stream behind it. That freed the 44-pixel band the line sits in.

**The age comes first on that line, and the order was chosen off a photograph
rather than designed.** With the directory in front, a stale line came out as
`…approver-esp32  -  70 s ago, the stream ha` — the caveat is the longest part of
the line, the panel ends before it does, and so the one thing the line exists to
say was the one thing cut off. It is §10.8.5's finding about the status page's
value column arriving on another row, and the general form of it is that **a
character budget is only true for the characters it was counted with.** So the two
facts go in the order they are worth reading, the clause was shortened to
`, stopped`, and the label is bounded and scrolls — like the activity line above
it, and for the reason that line gives: a clipped line ends at the panel edge with
nothing to say it was clipped.

**And then the band was measured rather than eyeballed, which is how the first
version of it turned out to be wrong.** The three gaps under the `ctx` bar were 8,
16 and 14 pixels: the activity line pressed against the bar above it while the two
rows below had air, and on a screen where `kLimitsPad` is 28 everywhere else that
reads as a slip rather than as a hierarchy. The fix is **up, not down** — the gauge
block starts at 106 instead of 116, because below the session line the panel ends —
and the band is now 16 / 16 / 16 by arithmetic, asserted at compile time in
`limits_screen.cpp` so that moving one constant fails the build rather than the
photograph. The block above it keeps 26 pixels between a bar and the next row's
text, deliberately wider than the 16 below: three rows of one kind read as a group,
and the band is what separates the group from the text under it.

One number in the capture does not match the arithmetic, and it is worth knowing
before somebody re-measures: the *ink* gaps are 20 / 16 / 16, not 16 / 16 / 16.
LVGL positions the 30-pixel line box, and `thinking` has no glyph reaching the top
of it — four pixels of that box are leading. Compensating for it would mean moving
the label two pixels up and making the invariant depend on which letters are in the
string, which is the wrong thing to hold constant. The boxes are equal; the string
is whatever the session is doing.

**Read back off the board**, with this repository's own session publishing: the
console's `limits` grew a `doing` block and a second counter pair, the subscription
list shows `activity` as `sid 2` next to `status` as `sid 1`, and the panel showed
`PowerShell - & 'C:\Espressif\tools\python\...'` — the very command that was asking
it — scrolling, with the session line and the age underneath. The `idle` half is
the one thing not photographed: every capture is itself a tool call, so a `stop`
document is always overwritten by the `pre_tool` of the command taking the picture.
Its decision is host-tested and its state is on the console.


#### 10.8.4 Request — the screen the device exists for

`tool_name`, `cwd`, the `tool_input` as the heaviest thing on the panel, a
countdown, and two buttons.

**The decision-surface rules from [`approver-web`](../approver-web/CLAUDE.md)
"Look and feel" constraint 1 carry over unchanged, and they matter more here**,
because this is a gadget and gadgets invite reflex taps:

- Allow and Deny are the **same size and weight**, told apart by label and
  position as well as by colour; neither is the quiet one you dismiss.
- The `tool_input` outweighs both buttons. If the command does not fit it
  **scrolls** — it is never truncated into something that reads as harmless, and
  a card whose command has not been fully seen is exactly the card people approve
  by reflex.
- A prettier screen that makes Allow easier to hit is a worse screen.

- **The countdown is the hook's, not the device's.** It must be ≥ the `timeout`
  in `handler-config.json`, and reaching zero means the card disappears with **no
  reply** — the §7 fail-safe, not a deny.
- **More than one request can be pending.** `approver-web` lists them; a 480×480
  panel shows one card at a time with `+2 waiting`, oldest first. Answering one
  brings the next up instantly — which is precisely when the 300 ms guard of
  §10.8.1 earns its place.
- **Then it says what it sent.** `behavior` plus a short fingerprint of the
  signature, for a beat, before returning to whatever was underneath — neither
  the press nor the signature tells the operator what actually left the device.
  `responder_yubikey.print_decision` (§8.7) exists for exactly this reason.

##### What is written, and the six decisions inside it

The split is the same one for the third time — `ui::RequestCard` decides and
`screens::RequestScreen` paints — and here it earns the most, because every rule
on this screen is a rule about not approving something by accident:

| File | What it is |
|---|---|
| `components/ui/request_card.h/.cpp` | **`ui::RequestCard`** — §7's fields in a bounded queue, the press guard, the countdown, the receipt. Includes `<cstdint>`, `<cstddef>` and the navigator, and nothing else, so all of it is host-tested (§10.11) |
| `components/screens/request_screen.h/.cpp` | the overlay: two plates, a scrollable command, a countdown. No decisions |

**What the board has actually done**: shown a `Bash` card with the command as the
heaviest thing on the glass and a countdown running down from the request's own
TTL, taken two more behind it and said `+2 waiting`, aged the queued ones out
while the first was up, expired the first and shown `TIMED OUT / Bash / nobody
answered - nothing was sent`, then let the clock back. Screenshots of each, taken
with §10.12.2, are what checked the layout.

Six things are decisions rather than plumbing:

- **The verdict is two physical buttons, and nothing on the screen is
  touchable.** `BOOT` allows, `PWR` denies. That is stricter than §10.8.4 asks
  for — it specifies on-screen buttons — and it is stricter in the useful
  direction: a stray finger on a 480×480 panel cannot approve anything at all.
  The plates are labels naming the buttons, built by **one function for both** so
  that "the same size and weight" survives somebody editing one of them. The one
  thing touch still does is drag a long command into view, which decides nothing.
- **`PWR` doubles as the way back to the clock, and that is a risk taken with
  eyes open.** It is the button people press to *get out* of a screen, so muscle
  memory will occasionally deny a request somebody meant to read. It is the safe
  direction to be wrong in (§10.10: never a silent allow) — a denied request is
  one the operator can ask for again, and an allowed one is not. Two consequences
  are written into the code: while a card is up that button is **a verdict and
  nothing else** — a press the guard throws away must not fall through to
  navigating, or the guard becomes a way to leave the screen instead of a way to
  protect it — and holding it six seconds still powers the board off, which is
  the AXP2101's own behaviour (§10.1) and not something this firmware
  participates in.
- **§10.8.1's queued touch became one comparison.** A press is taken only if it
  *began* at least 300 ms after the card was presented. That single signed
  subtraction refuses both halves of the rule — a finger already down when the
  card arrived gives a negative difference, and a card that has only just
  appeared gives a small one — and it applies again from scratch to the next card
  in the queue, which is exactly the moment §10.8.4 says the guard earns its
  place. The buttons are read as **edges**, so a finger resting on the allow
  button cannot approve a stream of arrivals either.
- **A payload that does not fit is refused, never truncated.** §10.8.4 forbids
  shortening a command into something that reads as harmless, so `kToolInputSize`
  is a limit with a stated cost rather than a cut: 2 KB holds any `Bash` command
  and does **not** hold a large `Write`, and such a request produces no card and
  no reply, which puts the question back in Claude Code's own terminal. The same
  door refuses a request with no reply subject and one with no tool name — a card
  nobody could answer, and a card that asks about nothing.
- **`Tick` can make a card disappear and can never answer one**, which is the
  single most load-bearing assertion in the host suite: two minutes of ticking a
  one-minute card produces an expiry and no verdict. And an expiry reads as a
  **timeout**, in amber, not as a deny in red: the hook has already fallen back
  to its own prompt, and an operator told "denied" would believe they had
  answered something they never saw.
- **There is a bus behind it now, and the shape of how it got there is the
  point.** This section used to say the opposite — that subscribing would take
  real requests away from responders that can sign one — and the condition that
  made it true is now a condition in the code: `components/responder` subscribes
  only when it has a key, a registration **and** a connection, so a device that
  cannot answer is never on the subject. `request test` is still there for when it
  is not.

  What did **not** change is anything in this component. `screens.cpp` gained a
  hook where `Decided()` used to log, and it still has no key, no subject and no
  signature in it — which is what §10.14.2 asks for and what makes "delete the
  responder and the card still works" true. The receipt line moved outside for the
  same reason: only the thing that publishes knows whether anything did, so
  `SetReceiptNote` is set from there and reads `allow sent, sig <8 characters>`
  instead of `decided, not sent - no key yet`.

Two things only the board could have said, both fixed, and both found by putting
a screenshot (§10.12.2) next to `request`:

- **`+N waiting` never appeared.** The queue count was refreshed only when the
  *card* changed, and a second request arriving does not change the card — so a
  queue that grew under one said nothing. It has its own comparison now.
- **A card nobody answered read `decided, not sent`.** The caller's note was
  printed under every outcome, including a timeout, where nothing was decided at
  all. The model had it right and the words did not, which is the failure mode a
  receipt is supposed to prevent.

And one number that came from measuring rather than reasoning: a `ui::Request` is
2.3 KB of §7 fields, and two of them as locals in the button poll took the screen
task's free stack from 2,944 bytes to **1,088**. They are static now — which
§10.14.1 would have asked for anyway; the measurement is what made it urgent.

##### The loop, closed — `components/responder`

`approvals.*` in, a signed decision out, and everything under it already worked
on its own. What this component is, is *when*:

| File | What it is |
|---|---|
| `protocol/approval.h/.cpp` | the JSON either side of the signature: what a request is, and what a reply has to echo. cJSON only, host-tested (§10.11) |
| `responder/responder.h/.cpp` | the subscription, the queue between the press and the signature, and the counters `request` prints |

**What the board has actually done**: taken a request built by `hook.build_request`
off the bus, shown the command whole, and — on a press — signed it and published
into the request's own reply subject, with `tools/test_request.py` reporting
`TRUSTED — Claude Code would allow this` from `hook.verify_reply` against the real
allowlist. And the other end of the same rule: a request nobody answered produced
no reply at all, and the hook fell back to its own prompt (§10.10).

Four things are decisions rather than plumbing:

- **It subscribes only when it could actually answer.** §6's queue group means
  each request reaches exactly one responder, so a device on the subject without
  a key or a registration would take requests away from the YubiKey responder and
  answer them with silence — which is worse than not being there, because the
  operator sees a request that simply never gets decided. Key, registration and
  connection, or it is not subscribed, and `request` names which of the three is
  missing. A reconnect drops the subscription with the client, so "subscribed" is
  tracked against the connection it was made on rather than believed.
- **Nothing is signed on the task that saw the press.** The screen task has 4 KB
  of stack in total and `crypto_sign` wants 4,112 bytes of it — but the real
  reason is §10.8.1's: a screen task that stalls cannot see the next press. So a
  decision is copied into one of two static slots and this component's own task
  does the work. Two slots because a press is a human and signing is ten
  milliseconds; over capacity is a drop, counted, and §10.10's fail-safe.
- **A decision that missed its moment is dropped rather than sent.** If the socket
  went and came back between the press and the publish, the inbox the reply would
  go into no longer exists, and publishing into it is worse than silence — it
  looks like an answer to a question nobody is waiting for. The connection
  generation is recorded at the press and checked at the publish.
- **The verdict reaches this component through a hook, not a call.** `screens`
  knows nothing about keys, subjects or the bus, and the dependency runs one way:
  the responder registers itself as where a verdict goes. Deleting it leaves a
  device that shows cards and lets them time out, which is the test §10.8.3 states
  for the limits screen applied here.

**And one bug the board found that no host test could have.** The counters and
the receipt were written *after* `nats::Flush`, and on the first real exchange the
reply reached the hook — verified, trusted, acted on — while `request` still said
`sent 0` and the glass still said nothing had left, **for minutes**. `Flush` waits
on a mutex the library holds across its own socket reads, so it stalls far longer
than the two seconds it is asked for, and everything after it stalled with it.

The fix is the ordering, and §4 had already drawn the line: published means the
bytes are gone, and for a decision that *is* the delivery — the hook is inside a
request-reply and has the answer the moment the server does. So the counters and
the receipt are true as soon as `Publish` returns, and the flush is confirmation
afterwards, worth a log line if it does not come. A readout that lags the thing it
describes is worse than no readout: it was the reason a working loop looked broken
for half an hour.

#### 10.8.5 Settings, and the status pages behind it

One list, no cleverness — and **the list is the repository owner's rather than
this section's**, which is worth saying plainly because it replaced a longer one
that had been sitting here unbuilt:

| Entry | What it is | State |
|-------|------------|-------|
| Wi-Fi | → §10.8.6 | **built** |
| Status | three pages of what the board is doing, below | **built** |
| Touch test | a crosshair that follows the finger, and a four-cross calibration behind it | **built** |
| Config save | `config save`, with a finger in front of it (§10.15) | **built** — and the row that made the list longer than the panel |
| Config reload | `config reload`, which drops every edit that has not reached the file | **built** |
| *(a few more, later)* | the owner's words: "пока непонятно, потом чтото добавим" | **deliberately not drawn** |
| | the last two are the rows that *do* something rather than opening something, and they are last in that order on purpose: the further down, the harder to undo | |
| Reboot | restarts the device | **built** |
| Power off | switches it off at the PMIC | **built**, and **refused while the cable is in** — see below |

What changed against the previous list, and why each one:

- **the settings that were here are gone from it.** Bus, display, time, key and
  registration, restore, factory reset — all of them are `config set`, `nats
  url`, `date`, `keys` and `config restore` on the console today, and a screen
  for each is a screen with a keyboard problem attached. The list the owner
  asked for is a list of *places*, and the settings screens can be added to it
  one at a time when there is something to type them with;
- **status is new and is the one the owner spelled out**: the Wi-Fi mode right
  now, the battery and its voltage, why the chip is powered at all, why the
  firmware last restarted, the temperatures and the accelerometer — "в общем что
  поместится, может несколько экранов". It is three pages;
- **an entry with nothing behind it used to be drawn** faint, with `soon` on the
  right, and pressing it said so — §10.9's rule that `unknown` is the honest state,
  applied to a menu. The rule stands and the code does not: the last row without a
  screen got one in §10.8.6, and the mechanism that drew it has been deleted rather
  than left carrying nothing (`ui/settings_menu.h`, and `build.md` §10.12 for what
  the deletion weighed). Whoever adds the next unfinished row brings the faint
  drawing back with it — which is a line of work, and the alternative was a
  predicate that answered `true` for every row for the rest of the project;
- **the placeholder rows are not drawn at all**, which is the owner's
  instruction and also the right call: a row that says nothing and does nothing
  is worse than a short list.

##### Getting in, and the one rule that had to change

Three ways, and the third is the reason the second exists:

| | |
|---|---|
| a **swipe up** | §10.8.5's own gesture, unchanged |
| **`KEY` held two seconds** | the free button (§10.1), and the way in when the glass is not being touched. It fires **while the finger is still down** — the operator is holding a button with no feedback but the screen, and a device that waits for the release is a device somebody keeps holding, wondering |
| the console's `screen` | not for the operator: it is what lets a screenshot be taken of a list that is otherwise reached only by a gesture (§10.12.2) |

**And settings is now reachable from the limits screen, which this document
previously forbade.** The old rule was "one way in is one place to look": the way
in belonged to the clock, so settings opened from the clock and from nowhere else,
and `navigator.cpp` refused a swipe up from the limits. That held while the
limits were something the operator *swiped to* — and §10.8.3 then made them a
screen that **arrives**, every few seconds, for as long as a Claude Code session
is spending. So a device left on the desk while the work happens is a device
parked on the one screen with no way into settings, and the gesture that opens
them does nothing.

Found by trying to reach the list on a board that was watching this repository's
own status line. It is the same action reaching the same place, so it is not a
second way in; what changed is that the first one stopped being reachable.

##### The settings file, from the glass — and the list that outgrew the panel

**Two rows, at the repository owner's request**: `config save` and `config
reload`. They are this repository's own console commands with a finger in front of
them, and the reason they are worth a row is §10.15's rule rather than
convenience — *every* setter in this firmware writes to memory and `config save`
is what reaches the filesystem. So until these existed, a Wi-Fi mode picked on the
glass (§10.8.6) and a touch correction made with four presses (§10.8.5) both
needed a USB cable to survive a reboot, on a device whose whole point is that it
does not need one.

Three decisions in them, and only the first is obvious:

- **one press each, and neither arms.** The arming below means exactly one thing —
  *this takes the device away from whoever is looking at it* — and a mechanism
  that means one thing is worth more than a mechanism that means "careful, in
  general". A save is idempotent. A reload does have a cost, and it is **written
  on the row** rather than asked about afterwards: `drops unsaved`, drawn before
  anybody touches it, which is the same call this section already makes about
  `usb in` on the power-off row;
- **what happened is said on the row**, for three seconds — `saved`, `reloaded`,
  `failed`. A save that reached the filesystem and one that did not are the same
  press from the glass otherwise, and the console's `config` readout is not on the
  panel. It belongs to the visit it happened in, so leaving the screen drops it:
  coming back to `saved` on a row nobody has pressed this time is a readout about
  a moment that has gone;
- **and the work happens off the LVGL lock.** `Activated` is reached from the
  button poll *and* from the tap handler, and that one runs inside the display
  lock — while `config::Save` writes a file and `config::Reload` reads one and
  then hands every subsystem holding a copy of a field its new value. So the press
  records what to do and the task loop does it a tick later, which is §10.8.1's
  rule about the LVGL task arriving on a row rather than on a signature.

**And the reload re-applies what a reload has always re-applied, through a hook
rather than a second list.** The console's `config reload` used to be followed, in
`console.cpp`, by the list of everybody holding a copy of a field: the codec's
volume, the Wi-Fi manager's network list, the clock's sync interval, the bus's
URL. That was one caller, so it was fine there. There are three now — this row,
the console, and §10.15's boot restore — so the list moved to
`config::OnChanged`, registered by `main` and called by `Reload` and `Restore`
themselves. Which is the fifth inversion of this kind in this firmware, after
`screens::OnDecision`, `wifimgr::OnTick` and `web::SetDiagnostics`, and it is what
makes a reload from a finger and a reload from a cable the same reload. It also
picked up `web::Apply`, which nothing had ever called: `web.mode` was re-read at
boot and never again.

**The list is now seven rows, and seven do not fit.** At the stride
`settings_screen.h` draws them at, seven rows are 624 pixels of list on a
480-pixel panel — and that header's `static_assert` refused the build rather than
letting anybody discover it on the glass, which is exactly what it did when the
*fifth* row arrived.

**The first answer was a window of five that the selection dragged along with it,
and the owner threw it out in one sentence**: "экран настроек скролится только
кнопками. надо сделать чтоб скрол был жестами". Which is right, and the mistake is
worth naming rather than quietly fixing — a window is what you build when the
scrolling is yours to implement, and on a touchscreen it is not: a list is a thing
people expect to drag. The window worked and it was reachable only from `BOOT`.

So **LVGL scrolls it**. The rows live in a scroll container that fills what is
left under the title, vertical only, with a scrollbar on `AUTO` and LVGL's own
momentum and elastic ends:

| | |
|---|---|
| a finger | drags the list, with momentum — LVGL's, not ours |
| `BOOT` | steps the selection as it always did, and `lv_obj_scroll_to_view` brings it onto the glass. **Only when it moves**: scrolling to the selection on every repaint would fight the finger that had just dragged the list somewhere else |
| a tap | a **row**, and that is the whole simplification — every row is its own widget carrying its own index, so the slot-to-entry mapping the window needed is gone and with it a bug class: a tap on the third slot is not a tap on the third row |
| the title | still says `4 / 7`; `screen` on the console says which five are on the glass, read off the scroll offset |
| coming back in | the top, which is `SettingsMenu::Opened`'s rule for the selection and now the scroll's as well — a highlight on row one under a list still scrolled to the bottom would be a highlight nowhere |

**What it cost is the swipe that used to leave this screen, and something had to
replace it.** LVGL suppresses a gesture while anything is scrolling
(`indev_gesture` returns early on `scroll_obj`), so on this screen a vertical drag
is the list and never reaches the navigator at all — including at the ends of the
list, because a container with content out of view stays the scroll target and
merely shows an elastic bounce. `PWR` and the title were always the other two ways
out, and a list a thumb can drag and cannot leave is a list people get stuck in, so
**a sideways swipe now goes back** — both directions, for the reason the clock's own
carousel gives: a swipe that works one way and not the other reads as a broken
screen. A *horizontal* drag is not consumed by a vertically-scrolling container, so
it still arrives. That is two navigator tests, and the older one that asserted "no
swipe navigates away from settings" was rewritten rather than worked around.

None of this reached `ui::SettingsMenu`, which went back to owning one thing: which
row is selected. Deleting the window took eight tests with it — they were testing a
mechanism, and the two properties worth keeping moved rather than vanished: *the
selected row is on the glass* is `lv_obj_scroll_to_view`'s job now, and *a tap lands
on the row under the finger* is true by construction.

**What the board said, and it is the same sentence for the third time.** The list
came up with seven rows, five on the glass, `1 / 7` in the title, and `config
reload` **sharing pixels with its own note**: `drops unsaved` is thirteen
characters, about 190 px at Montserrat 28, against a note column that starts 180 px
into a 432 px plate — and the label is 200 px of it. §10.8.5 already records this
about the status page's label column and §10.8.6 about the scan row; the answer is
the same one each time, and it is not to widen the box. `edits lost`, measured on
the glass afterwards rather than reasoned about. A layout constant is the one kind
of mistake that looks fine in every test.

The scroll container is on the glass with its scrollbar showing five rows of seven,
photographed, and the console reads the offset back (`showing rows 1-5 of 7`).
**And the drag has been done with a finger**, which is the half no console could
stand in for — `screen` deliberately cannot press a row or draw a gesture (§10.7),
so until somebody dragged it, "it scrolls" was LVGL's reputation rather than this
device's behaviour. A row pressed by finger is confirmed too, and by the shortest
route there is: the touch calibration two sections down was reached by pressing its
row.

**And the `config save` row has been pressed from the glass**, with `saved` on it
for its three seconds afterwards — which was the last thing on this screen that
only a finger could answer. So the whole of this list is confirmed on hardware
now: the drag, the row press, and the row saying what it did.

##### Reboot and power off ask twice, and the console does not

§10.7 argues that the console's `reboot` needs no confirmation: a reboot undoes
itself in seconds, and a second word there would be friction on the most ordinary
debugging action there is. The screen reaches the opposite answer from the same
premise, and the difference is who is asking:

- on a console somebody **typed a word**. On a 480×480 panel a stray finger is an
  ordinary event, and this row is at the bottom of a list people scroll with
  their thumb;
- §10.8.5's older list already made its destructive entries two-step, and this is
  the one that survived from it;
- so the first press **arms** — the row turns amber and says `press again` — and
  the second one goes. The arming **expires on its own** after five seconds,
  because an armed reboot left sitting on the glass is the stray finger with
  extra steps, and moving the selection off the row clears it, or the arming
  would be a delay rather than a confirmation.

**Power off is the same machine and one rule more**, and it is the rule §10.1
already made the console keep: **VBUS is a power-on source for this chip**, so a
shutdown with the cable in is one the hardware immediately undoes — and what the
operator sees is not a device switching off but a device rebooting, which on a
thing that sits on a desk reads as a crash. `Axp2101::PowerOff` refuses and
writes nothing; the row does the honest half of that:

- it says **`usb in`** and draws itself faint, *before* anybody presses it. The
  cable is on the row rather than in a refusal, which is the same call §10.9
  makes about `unknown` being a state and not a fault;
- a press on it while blocked is **refused before it arms**, not after. Arming a
  row that cannot fire would ask the operator to confirm something the hardware
  is going to refuse anyway — and worse, unplugging the cable at that moment
  would leave a device one stray press from switching off;
- the two destructive rows share one arming flag, and that flag belongs to the
  **selected** row: arming the reboot and stepping onto power off leaves the
  second one two presses away, not one. That is a test, because sharing a flag is
  exactly how it would not be.

There is a third way to switch this board off and no code of ours is in it:
holding `PWR` for six seconds is the AXP2101's own behaviour (§10.1). Worth
knowing when this row refuses.

**And the shutdown has now happened**, which took the one session that cannot be
scripted from here: the cable out, so the board on its battery, and therefore no
console to watch it from — two presses on the armed row and it switches off. It
was the last thing on this screen whose *effect* nobody had seen, as against its
refusals, which the console could always check.

All of that is `ui/settings_menu.h`, which includes `<cstdint>` and nothing else
and is where §10.11 can reach it: every way of getting a single press to reach a
restart is a test.

##### The status pages

Three, and each answers a different question rather than a third of one:

| Page | What is on it |
|------|---------------|
| **power** | the battery and its voltage, whether it is charging **and at what current**, VBUS in with its voltage **and what may be drawn through it**, the system rail, ALDO2 and ALDO3 with their states, the PMIC die temperature — and **why the board is awake**, which is the chip's own answer (§10.1) and not something the firmware participates in |
| **system** | why the firmware last restarted (`esp_reset_reason`), uptime, **the free heap and its low-water mark on one line** — §10.14.1's point is that the first only means something next to the second, and they were two rows until the page ran out of them — the firmware version, the Wi-Fi state with its SSID, signal and channel, the address, whether the bus is connected, and **whether the configuration web server is up** (§10.16). That row's first word is now always the fact — `up, port 80`, or `stopped` with which of the three reasons — because it used to lead with `auto` / `on` / `off`, and the repository owner read it and said what it looks like from the desk: there was no state on the row, only a desired state. `auto` is a setting; `stopped` is what is happening |
| **motion** | the three acceleration axes, the **magnitude** — the one line that says the other three mean anything, since at rest it must be 1 g (§10.7) — **the position in words** (`card-slot edge down`, `flat, screen up`), the three gyroscope axes, and the IMU die temperature |

Rules it keeps:

- **pages rather than one scrolling wall**, at the owner's suggestion. A
  scrollable list on this device means a finger dragging over numbers that are
  being repainted underneath it, and a screen whose content moves while it is
  being read is a screen nobody trusts. A page is a whole thought, and `BOOT`
  or a tap on the body steps to the next one;
- **the page travels with the numbers.** The rows are gathered outside the LVGL
  lock — one of them is an I²C read (§10.8.1) — and the page can turn between
  the gathering and the painting. A title taken from the pager at paint time
  would name a page whose numbers are not on the glass yet, which is a readout
  lying for a tenth of a second about the one thing it is for;
- **it reads the IMU and that changes nothing about §10.13.** No gesture ever
  approves anything, and the way that stays true is that nothing on the approval
  path can see it: this is a readout, in the same class as `imu` on the console;
- **the page holds nine rows, and it is full.** `kStatusRows` is nine and the
  system page had exactly nine when §10.16's server needed a tenth — so the two
  heap rows became one (`23364, low 17588`), which is what `status` on the console
  has always printed on one line anyway. The next row to arrive will need a fourth
  page rather than another merge, and that is a `StatusPage` value and a `Fill…`
  function;
- **a label has about eight characters of room**, measured on the glass rather
  than computed — and the column is clipped, so a ninth is a cut-off word rather
  than two words drawn on top of each other. `magnitude` was the ninth, and what
  was on the panel was the label and the number sharing pixels. It is `total`
  now.

**The two currents on the power page, and why they are not measurements.** The
repository owner asked for the current the battery is charging at and the current
the device is drawing, with a fourth page offered if they did not fit. They fit —
the power page had one row spare and the numbers went next to the two rows they
belong to, `charge` and `usb` — but neither is a *reading*, because **the AXP2101
cannot measure a current**: its ADC channel register has five channels (battery,
TS, VBUS, system, die) and no ammeter among them, and there is no sense resistor
on this board either. `pmic/axp2101.h` records where that is established.

So what is shown is what the charger is *configured to allow*, read back off the
registers on the same snapshot the voltages come from — so a chip that lost its
configuration shows that rather than what `Init` believes it wrote — and the
punctuation carries the difference:

| On the glass | What it means |
|---|---|
| `charge  charging, 500 mA` | the chip says it is in its **constant-current** phase, so the limit is about what is flowing into the cell |
| `charge  charging, <500 mA` | charging in some other phase — the taper is under that ceiling and this firmware cannot say by how much |
| `charge  discharging` / `idle` / `no cell` | not charging, so a charge limit would be a number about nothing |
| `usb  in 5.07 V, <2000 mA` | the cable is in, and that is the input limit the PMIC is set to — the other half of why a charge cannot go faster than it is going |

The console's `power` says the same thing in a sentence rather than in
punctuation, and adds the precharge and termination currents, which are a
charger's settings and not something a screen needs.

##### The board that would not come back, and what it actually was

A `power off` from the settings screen was followed by a board that appeared not
to switch on: dark panel, silent console. It is worth writing down in full,
because almost every step of the diagnosis pointed somewhere else and the answer
is a state no code of ours can reach.

What was true:

| Observation | What it ruled out |
|---|---|
| the USB device enumerated — `VID_303A PID_1001`, this board's MAC | the chip had power, so the PMIC had not left its rails off |
| `esptool` talked to it, and **without resetting it first** (`--before no-reset`) | it was sitting in the ROM, not running the app |
| the console answered nothing, at any baud, after any wait | the app was not running, rather than running quietly |
| the `BOOT` button was not held, and the app image was intact | the two ordinary causes of a download boot |

The ROM said it plainly, once there was a way to read it:

```
rst:0x15 (USB_UART_HPSYS), boot:0x15 (DOWNLOAD(USB/UART0/SDIO_FEI_REO))
waiting for download
```

**The chip was latched into download boot**, and every reset available over USB
lands as `USB_UART_HPSYS` — which does not clear the latch. `esptool --after
hard-reset` does not, and `--after watchdog-reset` answers *"Watchdog hard reset
is not supported on ESP32-C6"* and falls back to the one that does not work. Only
a **power-on reset** clears it.

**The way out is the button, and it is the AXP2101's own behaviour**: hold `PWR`
for six seconds — the chip switches the board off — then press it briefly. That
is a genuine power-on reset for the C6, the latch is gone, and the app boots.
Which is also the shortest possible answer to "the device will not turn on": *hold
it, then press it.*

Two things that follow, and neither is the fix that was asked for:

- **reading the port is not free on this chip.** `serial.Serial("COM4")` asserts
  DTR and RTS when it opens, and on the C6's native USB Serial/JTAG those two
  line states are how a host asks for reset and download boot — so the obvious
  way to open the port is a request to enter the ROM downloader. Every script in
  [`working-with-code.md`](working-with-code.md) sets them **before** `open()` for
  this reason, and the one place that did it the other way round is what made a
  latched board look like a permanently latched board;
- **and a hardware reset drops the USB device**, so a boot log cannot be captured
  on a handle opened before it. Catching one means reopening the port in a loop
  until it comes back — which is how the two ROM lines above were finally read,
  after three wrong theories.

**What the power key had to do with it: nothing, and the hardening is right
anyway.** The registers were checked on the board afterwards and held exactly
what §10.1 says they should — 128 ms on, 6 s off, long press enabled — so nothing
had drifted. But the driver was *trusting* that, and the day it is not true is a
day the button stops working with no way to tell why. `Init` writes them now, and
says so when what it found was different.


##### The touch test and the calibration

**The first question is whether a capacitive panel needs calibrating at all, and
the honest answer shaped everything below.** The CST9220 reports in its own
native grid, and that grid is 480×480 — the driver prints it at boot, the same
numbers as the glass. There is no gain to trim the way a resistive screen needs.
What there *is*:

| | |
|---|---|
| **the axes** — `swap_xy`, `mirror_x`, `mirror_y` | compiled in from the vendor's example and not derivable from anything (§10.1). If a revision lays the film down differently they are simply wrong, and **a calibration is the wrong place to fix that**: `board.h` is. The `touch` command prints them next to the correction for exactly this reason |
| **a small offset** | where the film sits over the glass. Real, usually a few pixels, and the thing this actually corrects |
| **anything else** | no. So it is one affine per axis and not a mesh: four numbers, two of which are almost always 1.0 |

A negative scale is allowed, and that is deliberate rather than an accident of
the arithmetic: it undoes a mirrored axis. The plausibility check therefore
bounds the **magnitude** and not the sign.

###### The rule the whole design hangs off

**A calibration must not be able to lock the operator out.** The screen it is
made on is touched, so a correction that lands every press in the wrong place
would take away the way to fix it. Three answers, and all three are needed:

- **a fit that is not believable is refused**, with a reason, and the one in use
  is kept. Each refusal is its own value because each is its own sentence: "the
  presses were all in one place" and "that would push a corner off the glass" are
  different problems, and one `false` would send somebody hunting the wrong one;
- **nothing on that screen is touchable.** Not one widget, not even the way out.
  `BOOT` starts a calibration, `KEY` puts the correction back to none, `PWR`
  leaves — the glass is for showing where the finger is, never for pressing;
- **and `touch reset` on the console** is the second escape hatch, which works
  with the panel unplugged.

The navigator refuses swipes on that screen for the same reason, and there it is
a safety property rather than a preference: this is the screen that tests the
thing a swipe is made of, so a gesture that navigated would take a device with a
bad correction *off* the one screen that can fix it, by accident, while a finger
is being dragged across it.

###### How it runs

`BOOT`, then four crosses — one per corner, inset 64 px — and a press on each.
Then:

- **the point is taken on release, at where the finger last was.** A press is
  what the operator can still adjust; a release is what they meant. Taking it on
  the way down records the first frame of a finger still landing;
- **a press shorter than 80 ms or longer than four seconds is not a point.** The
  panel reports a stray point now and then, and one of them landing in a
  calibration is a correction built out of noise; a long press drifts, and what
  would be recorded is wherever it ended up;
- **four points, least squares, not two subtractions.** Four is the smallest
  number that over-determines two parameters per axis, and that is the whole
  reason for it: a finger that slipped on one cross then costs a few pixels
  everywhere instead of deciding the answer;
- **the fit is applied at once and written by nobody.** `config save` is what
  reaches the filesystem, the way it is for every other setting (§10.15) — so a
  calibration that turns out worse than the one before it is undone by a reboot;
- **`PWR` part-way through changes nothing at all.**

**And it has been run on the glass**: four crosses pressed, the fit applied. Until
that happened this section described a sequence whose deciding half was
host-tested to the mutation and whose *pressing* half nobody had done — which for
a screen whose whole subject is where a finger actually lands is the half that
matters. The console could never stand in for it: `screen touch` opens the screen
and deliberately cannot press a cross (§10.12.2).

###### Two guards, and why they had to be pulled apart

The fit refuses a set whose presses did not spread out, and separately refuses a
fit that would stretch the screen. They started as one number and a mutation
found it: the span guard was written as "the raw span is at least half the
target span", which **is** the statement "the scale is at most two" — so it fired
first every time, and the plausibility check below it was unreachable code that
looked like a safety net.

They are an eighth and a factor of two now, and each has its own job: the first
says the presses were not spread out enough to mean anything, the second says the
line through them is a stretch nobody wants. The lower half of the scale bound is
still unreachable — a scale below 0.5 needs the raw points to span more than
twice the crosses, and they come off a controller whose grid is the panel — and
it is kept anyway, one comparison, so that the check is not a statement about
today's geometry.

###### What is written

| File | What it is |
|---|---|
| `ui/touch_cal.h/.cpp` | **`ui::TouchCalibration`** (the affine), `FitTouch` (the least squares and every refusal) and **`ui::TouchFlow`** (the sequence). `<cstdint>` and nothing else — 22 tests, 16 of 16 mutations caught |
| `display/touch.cpp` | where the correction is applied: `Read` returns screen coordinates, `ReadRaw` returns the controller's |
| `screens/touch_screen.h/.cpp` | the crosshair, the crosses and three lines of readout. Nothing clickable |
| `components/config` | four numbers in `config.json`, clamped on the way in because that file is edited by hand |

**One dependency edge this added, and it points the other way from every other
one in `components/display`**: that component now `REQUIRES ui`. The argument is
on its `CMakeLists.txt` — `ui` has an *empty* `REQUIRES` and is a leaf of pure
arithmetic, so a driver depending on it depends on a formula rather than on the
product. The rule that keeps §10.14.2 true is narrower than the dependency: **no
file in `display` may include a `ui` header that knows what an approval is.**

And one thing the console deliberately cannot do: **calibrate.** Four crosses
need four fingers in four places, and there is no honest way to send that down a
serial port. `screen touch` opens the screen; the presses are the operator's.

##### What is written, and the five decisions inside it

The split is the one every screen here takes, and this is the fifth pair:

| File | What it is |
|---|---|
| `ui/settings_menu.h/.cpp` | **`ui::SettingsMenu`** — the rows, the arming, and whether a power-off could actually happen. `<cstdint>` and nothing else: the cable is a `bool` handed in, because this layer has never heard of a PMIC |
| `ui/status_pages.h` | **`ui::StatusPager`** — three pages and a wrap. Embarrassingly small, and in `ui` for the reason the rest is |
| `screens/settings_screen.h/.cpp` | four plates, a title that is also the way out, and a tap recorded rather than acted on |
| `screens/status_screen.h/.cpp` | a title, a page counter and nine label/value rows |
| `screens/screens.cpp` | where the three inputs meet: a gesture, a button and the console all end at one `Apply(ui::Nav)` |

**What the board has actually done**: the list on the glass — with `soon` on the
two unbuilt rows when it was first photographed, and no such row on it since
§10.8.6 — all three status pages photographed —
`100%, 4.18 V` and `USB plugged in` on one, `usb` and `-36 dBm, ch 1` on the
next, `-1.012 g` on the third with the board stood on its card-slot edge, which
is §10.13's own table read back off the panel — and every input confirmed by
hand: `KEY` held, the swipe, `BOOT` stepping the list and the pages, a tap on a
row, `PWR` back, and the reboot row asking twice.

Five things are decisions rather than plumbing:

- **the three ways in are one function.** A gesture, a button and a console
  command all end at `Apply(ui::Nav)`, so the navigator is moved from exactly one
  place and the card still outranks all three (§10.8.1). The console's route
  cannot press a row, which keeps `reboot` something only a finger can reach —
  §10.7's rule of one route per surface, and the console already has its own;
- **a tap is recorded, never acted on.** The LVGL event callback writes a byte
  and the screen task is what moves anything, because §10.8.1 keeps decisions off
  the LVGL task for the same reason it keeps signatures off it — and one of these
  decisions ends in `esp_restart`. The handoff needs no lock: both sides run
  under the display lock, the callback because `lv_timer_handler` holds it while
  it dispatches;
- **the request card now swallows touch.** It is a full-screen opaque object and
  it was not clickable, so LVGL hit-tested straight through it — which mattered
  for the first time the moment there was a clickable row underneath. §10.8.4
  says nothing on that card is touchable; a finger reaching a settings row behind
  it is the same rule broken from the other side. There is no handler on it: a
  press lands there and stops;
- **the same button means yes and next**, and it cannot mean both at once: with a
  card up `BOOT` is a verdict and navigation is gone, and with no card it steps
  the list. The branch is the card's, not the screen's, which is what keeps the
  §10.8.4 guard the only thing between a press and a verdict;
- **`screens::Navigate` waits for the task to take each move.** There is one slot
  and callers chain — reaching the status pages is "up, then open" — so a second
  call that overwrote the first before the task saw it would silently perform
  only the last one. Found on the board: `screen status` from the limits screen
  did nothing at all, twice, and the readout printed afterwards was the honest
  answer to a question nobody had asked.

**What it costs**: `libui.a` and `libscreens.a` grew by the two screens, and the
number worth watching is the heap rather than the flash — with the radio up, the
bus connected and both new screens built, `status` reports **38,300 free and
29,720 lowest ever**, against 93,632 free on the same board with the radio off
and these screens absent. Most of that difference is Wi-Fi's ~41 KB (§10.9);
about 14 KB of it is these two screens' widgets. The screen task's own stack went
from 2,936 free to **2,672**, which is the `StatusFacts` local, and is still a
margin.

#### 10.8.6 Wi-Fi — the screen, and the list behind it

**What this section specified**, and it is still the end state: the front of
§10.9 — a list of what was found, sorted by signal, each with a lock glyph and
RSSI; the remembered ones marked; tap to join, long-press to forget. A password
goes in on the keyboard below, with a show/hide toggle — a mistyped WPA key and
an out-of-range AP look identical otherwise. `Other…` takes a hidden SSID by
hand.

**What shipped is smaller, and it is the repository owner's shape rather than a
subset chosen here.** Two screens, and between them they answer the three
questions somebody standing over the device actually has — what is this radio
doing, which network is it configured for, and what else is out there:

| | |
|---|---|
| **mode** | one row, cycling **off → client → ap → off**. Three states rather than the two the request named, because the shipped `config.json` has `wifi.active` false: a two-state row would switch the radio on the first time anybody pressed it and offer no way to switch it back off. It is also exactly `wifimgr::Desired`, which is what makes it one press rather than a mapping |
| **one record** | the access point's own name and password in AP mode; one remembered network's in client mode. Both drawn in full, password included |
| **arrows** | `‹ network 2/3 ›`, when there is more than one. They wrap, which is what lets `KEY` on that row be the whole stepper for an operator who is not touching the glass |
| **a name off the air** | the row at the bottom opens the second screen: a scan, five rows at a time, and a press puts the name into the record that was on the glass |
| **no keyboard** | so a *password* is read here and typed on the console (`wifi join`). The section below has that screen in millimetres and it is the piece of work still outstanding |
| **no way to add a network** | the owner's instruction. It is why the scan row draws itself faint with `no record` on a device that remembers none, and refuses with a sentence rather than growing the list |

Three decisions inside that are not obvious from the list:

- **the mode takes effect at once and everything else does not**, which looks
  inconsistent and is not. `wifimgr` re-reads the desired mode off the config on
  every pass, so a press on that row *is* the radio changing; a name picked off
  the air is an edit to a record whose password is still the old one, and
  applying it would walk the network list and fail an association nobody asked
  for. So: `config save` keeps it and `wifi retry` tries it, both said in the log
  line, and the screen carries `in memory - 'config save' keeps it` where its
  hint normally is. That is §10.15's rule for every setter, and the touch
  calibration next door already follows it;
- **the record shown follows `wifi.mode` and not `wifi.active`.** Off is a
  statement about the radio, not about which of the two records the operator was
  reading — so switching off leaves the access point's name on the glass, and
  only the row above it changes. It is also why the caller writes `wifi.mode`
  *only* when the new mode is not off: writing it anyway would move the record
  under a press that was about the radio;
- **an empty list and a refused scan are different screens.** A scan needs the
  radio for a second or two and can be refused outright; `nothing on the air` and
  `the radio refused` send somebody in different directions, which is §10.9's
  rule about `unknown` being an honest state arriving on a screen.

And two about where the work happens, both of them rules this firmware already
had:

- **the scan runs on a task of its own.** `wifimgr::Scan` blocks for a second or
  two, and the screen task is the one that polls the buttons — the same argument
  §10.8.1 makes about the chirp, and the same answer: a small task, a semaphore,
  and a screen that says `looking...` in the meantime. It logs the stack it never
  used on every scan, which is what turned 5,120 bytes of guess into 3,072 of
  measurement — see the end of this section;
- **a scan that comes back late is dropped.** The view ignores a result with no
  scan in flight, and a generation counter drops one belonging to a *previous*
  visit to the list — the rule `link_policy.h` and `sync_policy.h` both keep
  about a result nobody asked for.

What the console can and cannot do with it: `screen wifi` and `screen networks`
open either (and the second one starts a scan, which is navigation rather than a
press), and `screen` prints the mode, the record, the SSID and the state of the
list. **It never prints the password** — §10.15 keeps a passphrase out of every
log line and every console dump, and the glass is the one place it is meant to
be, to somebody holding the device.

Rules the screen has to keep because of what it is:

- **The password is a secret from the moment it is typed.** It goes into
  `config.json` (§10.15) — which is *not* encrypted, and §10.15 owns that
  trade — so the handling rules are the part that has to hold: never logged,
  never in a console dump, never in a crash trace.
- **Scanning while connected costs the connection a beat** — the radio has to
  leave the channel. Do not scan on a timer, only when this screen is open; and
  a request arriving mid-scan still preempts it (§10.8.1).
- Joining is not blocking: the screen shows the state machine's state
  (`connecting… / wrong password / no such network / connected`) rather than
  freezing on a spinner with no way back. **This one is kept as one line in the
  header**, right of the title: `wifimgr`'s current state, or its *failure* when
  the last attempt had one, because that section forbids spelling "wrong
  password" and "no such network" the same way. Desired above, current here.

##### Three things only the board could have said

All three came out of flashing it and typing `screen wifi`, and the first is the
kind of bug this document keeps finding in the same place — an *ordering*.

- **The screen showed a mode and a record count from nowhere, for a frame.** The
  readout was three lines that contradicted each other: `mode off`, `no networks`,
  and an SSID printed live out of the config. `SyncWifi` runs near the top of the
  task loop and the pending navigation is consumed further down, so on the pass
  that *arrives* at this screen the sync had already looked, seen the settings
  list, and declined to run — leaving the view on its own defaults until the next
  pass. On the glass that is a tenth of a second of `off` / `no networks` before
  it corrects itself, which is exactly the flicker the host suite has a test
  against, arriving from the one direction the host suite cannot see. Every route
  in goes through `Apply(ui::Nav)`, so that is where the sync belongs, and it is
  there now.
- **The scan task's stack was two thirds too big, and the log line is what said
  so.** Two measurements, and the second is the one worth having: **604 bytes**
  used for a scan with the radio already up, and **1,356** for a scan with
  `wifi.active` off — where `wifimgr::Scan` brings the whole Wi-Fi stack up inside
  this task and puts it back, which is the deep path and the only one that could
  have justified the original number. 3,072 now, still more than twice the
  measured peak, and 2 KB of RAM back on a device that runs with about 30 KB free.
- **`from the air` became `scan networks`**, at the repository owner's request,
  and the rename cost a layout change rather than a string: thirteen characters at
  Montserrat 28 is about 200 px, and the row's right-hand note column started at
  210 — so `no record` and the label would have shared pixels the moment a device
  with no networks opened this screen. The column is 150 wide now. That is
  §10.8.5's finding about the status page's label column, seen coming this time
  instead of photographed after the fact.

##### What is written, and the five decisions inside it

The split is the one every screen here takes, and this is the sixth pair — with
two screens on the painting side because there are two screens:

| File | What it is |
|---|---|
| `ui/wifi_view.h/.cpp` | **`ui::WifiView`** — the mode cycle and its mapping onto `config.json`'s two fields, which record is on the glass, what each row's press means, and the scan list with its selection and its window. `<cstdint>`/`<cstddef>` only, so all of it is host-tested (§10.11) |
| `screens/wifi_screen.h/.cpp` | three rows, two arrows, two values and a state line. No decisions |
| `screens/wifi_scan_screen.h/.cpp` | five rows and a headline. No decisions |
| `screens/screens.cpp` | where the two meet the world: the scan's own task, the record's strings read out of `config.json`, and the three ways in that all end at `Apply(ui::Nav)` |

Five things are decisions rather than plumbing, and four of them are above. The
fifth is small and would otherwise be rediscovered: **the arrows are their own
touch targets and are taken before the row**, so a press on one steps the record
without moving the selection — otherwise a later `KEY` would act on a row the
operator never chose. Their click area is extended by 26 px, because a chevron is
20 px wide and a fingertip is 100 (the millimetres are below).

What it costs, measured rather than assumed: **7,551 bytes of flash** for the
three new files, and **5,625 bytes of static RAM** — of which **3,072 is the scan
task's stack** and the rest is the two screens' label buffers, the view's
sixteen-entry list, and the driver's own results array. `libui.a` is 8,560 bytes
now (all flash, as always) and `libscreens.a` 66,435.

##### The keyboard is a 6×5 grid, and the reason is millimetres

This section used to say "the LVGL keyboard" and mean `lv_keyboard`. It does
not, and the argument is one number the rest of this document has never written
down.

**The panel is 38.8 mm across.** 2.16″ on the diagonal of a square 480×480 glass
is a side of **38.79 mm** and a pixel pitch of **0.081 mm** — 12.4 px/mm, about
314 ppi. Neither `board.h` nor anything in `docs/` records it (the vendor ships a
schematic and a pinout, not a mechanical drawing), so it is derived from the one
figure the product page gives — and it is written here because every decision
below is that number in disguise.

What it does to LVGL's own keyboard, **measured in the host preview (§10.12.1)
at 480×480** rather than argued about:

| `lv_keyboard` | Key | In millimetres |
|---|---|---|
| the bottom half (240 px) | 43 × 55 px | **3.5 × 4.4 mm** |
| the whole screen (408 px) | 43 × 96 px | **3.5 × 7.8 mm** |

**Height was never the constraint, and width cannot be bought with more of the
screen**: a QWERTY row is ten columns whatever its height, and ten columns
across 38.8 mm is 3.9 mm of pitch. A fingertip is 8–10 mm across, the smallest
touch target anybody recommends is 7 mm, and at 3.5 mm the finger covers the key
it is pressing and both of its neighbours. **None of that is the touch
controller's fault** — the CST9220 reports the panel's own 480×480 grid and
§10.8.5 corrects what offset there is. It is the hand, and no calibration
addresses a hand.

So the number of columns is chosen from the millimetres, and the layout follows
from the columns:

| Columns | Key | mm | |
|---|---|---|---|
| 10 (QWERTY) | 43 px | 3.5 | no |
| 8 | 55 px | 4.4 | no |
| **6** | **74 px** | **6.0** | ← **chosen**: the fewest columns that still hold the alphabet on one page |
| 5 | 90 px | 7.3 | comfortable, and a second page for the tail of the alphabet |
| 4 | 114 px | 9.2 | comfortable, at three or four pages |

**Six columns by five rows is thirty cells, and the lower-case alphabet plus the
four keys that matter is exactly thirty**: `a`…`z`, then shift, `123`, backspace
and done. One page to type a password on, and one more for digits and symbols —
rather than a keyboard that changes shape under the finger every third
character.

- **Alphabetical, not a QWERTY cut into six.** What a QWERTY layout buys is the
  shape of its rows, and that does not survive being re-flowed six wide; what is
  left is a scrambled alphabet. Somebody hunting a letter finds it faster in the
  order they already know.
- **A preview bubble above the pressed key**, the way a phone does it. At 6 mm
  the finger hides the key it is on, and what is being typed is masked as well —
  so without it a wrong letter is invisible twice over, and the operator learns
  about it from `wrong password` a minute later.
- **70 px of header and 410 of keyboard is 480 exactly**: the SSID and its
  security on one line, what has been typed so far and the show/hide eye on the
  next, and the grid under them. Nothing is left for the scan list, which
  **settles** a layout question rather than losing one — entering the password
  is its own screen, reached from the list, not a panel that rises under it.

Two costs that are already paid, so that nobody re-argues them when this is
built. **Montserrat 28 is enabled and, since §10.8.3, actually referenced**, so
key labels at that size are free — 48 is the one that costs 97 KB the moment
anything names it. And a thirty-key `lv_buttonmatrix` is a few hundred bytes of
LVGL's 64 KB pool: the screen is built at boot and kept, like every other one
(§10.14.1), which is also what keeps a half-typed password alive under a request
card that preempts it (§10.8.1).

**And the two ways round it stay, because 6 mm is workable rather than
pleasant.** A 30-character WPA key is `wifi join` on the console (§10.9) — which
exists, and is what this screen is competing with rather than replacing. The
fallback access point is the other one, and it is no longer the hypothetical this
paragraph first described: **it was worth it, and it is built** — §10.16 serves a
configuration site off SPIFFS whose Wi-Fi page writes, over the device's own AP,
on `esp_http_server` which was in-tree and therefore cost no dependency under
root §1. A phone's keyboard beats 6 mm every time, and that is now something an
operator can actually use rather than an argument. What this screen would buy is
that neither has to be reached for to get a device onto a network — which is why
it is still worth building even though the way round it exists.

