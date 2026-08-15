#pragma once

// The two POSIX functions the firmware uses that MSVC spells differently.
// Included by force (`/FI`) from CMakeLists rather than by any source file,
// because the sources under test are the ones that ship and must not grow a
// `#ifdef _WIN32`.
//
// `setenv`/`tzset` are what `tz::Apply` calls. The Windows CRT has both under
// underscored names and with the same semantics for what this uses them for —
// setting `TZ` and making libc re-read it.
//
// **A caveat worth stating**: the Windows CRT's `TZ` parser understands the
// simple `EST5EDT` form but not the `,M3.5.0/3,M10.5.0/4` transition rules
// §10.8.2's table is made of. So a test may assert that a rule was *stored*
// and *handed to libc*; it may not assert what local time comes out of it.
// That part is the device's, and §10.8.2 already says the table is only
// verifiable there.

#if defined(_MSC_VER) && defined(__cplusplus)

#include <cstdlib>
#include <ctime>

// `localtime_r` and `timegm` are POSIX; the Windows CRT has both under other
// names. **`localtime_s` takes its arguments the other way round** — buffer
// first, then the time — which is the sort of difference that compiles either
// way if you get it wrong, so it is written out rather than macro'd.
inline struct tm *localtime_r(const time_t *when, struct tm *out) {
    return localtime_s(out, when) == 0 ? out : nullptr;
}

inline struct tm *gmtime_r(const time_t *when, struct tm *out) {
    return gmtime_s(out, when) == 0 ? out : nullptr;
}

inline time_t timegm(struct tm *fields) { return _mkgmtime(fields); }

inline int setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite) {
        size_t needed = 0;
        if (getenv_s(&needed, nullptr, 0, name) == 0 && needed != 0) {
            return 0;
        }
    }
    return _putenv_s(name, value);
}

inline int unsetenv(const char *name) { return _putenv_s(name, ""); }

// No `tzset` here: MSVC already declares it (deprecated in favour of `_tzset`,
// which is why `_CRT_SECURE_NO_WARNINGS` is set), and redefining it would be a
// conflict rather than a shim.

#endif  // _MSC_VER && __cplusplus
