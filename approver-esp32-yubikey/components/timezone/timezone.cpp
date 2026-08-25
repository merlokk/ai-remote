#include "timezone.h"

#include <strings.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_log.h"

namespace tz {

namespace {

constexpr const char *TAG = "tz";

// The table. Curated rather than complete: these are the zones a device on this
// desk plausibly sits in, and every rule below is one whose transitions are
// settled at the time of writing. A zone that is missing is not a blocker —
// a raw POSIX string is always accepted (see the header).
//
// The groups repeat their rules on purpose. `CET-1CEST,M3.5.0,M10.5.0/3` looks
// copy-pasted forty times because it *is* the same rule forty times: the EU
// switches together, and collapsing that into a shared constant would hide the
// day one country leaves.
constexpr Zone kZones[] = {
    {"UTC", "UTC0"},

    // The EU's three zones by their own names, no city attached. tzdata carries
    // these as zones in their own right, and they are what someone who knows
    // they are "on Eastern European Time" will type. Same rules as the cities
    // below — the EU switches together — so `EET` and `Europe/Kyiv` are the
    // same clock with different labels, and which one is stored is the
    // operator's choice about how they think of where they are.
    {"WET", "WET0WEST,M3.5.0/1,M10.5.0"},   // Western European Time, UTC+0/+1
    {"CET", "CET-1CEST,M3.5.0,M10.5.0/3"},  // Central European Time, UTC+1/+2
    {"EET", "EET-2EEST,M3.5.0/3,M10.5.0/4"},  // Eastern European Time, UTC+2/+3

    // Western Europe
    {"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Dublin", "GMT0IST,M3.5.0/1,M10.5.0"},
    {"Europe/Lisbon", "WET0WEST,M3.5.0/1,M10.5.0"},

    // Central Europe — one rule, many countries
    {"Europe/Amsterdam", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Brussels", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Budapest", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Copenhagen", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Madrid", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Oslo", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Prague", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Rome", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Stockholm", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Vienna", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Warsaw", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Zurich", "CET-1CEST,M3.5.0,M10.5.0/3"},

    // Eastern Europe. The EU switches at 01:00 UTC, which for EET is 03:00
    // local in spring and 04:00 in autumn — hence the explicit hours.
    {"Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Bucharest", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Chisinau", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Kyiv", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Riga", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Sofia", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Tallinn", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Vilnius", "EET-2EEST,M3.5.0/3,M10.5.0/4"},

    // No daylight saving
    {"Europe/Istanbul", "<+03>-3"},
    {"Europe/Minsk", "<+03>-3"},
    {"Europe/Moscow", "MSK-3"},

    // Asia
    {"Asia/Almaty", "<+05>-5"},
    {"Asia/Baku", "<+04>-4"},
    {"Asia/Bangkok", "<+07>-7"},
    {"Asia/Dubai", "<+04>-4"},
    {"Asia/Hong_Kong", "HKT-8"},
    {"Asia/Jerusalem", "IST-2IDT,M3.4.4/26,M10.5.0"},
    {"Asia/Karachi", "PKT-5"},
    {"Asia/Kolkata", "IST-5:30"},
    // Cyprus is in the EU, so it switches on the EU's dates rather than on any
    // Asian rule — the same line as Kyiv and Athens above. tzdata files it
    // under Asia and links Europe/Nicosia to it; both names work here.
    {"Asia/Nicosia", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Asia/Seoul", "KST-9"},
    {"Asia/Shanghai", "CST-8"},
    {"Asia/Singapore", "<+08>-8"},
    {"Asia/Tashkent", "<+05>-5"},
    {"Asia/Tbilisi", "<+04>-4"},
    {"Asia/Tokyo", "JST-9"},
    {"Asia/Yerevan", "<+04>-4"},

    // Africa
    {"Africa/Johannesburg", "SAST-2"},
    {"Africa/Lagos", "WAT-1"},
    {"Africa/Nairobi", "EAT-3"},

    // Americas. The US and Canada switch on the second Sunday in March and the
    // first in November — a different rule from Europe's, and the reason a
    // device that crosses the Atlantic needs its zone changed rather than its
    // clock.
    {"America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"America/Bogota", "<-05>5"},
    {"America/Buenos_Aires", "<-03>3"},
    {"America/Chicago", "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Denver", "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Lima", "<-05>5"},
    {"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Mexico_City", "CST6"},
    {"America/New_York", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Phoenix", "MST7"},
    {"America/Sao_Paulo", "<-03>3"},
    {"America/Toronto", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Vancouver", "PST8PDT,M3.2.0,M11.1.0"},
    {"Pacific/Honolulu", "HST10"},

    // Southern hemisphere: summer time in the other half of the year.
    {"Australia/Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"Australia/Brisbane", "AEST-10"},
    {"Australia/Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia/Perth", "AWST-8"},
    {"Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
};

// Names that were renamed or are simply what people type. Kept separate from
// the table so the listing shows one name per zone.
struct Alias {
    const char *from;
    const char *to;
};

constexpr Alias kAliases[] = {
    {"Europe/Kiev", "Europe/Kyiv"},
    {"Europe/Nicosia", "Asia/Nicosia"},
    {"Asia/Calcutta", "Asia/Kolkata"},
    {"Etc/UTC", "UTC"},
    {"GMT", "UTC"},
    {"America/Argentina/Buenos_Aires", "America/Buenos_Aires"},
};

char current[kMaxPosixLength] = "UTC0";

bool EqualsIgnoringCase(const char *a, const char *b) {
    return strcasecmp(a, b) == 0;
}

}  // namespace

size_t Count() { return sizeof(kZones) / sizeof(kZones[0]); }

const Zone &At(size_t index) { return kZones[index < Count() ? index : 0]; }

const char *Lookup(const char *name) {
    if (name == nullptr || name[0] == '\0') {
        return nullptr;
    }

    for (const Alias &alias : kAliases) {
        if (EqualsIgnoringCase(name, alias.from)) {
            name = alias.to;
            break;
        }
    }

    for (const Zone &zone : kZones) {
        if (EqualsIgnoringCase(name, zone.name)) {
            return zone.posix;
        }
    }
    return nullptr;
}

const char *NameFor(const char *posix) {
    if (posix == nullptr) {
        return nullptr;
    }
    // First match wins, and the table's order is what makes that answer decent:
    // `WET`/`CET`/`EET` sit at the top, so a rule shared by a dozen cities is
    // named after the zone family rather than after whichever city happened to
    // be listed first.
    for (const Zone &zone : kZones) {
        if (strcmp(posix, zone.posix) == 0) {
            return zone.name;
        }
    }
    return nullptr;
}

bool LooksLikePosix(const char *value) {
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    // **A slash means different things in the two, and where it sits is what
    // separates them.** This used to reject any string containing one, on the
    // grounds that a zone name has a slash and a rule does not — which is
    // wrong, and wrong about most of the table: a transition time is written
    // `M3.5.0/3`, so `EET-2EEST,M3.5.0/3,M10.5.0/4` was refused as "not a
    // POSIX rule". §10.8.2 promises a raw rule is accepted anywhere a name is,
    // and it was not. Found by the host tests, which asked whether the table's
    // own rules pass this check.
    //
    // In a rule every slash follows a digit; in a name it separates letters.
    // That is the actual distinction.
    for (const char *p = strchr(value, '/'); p != nullptr; p = strchr(p + 1, '/')) {
        if (p == value || *(p - 1) < '0' || *(p - 1) > '9') {
            return false;
        }
    }
    // The rule needs an abbreviation and an offset: at least three characters
    // of name, then a sign or a digit somewhere. "UTC0" is the shortest real
    // one.
    if (strlen(value) < 4) {
        return false;
    }
    for (const char *p = value; *p != '\0'; ++p) {
        if (*p >= '0' && *p <= '9') {
            return true;
        }
    }
    return false;
}

esp_err_t Apply(const char *posix) {
    if (posix == nullptr || posix[0] == '\0' || strlen(posix) >= sizeof(current)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (setenv("TZ", posix, 1) != 0) {
        return ESP_FAIL;
    }
    tzset();
    snprintf(current, sizeof(current), "%s", posix);
    // The rule and nothing else. A name would have to come from `NameFor`,
    // which answers with the *first* zone sharing that rule — so applying
    // Kyiv's rule logged "Europe/Athens", which is true and useless. The caller
    // knows which name it asked for; this layer does not.
    ESP_LOGI(TAG, "TZ=%s", posix);
    return ESP_OK;
}

const char *Current() { return current; }

int OffsetSeconds(time_t when) {
    if (when == 0) {
        when = time(nullptr);
    }

    // Break `when` down in local terms, then ask what that breakdown would be
    // if it were UTC. The difference is the offset, DST included, because
    // `localtime_r` already applied it.
    //
    // The house firmware (§10.14.4) does this the other way round — UTC
    // breakdown through `mktime`, with `tm_isdst` copied across so `mktime`
    // does not guess — and that works, but it answers with the *opposite* sign
    // and needs the isdst carry to be right at all. `timegm` is the same
    // arithmetic without either trap.
    struct tm local_time = {};
    localtime_r(&when, &local_time);
    const time_t local_as_utc = timegm(&local_time);
    return static_cast<int>(local_as_utc - when);
}

bool IsDaylightSaving(time_t when) {
    if (when == 0) {
        when = time(nullptr);
    }
    struct tm local_time = {};
    localtime_r(&when, &local_time);
    return local_time.tm_isdst > 0;
}

}  // namespace tz
