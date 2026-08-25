#include "age_text.h"

namespace ui {
namespace {

// Digits into a buffer, without `<cstdio>`. The same shape `protocol::AppendInt`
// takes and for the same reason: nothing here goes through a format string, and
// nothing here can overrun.
size_t AppendUnsigned(uint64_t value, char *out, size_t capacity, size_t at) {
    char digits[20];
    size_t n = 0;
    do {
        digits[n++] = static_cast<char>('0' + static_cast<int>(value % 10));
        value /= 10;
    } while (value != 0);

    while (n > 0 && at + 1 < capacity) {
        out[at++] = digits[--n];
    }
    return at;
}

}  // namespace

void AgeText(uint32_t seconds, char *out, size_t capacity) {
    if (out == nullptr || capacity == 0) {
        return;
    }
    size_t at = 0;

    // Under ninety seconds the seconds *are* the answer, which is what a live
    // readout shows: `3 s ago` on a console line typed while something is
    // happening.
    if (seconds < 90) {
        at = AppendUnsigned(seconds, out, capacity, at);
        if (at + 1 < capacity) {
            out[at++] = ' ';
        }
        if (at + 1 < capacity) {
            out[at++] = 's';
        }
        out[at] = '\0';
        return;
    }

    const uint32_t minutes = seconds / 60;
    if (minutes < 90) {
        at = AppendUnsigned(minutes, out, capacity, at);
        if (at + 1 < capacity) {
            out[at++] = ' ';
        }
        if (at + 1 < capacity) {
            out[at++] = 'm';
        }
        out[at] = '\0';
        return;
    }

    at = AppendUnsigned(minutes / 60, out, capacity, at);
    if (at + 1 < capacity) {
        out[at++] = 'h';
    }
    if (at + 1 < capacity) {
        out[at++] = ' ';
    }
    // Two digits, padded: `8h 5m` and `8h 05m` are the same duration, and only
    // one of them lines up with the line above it.
    const uint32_t rest = minutes % 60;
    if (rest < 10 && at + 1 < capacity) {
        out[at++] = '0';
    }
    at = AppendUnsigned(rest, out, capacity, at);
    if (at + 1 < capacity) {
        out[at++] = 'm';
    }
    out[at] = '\0';
}

}  // namespace ui
