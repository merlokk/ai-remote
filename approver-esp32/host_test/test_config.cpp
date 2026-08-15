// `config.json` (CLAUDE.md §10.15), against a real filesystem in a temp
// directory.
//
// §10.15 ends with a list of the tests it wants — defaults parse, every field
// missing, a truncated file and a non-JSON file both ending in a restore, a
// restore leaving `registration.json` untouched, and the two shipped files
// having the same shape. All of them are here, plus the one that section spends
// the most words on and that no other tier can reach: **the write that is not
// allowed to half-happen**, and the boot-time recovery that closes the window
// SPIFFS forces it to leave open.
//
// The filesystem is real (`fakes/fake_storage.h` argues why), and it is real
// in the way that matters: **Windows `rename()` refuses to replace an existing
// file, exactly as SPIFFS does**. The measured EIO quirk §10.15 records is
// therefore reproduced by the host rather than modelled.

#include <cstring>

#include "config.h"
#include "fake_platform.h"
#include "fake_storage.h"
#include "timezone.h"
#include "unity.h"

namespace {

// A minimal but complete file, so a test can change one thing about it.
constexpr const char *kGoodJson = R"({
  "wifi": { "active": true, "networks": [ { "ssid": "desk", "password": "hunter2" } ] },
  "nats": { "url": "nats://10.0.0.9:4222" },
  "time": { "zone": "Europe/Kyiv", "posix": "EET-2EEST,M3.5.0/3,M10.5.0/4",
            "sntp": "time.google.com" },
  "display": { "brightness": 55, "dimSeconds": 15, "blankSeconds": 60 },
  "audio": { "volume": 45 }
})";

void Fresh() {
    fake::MountStorage();
}

// The state a booted device is in: both shipped files present.
void FreshWithDefaults() {
    Fresh();
    TEST_ASSERT_TRUE_MESSAGE(fake::PutRepoFile("config.init.json"),
                             "spiffs_image/config.init.json not found — check SPIFFS_IMAGE_DIR");
}

}  // namespace

// --- The files this firmware actually ships ------------------------------

void test_config_the_shipped_defaults_parse(void) {
    // **The committed file, not a fixture** — so this fails if
    // `spiffs_image/config.init.json` is edited into something the parser does
    // not accept, which is the only way that mistake is ever caught before a
    // flash.
    FreshWithDefaults();
    TEST_ASSERT_TRUE(fake::PutRepoFile("config.json"));

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    TEST_ASSERT_TRUE(config::Loaded());
    TEST_ASSERT_TRUE(config::Get().audio.volume_percent <= 100);
    TEST_ASSERT_TRUE(config::Get().display.brightness <= 100);
    TEST_ASSERT_TRUE(config::Get().time.posix[0] != '\0');
}

void test_config_the_two_shipped_files_have_the_same_shape(void) {
    // §10.15 asks for exactly this, and names why: the two drift apart, and a
    // restore then changes settings nobody meant to change. Parse each and
    // compare every field the struct has.
    FreshWithDefaults();
    TEST_ASSERT_TRUE(fake::PutRepoFile("config.json"));
    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    const config::Data live = config::Get();

    // `Restore` reads `config.init.json` over the top.
    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Restore());
    const config::Data defaults = config::Get();

    // Not a memcmp: the *values* are allowed to differ (a shipped `config.json`
    // may carry an operator's settings). What must match is that both files
    // populate the same fields — asserted as "nothing that is set in one is
    // empty in the other".
    TEST_ASSERT_EQUAL_INT(live.nats.url[0] == '\0', defaults.nats.url[0] == '\0');
    TEST_ASSERT_EQUAL_INT(live.time.zone[0] == '\0', defaults.time.zone[0] == '\0');
    TEST_ASSERT_EQUAL_INT(live.time.posix[0] == '\0', defaults.time.posix[0] == '\0');
    TEST_ASSERT_EQUAL_INT(live.time.sntp_server[0] == '\0',
                          defaults.time.sntp_server[0] == '\0');
}

void test_config_the_committed_file_carries_a_placeholder_password(void) {
    // §10.15: "a real key committed once is a real key in the history".
    // `CHANGEME` is what belongs there, and this is the test that notices the
    // day somebody flashes their own network and commits the result.
    FreshWithDefaults();
    TEST_ASSERT_TRUE(fake::PutRepoFile("config.json"));
    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());

    const config::Wifi &wifi = config::Get().wifi;
    // Not `for (i < count)` alone: with no networks that loop passes by doing
    // nothing, and the day someone commits their own file *with* a real key is
    // exactly the day the count is non-zero.
    TEST_ASSERT_TRUE_MESSAGE(wifi.network_count > 0,
                             "spiffs_image/config.json has no network to check the placeholder of");
    for (uint8_t i = 0; i < wifi.network_count; ++i) {
        TEST_ASSERT_EQUAL_STRING("CHANGEME", wifi.networks[i].password);
        TEST_ASSERT_EQUAL_STRING("YOUR_SSID", wifi.networks[i].ssid);
    }
}

// --- Parsing -------------------------------------------------------------

void test_config_reads_every_field(void) {
    FreshWithDefaults();
    fake::PutFile("config.json", kGoodJson);

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    const config::Data &c = config::Get();

    TEST_ASSERT_TRUE(c.wifi.active);
    TEST_ASSERT_EQUAL_UINT8(1, c.wifi.network_count);
    TEST_ASSERT_EQUAL_STRING("desk", c.wifi.networks[0].ssid);
    TEST_ASSERT_EQUAL_STRING("hunter2", c.wifi.networks[0].password);
    TEST_ASSERT_EQUAL_STRING("nats://10.0.0.9:4222", c.nats.url);
    TEST_ASSERT_EQUAL_STRING("Europe/Kyiv", c.time.zone);
    TEST_ASSERT_EQUAL_STRING("EET-2EEST,M3.5.0/3,M10.5.0/4", c.time.posix);
    TEST_ASSERT_EQUAL_STRING("time.google.com", c.time.sntp_server);
    TEST_ASSERT_EQUAL_UINT8(55, c.display.brightness);
    TEST_ASSERT_EQUAL_UINT16(15, c.display.dim_seconds);
    TEST_ASSERT_EQUAL_UINT16(60, c.display.blank_seconds);
    TEST_ASSERT_EQUAL_UINT8(45, c.audio.volume_percent);
}

void test_config_every_field_missing_falls_back_field_by_field(void) {
    // §10.15's "every field missing" case. `{}` is valid JSON, so this is not
    // the restore path — it parses, and each absent field keeps its default.
    // A parser that zeroed instead would give a device with brightness 0 and
    // no NATS URL, which looks like a working config.
    FreshWithDefaults();
    fake::PutFile("config.json", "{}");

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    config::Data expected = {};
    config::FillDefaults(&expected);
    const config::Data &c = config::Get();

    TEST_ASSERT_EQUAL_STRING(expected.nats.url, c.nats.url);
    TEST_ASSERT_EQUAL_STRING(expected.time.posix, c.time.posix);
    TEST_ASSERT_EQUAL_STRING(expected.time.sntp_server, c.time.sntp_server);
    TEST_ASSERT_EQUAL_UINT8(expected.display.brightness, c.display.brightness);
    TEST_ASSERT_EQUAL_UINT8(expected.audio.volume_percent, c.audio.volume_percent);
    TEST_ASSERT_EQUAL_UINT8(0, c.wifi.network_count);
}

void test_config_a_partial_file_keeps_the_defaults_for_the_rest(void) {
    FreshWithDefaults();
    fake::PutFile("config.json", R"({"audio":{"volume":12}})");

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    config::Data expected = {};
    config::FillDefaults(&expected);

    TEST_ASSERT_EQUAL_UINT8(12, config::Get().audio.volume_percent);
    TEST_ASSERT_EQUAL_UINT8(expected.display.brightness, config::Get().display.brightness);
}

void test_config_ignores_networks_past_the_fixed_capacity(void) {
    // §10.14.1: full is a designed state. Five entries into four slots keeps
    // the first four rather than overflowing or refusing the whole file.
    FreshWithDefaults();
    fake::PutFile("config.json", R"({"wifi":{"networks":[
        {"ssid":"a"},{"ssid":"b"},{"ssid":"c"},{"ssid":"d"},{"ssid":"e"}]}})");

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    TEST_ASSERT_EQUAL_UINT8(config::kMaxNetworks, config::Get().wifi.network_count);
    TEST_ASSERT_EQUAL_STRING("a", config::Get().wifi.networks[0].ssid);
    TEST_ASSERT_EQUAL_STRING("d", config::Get().wifi.networks[3].ssid);
}

void test_config_skips_a_network_with_no_ssid(void) {
    FreshWithDefaults();
    fake::PutFile("config.json",
                  R"({"wifi":{"networks":[{"password":"x"},{"ssid":"real","password":"y"}]}})");

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    TEST_ASSERT_EQUAL_UINT8(1, config::Get().wifi.network_count);
    TEST_ASSERT_EQUAL_STRING("real", config::Get().wifi.networks[0].ssid);
}

// --- The restore path (§10.15) -------------------------------------------

void test_config_a_missing_file_is_restored(void) {
    FreshWithDefaults();  // no config.json at all

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    TEST_ASSERT_TRUE(config::Loaded());
    // Restored means *written*, not just defaulted in memory — the next boot
    // must find a file.
    TEST_ASSERT_TRUE(fake::FileExists("config.json"));
}

void test_config_a_file_that_is_not_json_is_restored(void) {
    FreshWithDefaults();
    fake::PutFile("config.json", "this is not json, it is a sentence");

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    TEST_ASSERT_TRUE(config::Loaded());

    char back[512] = {};
    TEST_ASSERT_TRUE(fake::GetFile("config.json", back, sizeof(back)));
    TEST_ASSERT_NULL(std::strstr(back, "sentence"));
}

void test_config_a_truncated_file_is_restored(void) {
    FreshWithDefaults();
    fake::PutFile("config.json", R"({"audio":{"volume":45)");

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    TEST_ASSERT_TRUE(config::Loaded());
    TEST_ASSERT_TRUE(fake::FileExists("config.json"));
}

void test_config_an_oversized_file_is_restored(void) {
    // The cap is checked before parsing (§10.15). Untrusted-shaped input on a
    // device that has to keep booting — §10.10's rule, applied to a file.
    FreshWithDefaults();

    static char huge[config::kMaxFileSize + 512];
    std::memset(huge, ' ', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    huge[0] = '{';
    huge[1] = '}';
    fake::PutFile("config.json", huge);

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    TEST_ASSERT_TRUE(config::Loaded());
}

void test_config_a_restore_does_not_touch_the_registration(void) {
    // **The reason §10.15 keeps them in two files.** A device that comes back
    // on default settings is a minute's work; one that comes back unregistered
    // needs a token minted on the host and typed over USB.
    FreshWithDefaults();
    fake::PutFile("registration.json", R"({"key_id":"approver-esp32","server_key":"AAAA"})");
    fake::PutFile("config.json", "not json");

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());

    char back[256] = {};
    TEST_ASSERT_TRUE(fake::GetFile("registration.json", back, sizeof(back)));
    TEST_ASSERT_NOT_NULL(std::strstr(back, "approver-esp32"));
    TEST_ASSERT_NOT_NULL(std::strstr(back, "AAAA"));
}

void test_config_reload_does_not_restore_a_bad_file(void) {
    // The header states this and it is the subtle one: `Reload` is somebody
    // asking what the file says, and answering by overwriting it would destroy
    // the thing they were asking about.
    FreshWithDefaults();
    fake::PutFile("config.json", kGoodJson);
    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());

    fake::PutFile("config.json", "broken");
    TEST_ASSERT_NOT_EQUAL(ESP_OK, config::Reload());

    char back[64] = {};
    TEST_ASSERT_TRUE(fake::GetFile("config.json", back, sizeof(back)));
    TEST_ASSERT_EQUAL_STRING("broken", back);
}

void test_config_runs_on_built_in_defaults_with_no_filesystem(void) {
    fake::MountStorage();
    fake::UnmountStorage();

    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, config::Init());
    // And the values are still sane rather than zeros — a device that cannot
    // mount its storage should come up far enough to say so (§10.10).
    config::Data expected = {};
    config::FillDefaults(&expected);
    TEST_ASSERT_EQUAL_UINT8(expected.display.brightness, config::Get().display.brightness);
    TEST_ASSERT_EQUAL_STRING(expected.nats.url, config::Get().nats.url);
}

void test_config_survives_defaults_that_are_missing_too(void) {
    // A missing `config.init.json` is a *build* error, not a runtime state —
    // but the device still has to boot and say so.
    Fresh();  // neither file

    TEST_ASSERT_NOT_EQUAL(ESP_OK, config::Init());
    TEST_ASSERT_TRUE(config::Loaded());
    config::Data expected = {};
    config::FillDefaults(&expected);
    TEST_ASSERT_EQUAL_UINT8(expected.audio.volume_percent, config::Get().audio.volume_percent);
}

// --- Writing, and the window SPIFFS forces open --------------------------

void test_config_a_saved_setting_round_trips(void) {
    // §10.15's worked example: the volume is the first setting that survives a
    // reboot, and this is that path with the reboot replaced by a reload.
    FreshWithDefaults();
    fake::PutFile("config.json", kGoodJson);
    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());

    config::Get().audio.volume_percent = 33;
    config::Get().display.brightness = 21;
    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Save());

    config::Get().audio.volume_percent = 99;  // scribble over it in memory
    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Reload());
    TEST_ASSERT_EQUAL_UINT8(33, config::Get().audio.volume_percent);
    TEST_ASSERT_EQUAL_UINT8(21, config::Get().display.brightness);
}

void test_config_a_save_leaves_no_temp_file_behind(void) {
    FreshWithDefaults();
    fake::PutFile("config.json", kGoodJson);
    config::Init();

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Save());
    TEST_ASSERT_TRUE(fake::FileExists("config.json"));
    TEST_ASSERT_FALSE(fake::FileExists("config.json.new"));
}

void test_config_unknown_fields_are_lost_on_the_next_write(void) {
    // The honest behaviour of a fixed struct, stated in §10.15 so it does not
    // surprise anyone: a file written by a newer firmware does not survive a
    // downgrade. Pinned so it stays a known cost rather than becoming a bug
    // report.
    FreshWithDefaults();
    fake::PutFile("config.json", R"({"audio":{"volume":45},"future":{"thing":true}})");
    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Save());
    char back[1024] = {};
    TEST_ASSERT_TRUE(fake::GetFile("config.json", back, sizeof(back)));
    TEST_ASSERT_NULL(std::strstr(back, "future"));
    TEST_ASSERT_NOT_NULL(std::strstr(back, "volume"));
}

void test_config_a_leftover_temp_file_is_dropped(void) {
    // The crash landed *before* the old file was removed, so `config.json` is
    // intact and the temp is rubbish. Dropping it is the recovery.
    FreshWithDefaults();
    fake::PutFile("config.json", kGoodJson);
    fake::PutFile("config.json.new", R"({"audio":{"volume":7}})");

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    TEST_ASSERT_FALSE(fake::FileExists("config.json.new"));
    // The good config won, not the leftover.
    TEST_ASSERT_EQUAL_UINT8(45, config::Get().audio.volume_percent);
}

void test_config_a_write_interrupted_in_the_window_is_finished(void) {
    // **The case §10.15 spends the most words on.** SPIFFS will not rename onto
    // an existing name, so the write has to remove the old file first — which
    // leaves a real window where `config.json` is gone and a *complete*
    // `config.json.new` is not yet called anything. A crash there must be
    // finished, not treated as a corrupt config: restoring the defaults would
    // throw away a good file to fix a naming problem.
    FreshWithDefaults();
    fake::PutFile("config.json.new", R"({"audio":{"volume":7},"display":{"brightness":11}})");
    TEST_ASSERT_FALSE(fake::FileExists("config.json"));

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());

    TEST_ASSERT_TRUE(fake::FileExists("config.json"));
    TEST_ASSERT_FALSE(fake::FileExists("config.json.new"));
    TEST_ASSERT_EQUAL_UINT8(7, config::Get().audio.volume_percent);
    TEST_ASSERT_EQUAL_UINT8(11, config::Get().display.brightness);
}

void test_config_a_restore_is_written_through_the_same_atomic_path(void) {
    FreshWithDefaults();
    fake::PutFile("config.json", "not json");

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    TEST_ASSERT_TRUE(fake::FileExists("config.json"));
    TEST_ASSERT_FALSE(fake::FileExists("config.json.new"));
}

// --- The zone pair (§10.8.2) ---------------------------------------------

void test_config_a_named_zone_fills_in_its_posix_rule(void) {
    // §10.8.2's pair: `zone` is what a person reads, `posix` is what libc is
    // given. A file that names a zone without the rule must still end up with
    // a rule, or libc silently reads the misspelling as UTC.
    FreshWithDefaults();
    fake::PutFile("config.json", R"({"time":{"zone":"Europe/Kyiv"}})");

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    TEST_ASSERT_EQUAL_STRING("Europe/Kyiv", config::Get().time.zone);
    TEST_ASSERT_EQUAL_STRING(tz::Lookup("Europe/Kyiv"), config::Get().time.posix);
}

void test_config_an_unknown_zone_does_not_silently_become_utc(void) {
    // The failure §10.8.2 names: libc reads a misspelled zone as UTC and says
    // nothing. Whatever this driver does with `Atlantis/Capital`, it must not
    // be to store it as a working rule.
    FreshWithDefaults();
    fake::PutFile("config.json", R"({"time":{"zone":"Atlantis/Capital"}})");

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    TEST_ASSERT_NULL(tz::Lookup("Atlantis/Capital"));

    config::Data defaults = {};
    config::FillDefaults(&defaults);
    TEST_ASSERT_EQUAL_STRING(defaults.time.posix, config::Get().time.posix);
}

void RegisterConfigTests(void) {
    RUN_TEST(test_config_the_shipped_defaults_parse);
    RUN_TEST(test_config_the_two_shipped_files_have_the_same_shape);
    RUN_TEST(test_config_the_committed_file_carries_a_placeholder_password);

    RUN_TEST(test_config_reads_every_field);
    RUN_TEST(test_config_every_field_missing_falls_back_field_by_field);
    RUN_TEST(test_config_a_partial_file_keeps_the_defaults_for_the_rest);
    RUN_TEST(test_config_ignores_networks_past_the_fixed_capacity);
    RUN_TEST(test_config_skips_a_network_with_no_ssid);

    RUN_TEST(test_config_a_missing_file_is_restored);
    RUN_TEST(test_config_a_file_that_is_not_json_is_restored);
    RUN_TEST(test_config_a_truncated_file_is_restored);
    RUN_TEST(test_config_an_oversized_file_is_restored);
    RUN_TEST(test_config_a_restore_does_not_touch_the_registration);
    RUN_TEST(test_config_reload_does_not_restore_a_bad_file);
    RUN_TEST(test_config_runs_on_built_in_defaults_with_no_filesystem);
    RUN_TEST(test_config_survives_defaults_that_are_missing_too);

    RUN_TEST(test_config_a_saved_setting_round_trips);
    RUN_TEST(test_config_a_save_leaves_no_temp_file_behind);
    RUN_TEST(test_config_unknown_fields_are_lost_on_the_next_write);
    RUN_TEST(test_config_a_leftover_temp_file_is_dropped);
    RUN_TEST(test_config_a_write_interrupted_in_the_window_is_finished);
    RUN_TEST(test_config_a_restore_is_written_through_the_same_atomic_path);

    RUN_TEST(test_config_a_named_zone_fills_in_its_posix_rule);
    RUN_TEST(test_config_an_unknown_zone_does_not_silently_become_utc);
}
