// The arithmetic behind the one emitter (CLAUDE.md §10.17). No ESP-IDF, no
// allocation, no clock of its own — every function here is given the time it
// should reason about, which is what lets §10.11's host tier run the whole file.

#include "led_frames.h"

namespace led {
namespace {

// The on/off halves of each blink, in milliseconds. The house firmware of
// §10.14.4 packs these two numbers into an enumerator; here they are a table,
// because a packed constant is a thing you have to decode before you can read it
// and there is no wire format asking for one.
struct Rhythm {
    uint32_t on_ms;
    uint32_t off_ms;
};

constexpr Rhythm kRhythm[] = {
    {0, 0},        // kSolid — never dark, and the times are unused
    {100, 2000},   // kBeacon
    {1000, 1000},  // kSlowBlink
    {500, 500},    // kNormBlink
    {200, 200},    // kFastBlink
    {0, 0},        // kBreathe — the ramp below, not a duty cycle
};

constexpr uint8_t Clamp255(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return static_cast<uint8_t>(value);
}

// One channel of one breath frame: the colour's own value, the operator's
// ceiling and the ramp's position, divided once and **rounded** rather than
// truncated. The rounding is worth a word — truncation biases every level down,
// which at the bottom of the travel is the difference between a step that is dim
// and a step that is off.
constexpr uint8_t BreathLevel(uint8_t channel, int percent, int level) {
    return Clamp255((channel * percent * level + 12750) / 25500);
}

// bits -> the inverted-UART character that spells them (§10.17.1).
constexpr uint8_t kEncode2Bit[4] = {0x37, 0x07, 0x34, 0x04};

void EncodeByte(uint8_t value, uint8_t *out) {
    out[0] = kEncode2Bit[(value >> 6) & 0x03];
    out[1] = kEncode2Bit[(value >> 4) & 0x03];
    out[2] = kEncode2Bit[(value >> 2) & 0x03];
    out[3] = kEncode2Bit[value & 0x03];
}

}  // namespace

// Ninety steps up and the same ninety back down — the duty cycle that makes
// **perceived** lightness walk from 0 to 100 in a straight line, generated from
// the CIE inverse in `led_frames.h`'s comment rather than hand-tuned.
//
// Read it as a shape and it looks wrong, which is the point: the numbers crowd
// the bottom because that is where the eye's resolution is. The zero at each end
// makes the breath actually reach dark rather than hovering just above it, and
// the 255 at the top makes it actually reach the operator's ceiling — the two
// ends of `config set led`, neither of them approximated.
const uint8_t kBreathRamp[kBreathSteps] = {
      0,   0,   1,   1,   1,   2,   2,   2,   3,   3,  //
      3,   4,   4,   5,   5,   6,   6,   7,   8,   9,  //
      9,  10,  11,  12,  13,  14,  15,  16,  17,  19,  //
     20,  21,  23,  24,  26,  28,  29,  31,  33,  35,  //
     37,  39,  41,  43,  46,  48,  51,  53,  56,  59,  //
     61,  64,  67,  70,  74,  77,  80,  84,  87,  91,  //
     95,  99, 103, 107, 111, 115, 120, 124, 129, 134,  //
    139, 144, 149, 154, 159, 165, 170, 176, 182, 188,  //
    194, 200, 207, 213, 220, 226, 233, 240, 248, 255,  //
    255, 248, 240, 233, 226, 220, 213, 207, 200, 194,  //
    188, 182, 176, 170, 165, 159, 154, 149, 144, 139,  //
    134, 129, 124, 120, 115, 111, 107, 103,  99,  95,  //
     91,  87,  84,  80,  77,  74,  70,  67,  64,  61,  //
     59,  56,  53,  51,  48,  46,  43,  41,  39,  37,  //
     35,  33,  31,  29,  28,  26,  24,  23,  21,  20,  //
     19,  17,  16,  15,  14,  13,  12,  11,  10,   9,  //
      9,   8,   7,   6,   6,   5,   5,   4,   4,   3,  //
      3,   3,   2,   2,   2,   1,   1,   1,   0,   0};

const char *EffectName(Effect effect) {
    switch (effect) {
        case Effect::kSolid:
            return "solid";
        case Effect::kBeacon:
            return "beacon";
        case Effect::kSlowBlink:
            return "slow";
        case Effect::kNormBlink:
            return "blink";
        case Effect::kFastBlink:
            return "fast";
        case Effect::kBreathe:
            return "breathe";
    }
    return "?";
}

Rgb Scale(Rgb colour, uint8_t percent) {
    if (percent >= 100) {
        return colour;
    }
    if (percent == 0) {
        return colour::kOff;
    }
    const int p = percent;
    return Rgb{Clamp255(colour.r * p / 100), Clamp255(colour.g * p / 100),
               Clamp255(colour.b * p / 100)};
}

void Animator::Set(Rgb colour, Effect effect, uint8_t percent, uint32_t now_ms) {
    // **The no-op check is the whole reason this takes a clock.** `indicator`
    // re-asserts the current state on every tick, and restarting the phase each
    // time would pin a breath to its first step and a beacon to permanently on.
    if (colour == colour_ && effect == effect_ && percent == percent_ && !overriding_) {
        return;
    }
    colour_ = colour;
    effect_ = effect;
    percent_ = percent;
    // A `Set` while an override is running replaces what the override will fall
    // back *to*, and does not cut the override short: the flash that says
    // "denied" has to finish being seen, and the state underneath it moving on
    // is exactly the ordinary case rather than a conflict.
    if (!overriding_) {
        phase_at_ms_ = now_ms;
    }
}

void Animator::SetFor(Rgb colour, Effect effect, uint8_t percent, uint32_t duration_ms,
                      uint32_t now_ms) {
    over_colour_ = colour;
    over_effect_ = effect;
    over_percent_ = percent;
    over_until_ms_ = now_ms + duration_ms;
    overriding_ = true;
    phase_at_ms_ = now_ms;
}

void Animator::EndFor(uint32_t now_ms) {
    if (!overriding_) {
        return;
    }
    overriding_ = false;
    // The phase restarts with the state underneath, the same way an override that
    // expires on its own restarts it in `FrameAt` — otherwise a breath would
    // resume mid-cycle at whatever the prompt's blink had left behind.
    phase_at_ms_ = now_ms;
}

Rgb Animator::FrameAt(uint32_t now_ms, uint32_t *next_ms) {
    uint32_t next = 1000;

    if (overriding_ && static_cast<int32_t>(now_ms - over_until_ms_) >= 0) {
        overriding_ = false;
        phase_at_ms_ = now_ms;
    }

    const Rgb colour = overriding_ ? over_colour_ : colour_;
    const Effect effect = overriding_ ? over_effect_ : effect_;
    const uint8_t percent = overriding_ ? over_percent_ : percent_;

    Rgb frame = colour::kOff;
    if (effect == Effect::kBreathe) {
        const uint32_t elapsed = now_ms - phase_at_ms_;
        const size_t step = (elapsed / kBreathStepMs) % kBreathSteps;
        // The ramp is 0..255 of the operator's ceiling, so the two compose rather
        // than fight: `percent` says how bright this may ever be, the ramp says
        // how far through that it is now.
        //
        // **One multiply, not `Scale` and then another** — and that is not tidying.
        // Rounding to eight bits twice throws away the low end of a curve whose
        // low end is the whole point: at the 8 % idle ceiling the whole breath
        // lives in twenty duty levels, and half of them are in the bottom quarter
        // of the travel. Combining the two divisions keeps them.
        const int p = percent > 100 ? 100 : percent;
        const int level = kBreathRamp[step];
        frame = Rgb{BreathLevel(colour.r, p, level), BreathLevel(colour.g, p, level),
                    BreathLevel(colour.b, p, level)};
        next = kBreathStepMs - (elapsed % kBreathStepMs);
    } else if (effect == Effect::kSolid) {
        frame = Scale(colour, percent);
        // Nothing is going to change on its own. A second is not a frame rate —
        // it is how often the emitter is refreshed anyway, which is what keeps a
        // WS2812 that lost a bit to a glitch from staying wrong forever.
        next = 1000;
    } else {
        const Rhythm &rhythm = kRhythm[static_cast<size_t>(effect)];
        const uint32_t period = rhythm.on_ms + rhythm.off_ms;
        const uint32_t into = (now_ms - phase_at_ms_) % period;
        if (into < rhythm.on_ms) {
            frame = Scale(colour, percent);
            next = rhythm.on_ms - into;
        } else {
            frame = colour::kOff;
            next = period - into;
        }
    }

    // An override that is due to end before this frame would otherwise expire
    // shortens the sleep, so the fall-back happens on time rather than one beat
    // late. Without it a `SetFor` under a solid state could be a second long
    // when it was asked to be 120 ms.
    if (overriding_) {
        const uint32_t remaining = over_until_ms_ - now_ms;
        if (remaining < next) {
            next = remaining;
        }
    }

    if (next == 0) {
        next = 1;
    }
    if (next_ms != nullptr) {
        *next_ms = next;
    }
    return frame;
}

size_t EncodePixel(Rgb colour, uint8_t *out, size_t capacity) {
    if (out == nullptr || capacity < kBytesPerPixel) {
        return 0;
    }
    // GRB. The one place in this firmware that knows the part's byte order.
    EncodeByte(colour.g, out + 0);
    EncodeByte(colour.r, out + 4);
    EncodeByte(colour.b, out + 8);
    return kBytesPerPixel;
}

}  // namespace led
