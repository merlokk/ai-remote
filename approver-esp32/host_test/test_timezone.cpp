// Named time zones (CLAUDE.md §10.8.2).
//
// The failure this component exists to prevent is a quiet one: **libc reads a
// misspelled zone as UTC and says nothing.** So the most important test here is
// that `Lookup` answers with a null rather than with something plausible, and
// that `LooksLikePosix` can tell `Europe/Kyev` from `EET-2EEST,…`.
//
// **One limit is worth stating up front**, because it decides which of these
// tests exist. The Windows CRT's `TZ` parser understands the simple
// `EST5EDT` form and *not* the `,M3.5.0/3,M10.5.0/4` transition rules this
// table is made of. So the table, the lookups and what gets handed to libc are
// all tested here; what wall-clock time comes out of a rule is not, and cannot
// be from this tier. §10.8.2 already says the transition dates are only
// verifiable on the device — this is the same boundary, met from the other
// side.

#include <cstring>

#include "fake_platform.h"
#include "timezone.h"
#include "unity.h"

// --- The table -----------------------------------------------------------

void test_tz_the_table_is_not_empty_and_every_row_is_complete(void) {
    // A row with a null name or rule would be a crash in `Lookup`, and the
    // table is hand-edited every time a country moves its dates.
    TEST_ASSERT_TRUE(tz::Count() > 0);
    for (size_t i = 0; i < tz::Count(); ++i) {
        const tz::Zone &zone = tz::At(i);
        TEST_ASSERT_NOT_NULL(zone.name);
        TEST_ASSERT_NOT_NULL(zone.posix);
        TEST_ASSERT_TRUE(zone.name[0] != '\0');
        TEST_ASSERT_TRUE(zone.posix[0] != '\0');
        // The header promises callers can size buffers off these.
        TEST_ASSERT_TRUE(std::strlen(zone.name) < tz::kMaxNameLength);
        TEST_ASSERT_TRUE(std::strlen(zone.posix) < tz::kMaxPosixLength);
    }
}

void test_tz_every_name_in_the_table_looks_itself_up(void) {
    // The table and the lookup cannot disagree — but a row added with a
    // trailing space, or a duplicate name shadowing another, would make them.
    for (size_t i = 0; i < tz::Count(); ++i) {
        const tz::Zone &zone = tz::At(i);
        const char *found = tz::Lookup(zone.name);
        TEST_ASSERT_NOT_NULL_MESSAGE(found, zone.name);
        TEST_ASSERT_EQUAL_STRING(zone.posix, found);
    }
}

void test_tz_every_rule_in_the_table_is_a_posix_rule(void) {
    // `LooksLikePosix` is what `config set tz` uses to decide whether a typed
    // string is a rule or a misspelled name. If the table's own rules did not
    // pass it, the check would be refusing the values it ships with.
    for (size_t i = 0; i < tz::Count(); ++i) {
        TEST_ASSERT_TRUE_MESSAGE(tz::LooksLikePosix(tz::At(i).posix), tz::At(i).posix);
    }
}

void test_tz_no_name_in_the_table_looks_like_a_rule(void) {
    // The other direction, and the one that matters for the console: a zone
    // name must never be mistaken for a rule and stored raw.
    for (size_t i = 0; i < tz::Count(); ++i) {
        const char *name = tz::At(i).name;
        // `UTC`, `WET`, `CET`, `EET` are names with no digits and no slash, so
        // they fail the check for the right reason; the cities fail on the
        // slash. Either way the answer has to be no.
        TEST_ASSERT_FALSE_MESSAGE(tz::LooksLikePosix(name), name);
    }
}

// --- Looking a zone up ---------------------------------------------------

void test_tz_finds_a_zone_by_name(void) {
    TEST_ASSERT_EQUAL_STRING("EET-2EEST,M3.5.0/3,M10.5.0/4", tz::Lookup("Europe/Kyiv"));
    TEST_ASSERT_EQUAL_STRING("UTC0", tz::Lookup("UTC"));
}

void test_tz_lookup_ignores_case(void) {
    // Somebody typing into a console gets `europe/kyiv` as often as not.
    TEST_ASSERT_EQUAL_STRING(tz::Lookup("Europe/Kyiv"), tz::Lookup("europe/kyiv"));
    TEST_ASSERT_EQUAL_STRING(tz::Lookup("Europe/Kyiv"), tz::Lookup("EUROPE/KYIV"));
}

void test_tz_follows_the_aliases_people_actually_type(void) {
    // Both of these are what the world called the zone until recently, and both
    // are still what half the tooling emits.
    TEST_ASSERT_EQUAL_STRING(tz::Lookup("Europe/Kyiv"), tz::Lookup("Europe/Kiev"));
    TEST_ASSERT_EQUAL_STRING(tz::Lookup("Asia/Kolkata"), tz::Lookup("Asia/Calcutta"));
}

void test_tz_an_unknown_zone_is_a_null_and_not_a_guess(void) {
    // **The failure §10.8.2 names.** A `Lookup` that fell back to UTC would put
    // a device an hour or three wrong with nothing on screen to say so.
    TEST_ASSERT_NULL(tz::Lookup("Atlantis/Capital"));
    TEST_ASSERT_NULL(tz::Lookup("Europe/Kyev"));  // one letter out
    TEST_ASSERT_NULL(tz::Lookup(""));
    TEST_ASSERT_NULL(tz::Lookup(nullptr));
}

// --- The reverse, and its caveat -----------------------------------------

void test_tz_names_a_shared_rule_after_its_family(void) {
    // §10.8.2: one rule serves every EU country on that offset, so the answer
    // has to be "a zone that switches like this" rather than a city picked by
    // table order. `WET`/`CET`/`EET` sit first precisely so this is the answer.
    TEST_ASSERT_EQUAL_STRING("EET", tz::NameFor(tz::Lookup("Europe/Kyiv")));
    TEST_ASSERT_EQUAL_STRING("CET", tz::NameFor(tz::Lookup("CET")));
    TEST_ASSERT_EQUAL_STRING("UTC", tz::NameFor("UTC0"));
}

void test_tz_an_unknown_rule_has_no_name(void) {
    // Which is what makes a raw rule get stored as `Custom` instead of being
    // labelled with somebody else's zone.
    TEST_ASSERT_NULL(tz::NameFor("XYZ-9XYZDST,M1.1.0,M2.2.0"));
    TEST_ASSERT_NULL(tz::NameFor(nullptr));
}

// --- Telling a rule from a misspelling -----------------------------------

void test_tz_a_rule_is_recognised(void) {
    TEST_ASSERT_TRUE(tz::LooksLikePosix("UTC0"));
    TEST_ASSERT_TRUE(tz::LooksLikePosix("EET-2EEST,M3.5.0/3,M10.5.0/4"));
    TEST_ASSERT_TRUE(tz::LooksLikePosix("IST-5:30"));
}

void test_tz_a_zone_name_is_not_a_rule(void) {
    // A slash settles it, and no amount of parsing does better.
    TEST_ASSERT_FALSE(tz::LooksLikePosix("Europe/Kyiv"));
    TEST_ASSERT_FALSE(tz::LooksLikePosix("Europe/Kyev"));
}

void test_tz_something_too_short_to_be_either_is_refused(void) {
    // `UTC0` is the shortest real rule, so anything under four characters is
    // not one — and a bare `EET` is a name this table already knows, which
    // means it should reach `Lookup` rather than be stored raw.
    TEST_ASSERT_FALSE(tz::LooksLikePosix("EET"));
    TEST_ASSERT_FALSE(tz::LooksLikePosix("Z"));
    TEST_ASSERT_FALSE(tz::LooksLikePosix(""));
    TEST_ASSERT_FALSE(tz::LooksLikePosix(nullptr));
}

void test_tz_a_name_shaped_string_with_no_offset_is_refused(void) {
    // The other half of the check: an abbreviation with no number in it is not
    // a rule, however long it is. `Antarctica` has no digits and no slash.
    TEST_ASSERT_FALSE(tz::LooksLikePosix("Antarctica"));
    TEST_ASSERT_FALSE(tz::LooksLikePosix("NOTAZONE"));
}

// --- Applying it ---------------------------------------------------------

void test_tz_apply_stores_the_rule_it_was_given(void) {
    TEST_ASSERT_EQUAL_INT(ESP_OK, tz::Apply("UTC0"));
    TEST_ASSERT_EQUAL_STRING("UTC0", tz::Current());

    const char *kyiv = tz::Lookup("Europe/Kyiv");
    TEST_ASSERT_EQUAL_INT(ESP_OK, tz::Apply(kyiv));
    TEST_ASSERT_EQUAL_STRING(kyiv, tz::Current());
}

void test_tz_apply_refuses_nothing_and_refuses_too_much(void) {
    TEST_ASSERT_EQUAL_INT(ESP_OK, tz::Apply("UTC0"));

    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, tz::Apply(nullptr));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, tz::Apply(""));

    char too_long[tz::kMaxPosixLength + 40];
    std::memset(too_long, 'A', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, tz::Apply(too_long));

    // And a refused apply leaves the last good rule in place rather than a
    // half-written one.
    TEST_ASSERT_EQUAL_STRING("UTC0", tz::Current());
}

void test_tz_utc_has_no_offset_and_no_daylight_saving(void) {
    // The one arithmetic assertion this tier can make honestly: `UTC0` is a
    // fixed-offset rule that every libc parses the same way. Anything with an
    // `M` transition is the device's to verify — see the note at the top.
    TEST_ASSERT_EQUAL_INT(ESP_OK, tz::Apply("UTC0"));
    TEST_ASSERT_EQUAL_INT(0, tz::OffsetSeconds(0));
    TEST_ASSERT_FALSE(tz::IsDaylightSaving(0));
}

void test_tz_a_fixed_offset_reads_back_with_the_sign_flipped(void) {
    // **The trap POSIX sets for everyone**: inside a TZ string the offset is
    // how far *west* you are, so `EET-2` is UTC**+**2. `OffsetSeconds` returns
    // the ordinary convention — positive east — which is the whole reason it
    // exists rather than callers parsing the rule.
    TEST_ASSERT_EQUAL_INT(ESP_OK, tz::Apply("EET-2"));
    TEST_ASSERT_EQUAL_INT(2 * 3600, tz::OffsetSeconds(0));

    TEST_ASSERT_EQUAL_INT(ESP_OK, tz::Apply("EST5"));
    TEST_ASSERT_EQUAL_INT(-5 * 3600, tz::OffsetSeconds(0));

    // Put it back, so a later test does not inherit a zone.
    TEST_ASSERT_EQUAL_INT(ESP_OK, tz::Apply("UTC0"));
}

void RegisterTimezoneTests(void) {
    RUN_TEST(test_tz_the_table_is_not_empty_and_every_row_is_complete);
    RUN_TEST(test_tz_every_name_in_the_table_looks_itself_up);
    RUN_TEST(test_tz_every_rule_in_the_table_is_a_posix_rule);
    RUN_TEST(test_tz_no_name_in_the_table_looks_like_a_rule);

    RUN_TEST(test_tz_finds_a_zone_by_name);
    RUN_TEST(test_tz_lookup_ignores_case);
    RUN_TEST(test_tz_follows_the_aliases_people_actually_type);
    RUN_TEST(test_tz_an_unknown_zone_is_a_null_and_not_a_guess);

    RUN_TEST(test_tz_names_a_shared_rule_after_its_family);
    RUN_TEST(test_tz_an_unknown_rule_has_no_name);

    RUN_TEST(test_tz_a_rule_is_recognised);
    RUN_TEST(test_tz_a_zone_name_is_not_a_rule);
    RUN_TEST(test_tz_something_too_short_to_be_either_is_refused);
    RUN_TEST(test_tz_a_name_shaped_string_with_no_offset_is_refused);

    RUN_TEST(test_tz_apply_stores_the_rule_it_was_given);
    RUN_TEST(test_tz_apply_refuses_nothing_and_refuses_too_much);
    RUN_TEST(test_tz_utc_has_no_offset_and_no_daylight_saving);
    RUN_TEST(test_tz_a_fixed_offset_reads_back_with_the_sign_flipped);
}
