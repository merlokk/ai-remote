#pragma once

// Named time zones — `Europe/Kyiv` rather than `EET-2EEST,M3.5.0/3,M10.5.0/4`
// (CLAUDE.md §10.8.2).
//
// **The clock is UTC. A time zone is presentation, never storage.** The RTC
// holds UTC, `time_t` is UTC by definition, and everything that shows or reads
// a wall-clock time converts at the edge. That is the invariant this component
// exists to serve, and it is what makes a zone change cost nothing: no stored
// value moves, so a device flown to another country needs one setting and no
// migration.
//
// **What ESP-IDF gives, and what it does not.** libc understands POSIX TZ
// strings and nothing else: there is no IANA database in the framework, and v6
// did not add one (checked, not assumed). Shipping the real `tzdata` would be
// hundreds of kilobytes for a device with one clock face. So this is a table:
// a curated list of zone names, each with the POSIX rule that encodes it. The
// mechanism underneath — `setenv("TZ", …)` then `tzset()`, and the offset read
// back by comparing `localtime` with `gmtime` — is the house firmware's
// (§10.14.4, its `TimeUtil`), which keeps a name beside the rule for exactly
// this reason.
//
// **The cost is honest and worth stating: transition rules change.** A country
// that moves its DST dates needs this table edited and the firmware reflashed.
// That is why a raw POSIX string is always accepted too (`Custom` below) — an
// operator with a zone this table does not know, or one whose rules moved
// yesterday, is never stuck waiting for a release.
//
// Library layer (§10.14.2): it knows about zones, and nothing about approvals.

#include <cstddef>
#include <ctime>

#include "esp_err.h"

// **The namespace is `tz` and not `timezone` because libc owns that word**:
// `<time.h>` declares a `timezone` variable and `<sys/time.h>` a `struct
// timezone`, and a namespace of that name collides with both. The file keeps
// the readable name; the code gets the short one, which is also what the config
// field and the console subcommand are called.
namespace tz {

struct Zone {
    const char *name;   // IANA-style, e.g. "Europe/Kyiv"
    const char *posix;  // what libc is actually given
};

// The name used when the configured rule is a POSIX string this table has no
// name for. Stored, displayed, and never guessed at.
inline constexpr const char *kCustomName = "Custom";

// Longest name and rule in the table, so callers can size buffers without
// reaching for a magic number.
inline constexpr size_t kMaxNameLength = 40;
inline constexpr size_t kMaxPosixLength = 40;

size_t Count();
const Zone &At(size_t index);

// The POSIX rule for a zone name, or nullptr if the table does not have it.
// Case-insensitive, and it follows the aliases every user of this will type:
// `Europe/Kiev` is `Europe/Kyiv`, `Asia/Calcutta` is `Asia/Kolkata`.
const char *Lookup(const char *name);

// The reverse: the table's **first** name for a rule, or nullptr.
//
// "First" is the whole caveat: one rule serves many zones — every EU country on
// EET shares Kyiv's — so this answers "a zone that switches like this", not
// "the zone you meant". Fine for filling in a config that carries a rule and no
// name; wrong for naming what an operator just typed, which is why a raw rule
// is stored as `kCustomName` instead.
const char *NameFor(const char *posix);

// A cheap sanity check that a string is a POSIX TZ rule rather than a
// misspelled zone name — it must start with a letter, a `<` or a sign, and
// contain an offset. It exists so `config set tz Europe/Kyev` is refused with
// a suggestion instead of silently becoming a broken rule that libc reads as
// UTC.
bool LooksLikePosix(const char *value);

// `setenv("TZ", posix)` + `tzset()`. Everything that formats a local time
// afterwards — `localtime_r`, `strftime`, `mktime` — follows it.
esp_err_t Apply(const char *posix);

// The rule currently applied, for printing back.
const char *Current();

// The effective offset from UTC in seconds, DST included, at `when`
// (0 meaning now). Positive east of Greenwich, the opposite of the sign inside
// a POSIX string — which is one of the two things about POSIX TZ that catches
// everyone, and the reason this returns a number rather than making callers
// parse the rule.
int OffsetSeconds(time_t when = 0);

// Whether daylight saving is in force at `when`.
bool IsDaylightSaving(time_t when = 0);

}  // namespace tz
