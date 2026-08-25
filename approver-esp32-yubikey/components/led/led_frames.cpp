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

// bits -> the inverted-UART character that spells them (§10.17.1).
constexpr uint8_t kEncode2Bit[4] = {0x37, 0x07, 0x34, 0x04};

void EncodeByte(uint8_t value, uint8_t *out) {
    out[0] = kEncode2Bit[(value >> 6) & 0x03];
    out[1] = kEncode2Bit[(value >> 4) & 0x03];
    out[2] = kEncode2Bit[(value >> 2) & 0x03];
    out[3] = kEncode2Bit[value & 0x03];
}

}  // namespace

// Sixty steps up and back down, from the house firmware of §10.14.4. The zero at
// each end is what makes the breath actually reach dark rather than hovering
// just above it, and the plateau in the middle is the curve, not a typo.
const uint8_t kBreathRamp[kBreathSteps] = {
    0,   80,  101, 115, 127, 137, 145, 153, 159, 166,  //
    172, 177, 182, 187, 192, 196, 200, 205, 208, 212,  //
    216, 219, 223, 226, 229, 232, 235, 238, 241, 245,  //
    245, 241, 238, 235, 232, 229, 226, 223, 219, 216,  //
    212, 208, 205, 200, 196, 192, 187, 182, 177, 172,  //
    166, 159, 153, 145, 137, 127, 115, 101, 80,  0};

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
        // The ramp is 0..245 of the *scaled* colour, so the operator's ceiling
        // and the perceptual curve compose rather than fight: `percent` says how
        // bright this may ever be, the ramp says how far through that it is now.
        const Rgb ceiling = Scale(colour, percent);
        frame = Rgb{Clamp255(ceiling.r * kBreathRamp[step] / 255),
                    Clamp255(ceiling.g * kBreathRamp[step] / 255),
                    Clamp255(ceiling.b * kBreathRamp[step] / 255)};
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
