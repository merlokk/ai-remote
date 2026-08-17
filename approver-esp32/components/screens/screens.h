#pragma once

// The screens of CLAUDE.md §10.8, and the one task that keeps them current.
//
// It is `wifimgr` / `nats` / `timesync` one layer up and in the same shape: a
// task, a snapshot of the world taken at one instant, and no decisions of its
// own — the decisions are `ui::ClockFace`'s (host-tested, §10.11) and the pixels
// are `clock_screen.cpp`'s. What is left in the middle is *gathering*, which is
// the part that cannot be tested without a board and is therefore deliberately
// the part with nothing in it worth testing.
//
// One of the five screens exists (§10.8.2, the clock). When the other four
// arrive, this is where the navigator (`ui/navigator.h`) will sit: which screen
// is up is its answer, and this task is what would carry that answer to LVGL.
//
// Two rules it exists to keep, both §10.8.1's:
//
//   * **the LVGL task owns the display.** Nothing here touches a widget outside
//     `display::Lock`, and the lock is taken with a bound — an LVGL lock waited
//     on forever from another task is a hang that looks like a hardware fault;
//   * **an I²C read never happens inside an LVGL callback.** The PMIC is a dozen
//     registers under a lease (§10.14.3) and this task is where that happens, at
//     its own slow rate, well away from the frame the operator is looking at.
//
// The PMIC is passed in rather than reached for, the way `timesync::Init` takes
// the RTC: this component has never heard of `board.h`, and `main` is where the
// two meet (§10.14.2).

#include <cstdint>

#include "axp2101.h"
#include "buttons.h"
#include "clock_face.h"
#include "esp_err.h"
#include "speaker.h"
#include "request_card.h"

namespace screens {

// **The tick is the buttons' rate, not the screen's.** It was 100 ms while the
// clock was the only thing here; a deliberate press is 50-200 ms long, and a poll
// that slow can miss one entirely — which on the screen this device exists for is
// a press somebody made and the card did not take. `buttons.h` asks callers not
// to go far above 10 ms, so 20 it is: fifty GPIO reads a second, which is free.
inline constexpr uint32_t kTickMs = 20;

// The face is still repainted ten times a second — that is what makes the water
// of §10.8.2 read as flowing rather than stepping, and it is affordable because
// only the digits are redrawn at that rate.
inline constexpr uint32_t kFaceEveryTicks = 5;

// Below LVGL's own task (`display::kLvglTaskPriority`), because this one's job
// is to hand it work rather than to compete with it.
inline constexpr int kTaskPriority = 3;
inline constexpr uint32_t kTaskStackBytes = 4096;

// How long to wait for the LVGL lock before giving this tick up. A skipped
// frame is invisible; a blocked task is a watchdog panic with somebody else's
// name on it.
inline constexpr uint32_t kLockTimeoutMs = 100;

// The battery is read every this-many ticks rather than every tick: it is a
// dozen I²C registers under a lease, and a charge percentage that is two seconds
// stale is a charge percentage.
inline constexpr uint32_t kBatteryEveryTicks = 100;

// A card's countdown is repainted whenever its whole-second value changes, so the
// card is applied on every face pass rather than every tick — one repaint a
// second, plus the ones a press or an arrival cause.
static_assert(kTickMs * kFaceEveryTicks == 100, "the face rate is what §10.8.2 argues for");

// What is on the glass, taken at one instant — what `clock` on the console
// prints. **A snapshot rather than a look at LVGL**: the console must not touch a
// widget (§10.8.1's one-task rule) and must not wait behind the display lock to
// answer a question, so the task keeps this up to date and nothing here reaches
// into anything.
//
// It exists because a screen is the one part of this firmware whose output cannot
// be captured from a script. Every other component has a readout the four-places
// rule of §10.7 hangs off; without this one, "the drift is moving" would be a
// claim nobody could check without a camera.
struct Status {
    bool ready = false;

    ui::ClockView view = {};

    uint32_t updates = 0;      // how many times the face has been recomputed
    uint32_t lock_misses = 0;  // …and how many of those gave the frame up

    // The **lowest** free stack the task has ever had, the same call §10.14.1
    // makes about the heap: the minimum ever seen is the number that says
    // whether the device is safe.
    uint32_t stack_low_water = 0;
};

// Which physical button means what (§10.8.4). **Indices rather than pins**, and
// filled in by `main`: this component has never heard of `board.h`, and which
// button is where is exactly the kind of fact §10.14.2 keeps out of it.
//
// The mapping is the operator's, not this file's: `BOOT` says yes, `PWR` says no
// — and `PWR` doubles as "back to the clock" when there is nothing to say no to.
//
// **One thing about `PWR` that is hardware and not ours** (§10.1): it is wired to
// the AXP2101's PWRON pin, and holding it for six seconds powers the board off
// whatever this firmware thinks. A short press is a deny; a long one is a
// shutdown, and no code here participates in that.
//
// And one risk worth naming rather than discovering: `PWR` is the button people
// press to *get out* of a screen, so muscle memory will occasionally deny a
// request somebody meant to read. That is the safe direction to be wrong in
// (§10.10: never a silent allow), which is why the mapping is acceptable.
struct Keys {
    buttons::Buttons *buttons = nullptr;
    size_t allow = 0;
    size_t deny = 0;
};

// What the card is doing, for `request` on the console. A snapshot, like
// `Status` above and for the same reasons.
struct CardStatus {
    bool ready = false;

    ui::CardState state = ui::CardState::kIdle;
    uint8_t pending = 0;
    uint8_t waiting = 0;
    uint32_t remaining_ms = 0;

    char tool[ui::kToolNameSize] = {};
    char cwd[ui::kCwdSize] = {};

    // **A preview, and it says how much it is not showing.** The whole
    // `tool_input` is 2 KB and this snapshot is copied onto a caller's stack; the
    // screen is where a command is read in full, and §10.8.4 is about the screen.
    char input_preview[240] = {};
    uint16_t input_length = 0;

    ui::Outcome last_outcome = ui::Outcome::kNone;
    char last_tool[ui::kToolNameSize] = {};

    uint16_t allowed = 0;
    uint16_t denied = 0;
    uint16_t timed_out = 0;
    uint16_t refused = 0;
    uint16_t ignored = 0;
};

// Builds the screens on LVGL's active screen and starts the tasks. LVGL has to be
// up already — `main` starts it — and a null `battery` is allowed: the icon then
// says what it always says when there is nothing to ask. A null `keys.buttons`
// is allowed too, and means the card cannot be answered — which is a device that
// still shows a request and still lets it time out (§10.10).
//
// `alert` may be null as well, and then a card arrives silently. Passed in rather
// than reached for, like the PMIC: this component has never heard of `board.h`.
esp_err_t Init(pmic::Axp2101 *battery, const Keys &keys, audio::Speaker *alert);

// The file played when a card goes up. In the SPIFFS image (§10.15), not compiled
// in — `speaker.h` argues why the firmware has no decoder.
inline constexpr const char *kAlertSound = "alert.wav";
bool Ready();

Status Get();

// --- The card (§10.8.4) --------------------------------------------------

// Put a request on the screen. False when the card queue refused it — full, a
// field that did not fit, or no reply subject — and the caller's job is then
// §10.10's: drop it, one log line, **no reply**.
//
// **The chirp of §10.8.1 happens here now**, and the sentence this replaces said
// it could not: `Speaker::PlayWav` blocks for the length of the file, and a
// screen task that stalls for three seconds cannot see the press it exists to
// see. That was an argument against playing it *on this task*, not against
// playing it — so there is a small task of its own for it, woken by this
// function, and the card going up is one rule in one place rather than something
// each caller has to remember.
//
// It is only ever a **new** card, which is §10.8.1's other half: this returns
// false for one the queue refused, and the semaphore behind it is binary, so four
// arriving at once are one noise rather than four.
//
// **Two callers now**: `request test` on the console, and `components/responder`
// with what arrived on `approvals.*`. This function cannot tell them apart and
// must not — a synthetic card and a real one are the same question put to the
// same human, and a screen that treated them differently would be a screen whose
// test does not test the thing.
bool Inject(const ui::Request &request);

CardStatus Card();

// --- Where a verdict goes (§7, §10.8.4) ----------------------------------

// Called on the screen task the moment a press decides a card, with the whole
// request the reply has to echo.
//
// **It must not sign or publish anything itself.** This runs on the task that
// polls the buttons and drives LVGL; `crypto_sign` needs 4 KB of stack and this
// task has 4 KB in total, and a task that stalls cannot see the next press
// (§10.8.1). The handler's job is to copy and hand off — `components/responder`
// is what does the signing, on a task sized for it.
using DecisionHandler = void (*)(const ui::Request &request, ui::Verdict verdict, void *user);

// A hook rather than a call, so that this component keeps knowing nothing about
// keys, subjects or the bus (§10.14.2) — and so that `responder` can depend on
// `screens` without `screens` depending back. With none set, a decision is a log
// line and the receipt says nothing was sent.
void OnDecision(DecisionHandler handler, void *user);

// The line under a receipt for a card somebody answered. Set by whoever handles
// the decision, because only they know whether it left the device: `Decided()`
// cannot say "sent" for a publish it did not make.
//
// Copied, and bounded — a caller that goes out of scope must not leave the screen
// pointing at freed text.
void SetReceiptNote(const char *note);

}  // namespace screens
