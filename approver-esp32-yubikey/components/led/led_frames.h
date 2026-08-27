#pragma once

// **The half of the LED that has no ESP-IDF in it** (CLAUDE.md §10.17): what
// colour the emitter should be at a given millisecond, and what bytes that
// colour turns into on the wire. Both are arithmetic, both are the part that is
// easy to get subtly wrong, and both are what §10.11's host tier can run with no
// board — the same split `buttons::Debounce` is carved out for, and for the same
// reason.
//
// Nothing here talks to a UART, allocates, or knows what a request is. The
// driver is `ws2812.h` and the policy is `indicator.h`; this is the arithmetic
// between them.

#include <cstddef>
#include <cstdint>

namespace led {

// A colour, in the order a human writes one. **Not the order the wire wants** —
// a WS2812 is fed green first, and that swap lives in `EncodePixel` below, which
// is the one place that has to know it.
struct Rgb {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    constexpr bool operator==(const Rgb &other) const {
        return r == other.r && g == other.g && b == other.b;
    }
    constexpr bool operator!=(const Rgb &other) const { return !(*this == other); }
    constexpr bool Dark() const { return r == 0 && g == 0 && b == 0; }
};

// **The palette is compiled in, and §10.17 says why it may not be a setting**:
// on a device whose only output is one emitter, which colour means what *is* the
// user interface, and an operator who can recolour "denied" can build a device
// that lies about what it did. `config::Led` may scale these and may not choose
// them.
namespace colour {
inline constexpr Rgb kOff{0, 0, 0};
inline constexpr Rgb kWhite{255, 255, 255};
inline constexpr Rgb kRed{255, 0, 0};
inline constexpr Rgb kGreen{0, 255, 0};
inline constexpr Rgb kBlue{0, 0, 255};
inline constexpr Rgb kAmber{255, 140, 0};
// **Yellow and amber are both here and they are not interchangeable** (§10.17):
// yellow is every state between power-on and a NATS connection, amber is not
// used for any of them. On a bare WS2812 the two are close enough that giving
// them adjacent meanings would be a design that only works on paper, so they
// have meanings that never appear at the same time.
inline constexpr Rgb kYellow{255, 210, 0};
inline constexpr Rgb kCyan{0, 255, 255};
inline constexpr Rgb kMagenta{255, 0, 255};
}  // namespace colour

// The rhythm, which on this device carries as much meaning as the colour does
// (§10.17): the colour says *what state*, the rhythm says *how much it wants
// you*. Named after what it looks like rather than after what asks for it, so
// that a state can change its mind about which one it uses without renaming an
// enumerator.
//
// The four blink rates and the breath are the house firmware's of §10.14.4,
// carried over numbers and all, because they were chosen against a real emitter
// on a real desk and re-deriving them from nothing would be re-deriving them
// worse.
enum class Effect : uint8_t {
    kSolid = 0,  // on, and staying on
    kBeacon,     // 100 ms in every 2 s — present, and not asking for anything
    kSlowBlink,  // 1 s / 1 s
    kNormBlink,  // 500 ms / 500 ms
    kFastBlink,  // 200 ms / 200 ms
    kBreathe,    // the perceptually linear ramp below, ~9 s a cycle
};

const char *EffectName(Effect effect);

// **The duty cycle that makes perceived brightness climb in a straight line**,
// which is the *inverse* of the perceptual curve rather than the curve itself.
// That distinction is the whole content of this table and it is easy to get
// exactly backwards — this firmware did, and §10.17.5 records what it looked
// like: the emitter reached 63 % of its apparent brightness one 150 ms step in
// and then spent the remaining four seconds buying the other 37 %, so the breath
// read as a snap to full followed by eight seconds of sitting there.
//
// The model is the one §10.17.5 quotes, solved for duty instead of for
// appearance. CIE lightness is `L* = 116·Y^(1/3) − 16` above `Y = 0.008856` and
// `903.3·Y` below it; walking `L*` linearly from 0 to 100 and taking the `Y` that
// produces it gives this ramp. Each step is about **1.1 L\* units**, which is near
// enough the just-noticeable difference that the travel has no visible stairs and
// no visible plateau.
//
// The low, linear segment of CIE is why this is not simply `x^3`: a cube leaves
// the bottom fifteen steps quantised to zero on an 8-bit part, which is a breath
// with three quarters of a second of black at the bottom of it. Here only the
// first two steps are dark, and that reads as the pause a breath has anyway.
//
// **What is carried over from the house firmware of §10.14.4 is the nine-second
// period, not the table** — that number was chosen against a real emitter on a
// real desk and is slow enough to read as *resting* rather than as *pulsing at
// you*. 180 steps at 50 ms is the same nine seconds at three times the temporal
// resolution, which the corrected curve needs: its interesting region is no
// longer crammed into the first step.
inline constexpr size_t kBreathSteps = 180;
extern const uint8_t kBreathRamp[kBreathSteps];

// How often the breath advances one step. Everything else in this file is
// event-driven; this is the one effect with a frame rate.
inline constexpr uint32_t kBreathStepMs = 50;

// Scale a colour to a percentage, the way the house firmware does it: multiply
// and divide once, in `int`, so that 1 % of 255 is 2 rather than 0.
//
// **Not gamma-corrected**, and that is deliberate rather than an omission. This
// is the operator's ceiling — "how bright may this thing be" — and a ceiling
// that is not proportional to the number typed is a ceiling nobody can reason
// about: a `led` readout saying `50%` next to an emitter running at 12 % duty is
// a readout that has to be explained every time. The perceptual curve is
// `kBreathRamp`'s job, and it applies on top.
//
// **What it costs is that the top three quarters of the range do almost
// nothing**, which is a real effect somebody will meet before they meet this
// comment: perceived lightness goes as the cube root of power, so half of what a
// person can see lives below 20 % here. §10.17.5 has the table, the second reason
// (a bare die past a fifth reads as glare rather than as a hue), and the one line
// that would change this if the numbers should ever mean perceived brightness.
Rgb Scale(Rgb colour, uint8_t percent);

// What the emitter should be showing, given a colour, a rhythm and a clock.
//
// **It is a pull, not a push**: the caller asks what to show now and is told
// when to ask again, which is what lets the task sleep for two seconds through a
// beacon's dark half instead of waking sixty times a second to discover nothing
// changed.
//
// Times are milliseconds in `uint32_t` and are only ever subtracted, so the wrap
// at ~49 days is arithmetic rather than a case — the same rule
// `buttons::Debounce` states.
class Animator {
   public:
    // Adopt a colour and a rhythm. Restarts the phase **only when something
    // actually changed**, which is what keeps a caller that re-asserts the same
    // state every tick — and `indicator` is exactly that caller — from freezing
    // a breath at its first step forever.
    void Set(Rgb colour, Effect effect, uint8_t percent, uint32_t now_ms);

    // Same, but reverting to whatever was set before once `duration_ms` has
    // passed. This is how a verdict flashes for a second and the device then
    // goes back to being idle, without the caller having to remember what idle
    // looked like.
    void SetFor(Rgb colour, Effect effect, uint8_t percent, uint32_t duration_ms,
                uint32_t now_ms);

    // **End a running override now**, falling back to the state underneath.
    // A no-op when nothing is overriding.
    //
    // This exists because not every override is a flash. A verdict has a duration
    // of its own and must not be cut short — that is why `Set` deliberately does
    // not do it. A prompt is the other kind: "touch the key now" is true only
    // until the key is touched, and a light still asking after the answer arrived
    // is a light that lies (§10.17). So ending one early is its own call, made by
    // the caller that put the prompt up.
    void EndFor(uint32_t now_ms);

    // What to put on the wire now. `next_ms` is filled with how long this frame
    // is good for; a caller may sleep that long, and must not sleep longer.
    Rgb FrameAt(uint32_t now_ms, uint32_t *next_ms);

    // What was last asked for, ignoring the phase — for a console readout that
    // wants to name the state rather than the instant.
    Rgb Colour() const { return overriding_ ? over_colour_ : colour_; }
    Effect CurrentEffect() const { return overriding_ ? over_effect_ : effect_; }
    uint8_t Percent() const { return overriding_ ? over_percent_ : percent_; }
    bool Overriding() const { return overriding_; }

   private:
    Rgb colour_{};
    Effect effect_ = Effect::kSolid;
    uint8_t percent_ = 0;

    bool overriding_ = false;
    Rgb over_colour_{};
    Effect over_effect_ = Effect::kSolid;
    uint8_t over_percent_ = 0;
    uint32_t over_until_ms_ = 0;

    uint32_t phase_at_ms_ = 0;
};

// --- The wire (§10.17.1) --------------------------------------------------
//
// **A WS2812 driven from a UART, which is the house firmware's trick and not a
// new idea here.** The part reads a 1.25 us bit cell whose duty cycle carries
// the value; a UART at 3,333,333 baud with **six** data bits, one start and one
// stop puts eight bit-times of 300 ns each on the wire, and four of those groups
// spell one byte of colour. With TX inverted — the line idles low, which is what
// WS2812 wants — each pair of colour bits is one of four fixed characters.
//
// What it buys over RMT is a peripheral this firmware was not otherwise going to
// use, no managed component, and a write with no ISR of our own. What it costs
// is a UART, and on this board that is UART1: UART0 is the console on the CH343P
// bridge (§10.1), and the console is not negotiable.
//
// The table itself: bits 00 -> 0x37, 01 -> 0x07, 10 -> 0x34, 11 -> 0x04.
inline constexpr size_t kBytesPerPixel = 12;

// Encodes one pixel — **GRB, which is what the part reads**, not the RGB it was
// written as. Returns bytes written, or 0 when `capacity` is short.
size_t EncodePixel(Rgb colour, uint8_t *out, size_t capacity);

}  // namespace led
