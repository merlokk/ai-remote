// What a form on the configuration site may change (CLAUDE.md §10.16), tested
// where it costs nothing (§10.11, host tier).
//
// `web/web_settings.cpp` needs cJSON and `components/config` and no board, the
// same shape `test_config.cpp` has — so the document below is the one the real
// parser reads, not a model of it.
//
// **Nearly every test here is a refusal**, and they are not interchangeable: this
// is the only path in the firmware by which something outside the device can
// change what the device does, so "that field does not exist", "that is not an
// address" and "that is one network too many" each have to arrive as themselves.
// The two that are not refusals are the two that carry the whole design: a
// password that was not retyped is kept, and a refused document changes nothing.

#include "unity.h"
#include "web_settings.h"

#include <cstdio>
#include <cstring>

namespace {

using web::Action;
using web::ApplySettings;
using web::WriteResult;

// A device that already knows two networks with keys on them, an access point
// with a key of its own, and a bus. Every test starts from this, because "keep
// what is there" is only testable against something being there.
config::Data Device() {
    config::Data data = {};
    config::FillDefaults(&data);
    data.wifi.active = true;
    data.wifi.mode = config::WifiMode::kClient;
    data.wifi.network_count = 2;
    std::snprintf(data.wifi.networks[0].ssid, config::kSsidSize, "%s", "home");
    std::snprintf(data.wifi.networks[0].password, config::kPasswordSize, "%s", "homesecret");
    std::snprintf(data.wifi.networks[1].ssid, config::kSsidSize, "%s", "office");
    std::snprintf(data.wifi.networks[1].password, config::kPasswordSize, "%s", "officesecret");
    std::snprintf(data.wifi.ap_ssid, config::kSsidSize, "%s", "approver-esp32");
    std::snprintf(data.wifi.ap_password, config::kPasswordSize, "%s", "apsecret");
    std::snprintf(data.nats.url, config::kUrlSize, "%s", "nats://192.168.11.70:4222");
    return data;
}

web::WriteOutcome Apply(const char *body, config::Data *into) {
    return ApplySettings(body, std::strlen(body), into);
}

// A refusal has to leave the settings exactly as they were — the rule §10.2 keeps
// about the signing bytes. Asserted field by field rather than with a memcmp, so a
// failure names what moved.
void AssertUnchanged(const config::Data &now, const config::Data &before) {
    TEST_ASSERT_EQUAL_UINT8(before.wifi.network_count, now.wifi.network_count);
    TEST_ASSERT_EQUAL(before.wifi.active, now.wifi.active);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(before.wifi.mode), static_cast<int>(now.wifi.mode));
    for (uint8_t i = 0; i < config::kMaxNetworks; ++i) {
        TEST_ASSERT_EQUAL_STRING(before.wifi.networks[i].ssid, now.wifi.networks[i].ssid);
        TEST_ASSERT_EQUAL_STRING(before.wifi.networks[i].password,
                                 now.wifi.networks[i].password);
        TEST_ASSERT_EQUAL_STRING(before.wifi.networks[i].ip.address,
                                 now.wifi.networks[i].ip.address);
    }
    TEST_ASSERT_EQUAL_STRING(before.wifi.ap_ssid, now.wifi.ap_ssid);
    TEST_ASSERT_EQUAL_STRING(before.wifi.ap_password, now.wifi.ap_password);
    TEST_ASSERT_EQUAL_STRING(before.nats.url, now.nats.url);
}

void AssertRefused(const char *body, WriteResult expected) {
    const config::Data before = Device();
    config::Data data = before;
    const web::WriteOutcome out = Apply(body, &data);
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(expected), static_cast<int>(out.result), body);
    TEST_ASSERT_FALSE(out.wifi_changed);
    TEST_ASSERT_FALSE(out.nats_changed);
    AssertUnchanged(data, before);
}

}  // namespace

// --- What may be written -------------------------------------------------

void test_web_the_mode_is_the_screens_own_three_words(void) {
    // The same mapping `ui::WifiView` cycles through, because it is the same two
    // config fields — and `off` deliberately leaves `wifi.mode` alone: off is a
    // statement about the radio, not about which record is being looked at.
    config::Data data = Device();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WriteResult::kOk),
                          static_cast<int>(Apply("{\"wifi\":{\"mode\":\"ap\"}}", &data).result));
    TEST_ASSERT_TRUE(data.wifi.active);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(config::WifiMode::kAp),
                          static_cast<int>(data.wifi.mode));

    TEST_ASSERT_EQUAL_INT(static_cast<int>(WriteResult::kOk),
                          static_cast<int>(Apply("{\"wifi\":{\"mode\":\"off\"}}", &data).result));
    TEST_ASSERT_FALSE(data.wifi.active);
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(config::WifiMode::kAp),
                                  static_cast<int>(data.wifi.mode),
                                  "switching the radio off moved which record is shown");

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(WriteResult::kOk),
        static_cast<int>(Apply("{\"wifi\":{\"mode\":\"client\"}}", &data).result));
    TEST_ASSERT_TRUE(data.wifi.active);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(config::WifiMode::kClient),
                          static_cast<int>(data.wifi.mode));
}

void test_web_a_network_list_replaces_the_one_there(void) {
    config::Data data = Device();
    const web::WriteOutcome out = Apply(
        "{\"wifi\":{\"networks\":[{\"ssid\":\"cafe\",\"password\":\"latte123\"}]}}", &data);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WriteResult::kOk), static_cast<int>(out.result));
    TEST_ASSERT_TRUE(out.wifi_changed);
    TEST_ASSERT_FALSE(out.nats_changed);
    TEST_ASSERT_EQUAL_UINT8(1, data.wifi.network_count);
    TEST_ASSERT_EQUAL_STRING("cafe", data.wifi.networks[0].ssid);
    TEST_ASSERT_EQUAL_STRING("latte123", data.wifi.networks[0].password);
    // And the slots behind it are cleared rather than left holding the old list —
    // a name in a slot past `network_count` is a name that comes back the next time
    // somebody adds a network.
    TEST_ASSERT_EQUAL_STRING("", data.wifi.networks[1].ssid);
    TEST_ASSERT_EQUAL_STRING("", data.wifi.networks[1].password);
}

void test_web_an_empty_list_forgets_every_network(void) {
    // Which is how the page's ✕ works: it submits the list without that row.
    config::Data data = Device();
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(WriteResult::kOk),
        static_cast<int>(Apply("{\"wifi\":{\"networks\":[]}}", &data).result));
    TEST_ASSERT_EQUAL_UINT8(0, data.wifi.network_count);
    TEST_ASSERT_EQUAL_STRING("", data.wifi.networks[0].ssid);
}

void test_web_the_bus_url_is_parsed_not_just_stored(void) {
    config::Data data = Device();
    const web::WriteOutcome out =
        Apply("{\"nats\":{\"url\":\"nats://10.0.0.9:4222\"}}", &data);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WriteResult::kOk), static_cast<int>(out.result));
    TEST_ASSERT_TRUE(out.nats_changed);
    TEST_ASSERT_FALSE(out.wifi_changed);
    TEST_ASSERT_EQUAL_STRING("nats://10.0.0.9:4222", data.nats.url);
}

void test_web_an_empty_bus_url_is_how_the_bus_is_switched_off(void) {
    // The same pair the console's `config set nats` makes: empty is not a typo.
    config::Data data = Device();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WriteResult::kOk),
                          static_cast<int>(Apply("{\"nats\":{\"url\":\"\"}}", &data).result));
    TEST_ASSERT_EQUAL_STRING("", data.nats.url);
}

void test_web_both_sections_in_one_document(void) {
    config::Data data = Device();
    const web::WriteOutcome out = Apply(
        "{\"wifi\":{\"mode\":\"client\"},\"nats\":{\"url\":\"nats://10.0.0.9:4222\"}}", &data);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WriteResult::kOk), static_cast<int>(out.result));
    TEST_ASSERT_TRUE(out.wifi_changed);
    TEST_ASSERT_TRUE(out.nats_changed);
}

// --- The rule the whole design hangs off: a password is write-only --------

void test_web_a_password_nobody_retyped_is_kept(void) {
    // **The reason this is not a merge.** `GET /api/wifi` never returns a
    // passphrase (§10.15), so a form cannot send back what it never had — and a
    // document that took an absent password as "make this network open" would strip
    // the key off every network the first time anybody pressed Apply.
    config::Data data = Device();
    const web::WriteOutcome out = Apply(
        "{\"wifi\":{\"networks\":[{\"ssid\":\"home\"},"
        "{\"ssid\":\"office\",\"password\":null}]}}",
        &data);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WriteResult::kOk), static_cast<int>(out.result));
    TEST_ASSERT_EQUAL_STRING("homesecret", data.wifi.networks[0].password);
    TEST_ASSERT_EQUAL_STRING("officesecret", data.wifi.networks[1].password);
}

void test_web_a_password_is_kept_by_name_not_by_position(void) {
    // The form can reorder the rows, and a password that followed the *slot* would
    // then be handed to a different network.
    config::Data data = Device();
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(WriteResult::kOk),
        static_cast<int>(
            Apply("{\"wifi\":{\"networks\":[{\"ssid\":\"office\"},{\"ssid\":\"home\"}]}}", &data)
                .result));
    TEST_ASSERT_EQUAL_STRING("office", data.wifi.networks[0].ssid);
    TEST_ASSERT_EQUAL_STRING("officesecret", data.wifi.networks[0].password);
    TEST_ASSERT_EQUAL_STRING("home", data.wifi.networks[1].ssid);
    TEST_ASSERT_EQUAL_STRING("homesecret", data.wifi.networks[1].password);
}

void test_web_an_empty_password_is_a_network_with_no_key(void) {
    // The other half, and the difference between them is the whole point: absent
    // is "I did not retype it", empty is "this one is open".
    config::Data data = Device();
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(WriteResult::kOk),
        static_cast<int>(
            Apply("{\"wifi\":{\"networks\":[{\"ssid\":\"home\",\"password\":\"\"}]}}", &data)
                .result));
    TEST_ASSERT_EQUAL_STRING("", data.wifi.networks[0].password);
}

void test_web_a_new_network_starts_with_no_password_rather_than_somebody_elses(void) {
    config::Data data = Device();
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(WriteResult::kOk),
        static_cast<int>(Apply("{\"wifi\":{\"networks\":[{\"ssid\":\"cafe\"}]}}", &data).result));
    TEST_ASSERT_EQUAL_STRING("", data.wifi.networks[0].password);
}

void test_web_the_access_point_keeps_its_key_the_same_way(void) {
    config::Data data = Device();
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(WriteResult::kOk),
        static_cast<int>(Apply("{\"wifi\":{\"ap\":{\"ssid\":\"desk\"}}}", &data).result));
    TEST_ASSERT_EQUAL_STRING("desk", data.wifi.ap_ssid);
    TEST_ASSERT_EQUAL_STRING("apsecret", data.wifi.ap_password);
}

// --- The refusals --------------------------------------------------------

void test_web_a_field_that_is_not_on_the_list_is_refused(void) {
    // **Refused rather than ignored**, and this is the test that says the write
    // path is a whitelist: the touch calibration, the idle timers, the clock and
    // the display's brightness are all in `config.json` and none of them is
    // reachable from a form. A page with a typo in it fails loudly here.
    AssertRefused("{\"touch\":{\"scaleX\":30000}}", WriteResult::kUnknownField);
    AssertRefused("{\"display\":{\"brightness\":1}}", WriteResult::kUnknownField);
    AssertRefused("{\"time\":{\"zone\":\"UTC\"}}", WriteResult::kUnknownField);
    AssertRefused("{\"audio\":{\"volume\":100}}", WriteResult::kUnknownField);
    AssertRefused("{\"web\":{\"write\":false}}", WriteResult::kUnknownField);
    AssertRefused("{\"wifi\":{\"rounds\":9}}", WriteResult::kUnknownField);
    AssertRefused("{\"nats\":{\"port\":4222}}", WriteResult::kUnknownField);
}

void test_web_the_switch_that_turns_writing_off_cannot_be_written(void) {
    // Named on its own because it is the one field whose reachability would make
    // the switch pointless: a form that could set `web.write` back to true is not a
    // switch, it is a suggestion. It is refused by the rule above rather than by a
    // special case, which is the whole argument for a whitelist.
    AssertRefused("{\"web\":{\"write\":true}}", WriteResult::kUnknownField);
}

void test_web_the_credential_that_locks_the_site_cannot_be_written_from_it(void) {
    // The other field named on its own, and for the sharper version of the same
    // reason (§10.16): a form that could clear `web.user` is a lock that unlocks
    // itself, and one that could *set* it is a way to lock the owner out of their
    // own device from the LAN. Both are refused by the rule above rather than by a
    // special case — the console and `config.json` are the two ways in.
    AssertRefused("{\"web\":{\"user\":\"attacker\",\"password\":\"x\"}}",
                  WriteResult::kUnknownField);
    AssertRefused("{\"web\":{\"user\":\"\"}}", WriteResult::kUnknownField);
}

void test_web_junk_is_refused_as_junk(void) {
    AssertRefused("", WriteResult::kNotJson);
    AssertRefused("{\"wifi\":", WriteResult::kNotJson);
    AssertRefused("not json at all", WriteResult::kNotJson);
    // Valid JSON that is not an object — §10.15 records this exact hole in the
    // config parser, where every lookup answered null and the device said nothing.
    AssertRefused("[]", WriteResult::kNotObject);
    AssertRefused("42", WriteResult::kNotObject);
    AssertRefused("\"hello\"", WriteResult::kNotObject);
}

void test_web_a_value_of_the_wrong_kind_is_refused(void) {
    AssertRefused("{\"wifi\":\"client\"}", WriteResult::kBadValue);
    AssertRefused("{\"wifi\":{\"mode\":7}}", WriteResult::kBadValue);
    AssertRefused("{\"wifi\":{\"networks\":{}}}", WriteResult::kBadValue);
    AssertRefused("{\"wifi\":{\"networks\":[\"home\"]}}", WriteResult::kBadValue);
    AssertRefused("{\"wifi\":{\"ap\":true}}", WriteResult::kBadValue);
    AssertRefused("{\"nats\":{\"url\":4222}}", WriteResult::kBadValue);
}

void test_web_a_mode_this_device_does_not_have_is_refused_not_guessed(void) {
    // §10.9's rule, arriving over HTTP: "yes" is not "on", and a device that
    // guessed would be a device serving an access point nobody asked for.
    AssertRefused("{\"wifi\":{\"mode\":\"yes\"}}", WriteResult::kBadValue);
    AssertRefused("{\"wifi\":{\"mode\":\"AP\"}}", WriteResult::kBadValue);
    AssertRefused("{\"wifi\":{\"mode\":\"\"}}", WriteResult::kBadValue);
}

void test_web_a_url_that_will_not_parse_is_refused(void) {
    // The console refuses the same strings for the same reason: a bus this device
    // silently never connects to looks exactly like a bus that is down.
    AssertRefused("{\"nats\":{\"url\":\"http://10.0.0.9:4222\"}}", WriteResult::kBadValue);
    AssertRefused("{\"nats\":{\"url\":\"nats://10.0.0.9:0\"}}", WriteResult::kBadValue);
    AssertRefused("{\"nats\":{\"url\":\"nats://10.0.0.9:4222/sub\"}}", WriteResult::kBadValue);
}

void test_web_a_string_too_long_for_its_field_is_refused_rather_than_cut(void) {
    // `config::CopyString` makes the same call inside the parser: a half-length
    // SSID fails to connect and gives no hint which half is being used.
    char body[256] = {};
    std::snprintf(body, sizeof body,
                  "{\"wifi\":{\"networks\":[{\"ssid\":\"%s\"}]}}",
                  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");  // 33, one over
    AssertRefused(body, WriteResult::kTooLong);

    std::snprintf(body, sizeof body, "{\"nats\":{\"url\":\"nats://%s:4222\"}}",
                  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    AssertRefused(body, WriteResult::kTooLong);
}

void test_web_a_fifth_network_is_refused_rather_than_dropped(void) {
    // A list that came back shorter than it was submitted is a network the operator
    // believes this device knows about.
    AssertRefused(
        "{\"wifi\":{\"networks\":[{\"ssid\":\"a\"},{\"ssid\":\"b\"},{\"ssid\":\"c\"},"
        "{\"ssid\":\"d\"},{\"ssid\":\"e\"}]}}",
        WriteResult::kTooMany);
}

void test_web_a_nameless_network_is_refused(void) {
    // What an empty row in the form looks like. Dropping it silently is the same
    // mistake as truncating the list.
    AssertRefused("{\"wifi\":{\"networks\":[{\"ssid\":\"\"}]}}", WriteResult::kBadValue);
    AssertRefused("{\"wifi\":{\"networks\":[{\"password\":\"x\"}]}}", WriteResult::kBadValue);
}

void test_web_a_static_address_is_parsed_like_the_console_parses_one(void) {
    config::Data data = Device();
    const web::WriteOutcome out = Apply(
        "{\"wifi\":{\"networks\":[{\"ssid\":\"home\",\"ip\":{\"static\":true,"
        "\"address\":\"10.0.0.42\",\"netmask\":\"255.255.255.0\",\"gateway\":\"10.0.0.1\"}}]}}",
        &data);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WriteResult::kOk), static_cast<int>(out.result));
    TEST_ASSERT_TRUE(data.wifi.networks[0].ip.enabled);
    TEST_ASSERT_EQUAL_STRING("10.0.0.42", data.wifi.networks[0].ip.address);
    TEST_ASSERT_EQUAL_STRING("", data.wifi.networks[0].ip.dns1);

    // And the two §10.9 spends a paragraph on: a leading zero means two different
    // addresses to two different readers, and an octet over 255 is not one at all.
    AssertRefused(
        "{\"wifi\":{\"networks\":[{\"ssid\":\"home\",\"ip\":{\"static\":true,"
        "\"address\":\"010.0.0.42\",\"netmask\":\"255.255.255.0\",\"gateway\":\"10.0.0.1\"}}]}}",
        WriteResult::kBadValue);
    AssertRefused(
        "{\"wifi\":{\"networks\":[{\"ssid\":\"home\",\"ip\":{\"static\":true,"
        "\"address\":\"10.0.0.300\",\"netmask\":\"255.255.255.0\",\"gateway\":\"10.0.0.1\"}}]}}",
        WriteResult::kBadValue);
    // Enabled and missing the three that matter is refused here rather than
    // half-configured — the console's own parser falls back to DHCP with a log
    // line, and this one has somebody looking at a form to tell instead.
    AssertRefused(
        "{\"wifi\":{\"networks\":[{\"ssid\":\"home\",\"ip\":{\"static\":true}}]}}",
        WriteResult::kBadValue);
}

void test_web_an_address_the_form_does_not_carry_is_kept(void) {
    // **The password rule, applied to the other field a form does not have**, and
    // it is here because the board found the bug: the Wi-Fi page has no address
    // fields, so before this every Apply quietly forgot a static address somebody
    // had set from the console. Watched happening to a `192.168.1.42`.
    config::Data data = Device();
    data.wifi.networks[0].ip.enabled = true;
    std::snprintf(data.wifi.networks[0].ip.address, config::kIpTextSize, "%s", "10.0.0.42");
    std::snprintf(data.wifi.networks[0].ip.netmask, config::kIpTextSize, "%s", "255.255.255.0");
    std::snprintf(data.wifi.networks[0].ip.gateway, config::kIpTextSize, "%s", "10.0.0.1");

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(WriteResult::kOk),
        static_cast<int>(
            Apply("{\"wifi\":{\"networks\":[{\"ssid\":\"home\"},{\"ssid\":\"office\"}]}}", &data)
                .result));
    TEST_ASSERT_TRUE(data.wifi.networks[0].ip.enabled);
    TEST_ASSERT_EQUAL_STRING("10.0.0.42", data.wifi.networks[0].ip.address);
    TEST_ASSERT_EQUAL_STRING("10.0.0.1", data.wifi.networks[0].ip.gateway);
}

void test_web_a_block_that_is_sent_replaces_the_address(void) {
    // The other half: a form that *does* carry an address sets it, and `static`
    // false is how a network goes back to DHCP.
    config::Data data = Device();
    data.wifi.networks[0].ip.enabled = true;
    std::snprintf(data.wifi.networks[0].ip.address, config::kIpTextSize, "%s", "10.0.0.42");

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(WriteResult::kOk),
        static_cast<int>(
            Apply("{\"wifi\":{\"networks\":[{\"ssid\":\"home\",\"ip\":{\"static\":false}}]}}",
                  &data)
                .result));
    TEST_ASSERT_FALSE(data.wifi.networks[0].ip.enabled);
    TEST_ASSERT_EQUAL_STRING("", data.wifi.networks[0].ip.address);
}

void test_web_a_document_bigger_than_the_buffer_is_refused(void) {
    config::Data data = Device();
    const web::WriteOutcome out =
        ApplySettings("{\"wifi\":{}}", web::kMaxSettingsBody + 1, &data);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WriteResult::kTooBig), static_cast<int>(out.result));
}

void test_web_the_second_section_being_bad_undoes_the_first(void) {
    // **The rule that makes a refusal safe**: the document is applied to a copy, so
    // a form whose bus URL is a typo does not leave the Wi-Fi half written and the
    // operator none the wiser about which parts landed.
    //
    // The first version of this test used `nope` as the bad URL and it **passed
    // through**, which is §10.11's usual finding arriving from its less usual
    // direction: the test was wrong and the code was right. §10.5 accepts a bare
    // host as one of its four spellings, so `nope` is a hostname — a wrong one, and
    // not one this layer can know is wrong. A scheme this device refuses outright is
    // the thing that is actually invalid.
    AssertRefused("{\"wifi\":{\"mode\":\"ap\"},\"nats\":{\"url\":\"ws://10.0.0.9:4222\"}}",
                  WriteResult::kBadValue);
}

void test_web_every_refusal_has_words_of_its_own(void) {
    // A console prints the reason and the page shows it. Two refusals that read the
    // same are two refusals somebody debugs the same way.
    const WriteResult every[] = {WriteResult::kTooBig,       WriteResult::kNotJson,
                                 WriteResult::kNotObject,    WriteResult::kUnknownField,
                                 WriteResult::kBadValue,     WriteResult::kTooLong,
                                 WriteResult::kTooMany};
    for (size_t i = 0; i < sizeof every / sizeof every[0]; ++i) {
        const char *text = web::WriteResultText(every[i]);
        TEST_ASSERT_NOT_NULL(text);
        TEST_ASSERT_TRUE(std::strlen(text) > 8);
        for (size_t j = 0; j < i; ++j) {
            TEST_ASSERT_TRUE(std::strcmp(text, web::WriteResultText(every[j])) != 0);
        }
    }
}

// --- The four verbs ------------------------------------------------------

void test_web_each_action_is_named_exactly(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kSave),
                          static_cast<int>(web::ActionFromUri("/api/action?do=save")));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kReload),
                          static_cast<int>(web::ActionFromUri("/api/action?do=reload")));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kWifiRetry),
                          static_cast<int>(web::ActionFromUri("/api/action?do=retry")));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kBusRetry),
                          static_cast<int>(web::ActionFromUri("/api/action?do=reconnect")));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kSave),
                          static_cast<int>(web::ActionFromUri("/api/action?t=1&do=save")));
}

void test_web_a_verb_this_device_does_not_have_does_nothing(void) {
    // The same trap `ConfirmsReboot` is written round: a value that merely starts
    // with a verb is not that verb, and a parameter that merely ends with `do` is
    // not `do`.
    const char *nothing[] = {"/api/action",       "/api/action?",
                             "/api/action?do=",   "/api/action?do=saved",
                             "/api/action?do=sav", "/api/action?xdo=save",
                             "/api/action?do=SAVE", "/api/action?done=save",
                             "/api/do=save"};
    for (const char *uri : nothing) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(Action::kNone),
                                      static_cast<int>(web::ActionFromUri(uri)), uri);
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Action::kNone), static_cast<int>(web::ActionFromUri(nullptr)));
}

void test_web_every_action_has_a_name(void) {
    const Action every[] = {Action::kNone, Action::kSave, Action::kReload, Action::kWifiRetry,
                            Action::kBusRetry};
    for (Action action : every) {
        TEST_ASSERT_NOT_NULL(web::ActionName(action));
    }
}

void RegisterWebSettingsTests(void) {
    RUN_TEST(test_web_the_mode_is_the_screens_own_three_words);
    RUN_TEST(test_web_a_network_list_replaces_the_one_there);
    RUN_TEST(test_web_an_empty_list_forgets_every_network);
    RUN_TEST(test_web_the_bus_url_is_parsed_not_just_stored);
    RUN_TEST(test_web_an_empty_bus_url_is_how_the_bus_is_switched_off);
    RUN_TEST(test_web_both_sections_in_one_document);

    RUN_TEST(test_web_a_password_nobody_retyped_is_kept);
    RUN_TEST(test_web_a_password_is_kept_by_name_not_by_position);
    RUN_TEST(test_web_an_empty_password_is_a_network_with_no_key);
    RUN_TEST(test_web_a_new_network_starts_with_no_password_rather_than_somebody_elses);
    RUN_TEST(test_web_the_access_point_keeps_its_key_the_same_way);

    RUN_TEST(test_web_a_field_that_is_not_on_the_list_is_refused);
    RUN_TEST(test_web_the_switch_that_turns_writing_off_cannot_be_written);
    RUN_TEST(test_web_the_credential_that_locks_the_site_cannot_be_written_from_it);
    RUN_TEST(test_web_junk_is_refused_as_junk);
    RUN_TEST(test_web_a_value_of_the_wrong_kind_is_refused);
    RUN_TEST(test_web_a_mode_this_device_does_not_have_is_refused_not_guessed);
    RUN_TEST(test_web_a_url_that_will_not_parse_is_refused);
    RUN_TEST(test_web_a_string_too_long_for_its_field_is_refused_rather_than_cut);
    RUN_TEST(test_web_a_fifth_network_is_refused_rather_than_dropped);
    RUN_TEST(test_web_a_nameless_network_is_refused);
    RUN_TEST(test_web_a_static_address_is_parsed_like_the_console_parses_one);
    RUN_TEST(test_web_an_address_the_form_does_not_carry_is_kept);
    RUN_TEST(test_web_a_block_that_is_sent_replaces_the_address);
    RUN_TEST(test_web_a_document_bigger_than_the_buffer_is_refused);
    RUN_TEST(test_web_the_second_section_being_bad_undoes_the_first);
    RUN_TEST(test_web_every_refusal_has_words_of_its_own);

    RUN_TEST(test_web_each_action_is_named_exactly);
    RUN_TEST(test_web_a_verb_this_device_does_not_have_does_nothing);
    RUN_TEST(test_web_every_action_has_a_name);
}
