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

#include <cstdio>
#include <cstring>

#include "cJSON.h"
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
            "sntp": "time.google.com", "syncHours": 12 },
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

// Every key in `a` is in `b` and the other way round, one level at a time.
// Objects recurse; arrays are compared through their **first** element only,
// because the two files legitimately hold different numbers of networks and
// what matters is that an entry has the same fields in both. A missing array
// on one side is not drift either — `config.init.json` ships with no networks
// at all, and that is the point of it.
void AssertSameKeys(const cJSON *a, const cJSON *b, const char *path) {
    char child_path[128] = {};

    for (const cJSON *item = a->child; item != nullptr; item = item->next) {
        if (item->string == nullptr) {
            continue;
        }
        snprintf(child_path, sizeof(child_path), "%s.%s", path, item->string);
        const cJSON *other = cJSON_GetObjectItemCaseSensitive(b, item->string);
        TEST_ASSERT_NOT_NULL_MESSAGE(other, child_path);
        if (cJSON_IsObject(item) && cJSON_IsObject(other)) {
            AssertSameKeys(item, other, child_path);
        } else if (cJSON_IsArray(item) && cJSON_IsArray(other)) {
            const cJSON *first_a = item->child;
            const cJSON *first_b = other->child;
            if (first_a != nullptr && first_b != nullptr && cJSON_IsObject(first_a) &&
                cJSON_IsObject(first_b)) {
                AssertSameKeys(first_a, first_b, child_path);
            }
        }
    }

    for (const cJSON *item = b->child; item != nullptr; item = item->next) {
        if (item->string == nullptr) {
            continue;
        }
        snprintf(child_path, sizeof(child_path), "%s.%s", path, item->string);
        TEST_ASSERT_NOT_NULL_MESSAGE(cJSON_GetObjectItemCaseSensitive(a, item->string), child_path);
    }
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
    // restore then changes settings nobody meant to change.
    //
    // **Compared as JSON, not as parsed structs**, and that is the whole
    // design of this test. After `FillDefaults` a key that was missing and a
    // key that was present hold the same number, so a struct comparison passes
    // for the very drift this exists to catch. Worse, it fails for something
    // that is *not* drift: the shipped `config.json` legitimately carries an
    // operator's settings, so an AP channel of 1 against a default of 6 is a
    // configured device, not a mistake. Both of those were learned here — the
    // struct version started failing the day somebody edited the file.
    //
    // So: every key one file has, the other has too. Values are ignored.
    FreshWithDefaults();
    TEST_ASSERT_TRUE(fake::PutRepoFile("config.json"));

    // Read before anything runs: `config::Restore` would put `config.init.json`
    // over `config.json` and leave two identical files to compare, which is a
    // test that cannot fail.
    static char live_json[config::kMaxFileSize];
    static char defaults_json[config::kMaxFileSize];
    TEST_ASSERT_TRUE(fake::GetFile("config.json", live_json, sizeof(live_json)));
    TEST_ASSERT_TRUE(fake::GetFile("config.init.json", defaults_json, sizeof(defaults_json)));

    cJSON *live = cJSON_Parse(live_json);
    cJSON *defaults = cJSON_Parse(defaults_json);
    TEST_ASSERT_NOT_NULL_MESSAGE(live, "spiffs_image/config.json does not parse");
    TEST_ASSERT_NOT_NULL_MESSAGE(defaults, "spiffs_image/config.init.json does not parse");
    AssertSameKeys(live, defaults, "config");
    cJSON_Delete(live);
    cJSON_Delete(defaults);
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
        // The password exactly, because that is the thing that must never be
        // real. The SSID by **prefix**, because the file legitimately grows
        // numbered placeholder slots — `YOUR_SSID2` is still nobody's network,
        // and failing on it is a false alarm that teaches people to ignore
        // this test. A real name (`Barsik`) still fails, which is the case it
        // exists for.
        TEST_ASSERT_EQUAL_STRING("CHANGEME", wifi.networks[i].password);
        TEST_ASSERT_EQUAL_STRING_LEN("YOUR_SSID", wifi.networks[i].ssid, 9);
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
    TEST_ASSERT_EQUAL_UINT8(12, c.time.sync_hours);
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
    TEST_ASSERT_EQUAL_UINT8(expected.time.sync_hours, c.time.sync_hours);
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

// --- What the Wi-Fi manager reads out of it (§10.9) ----------------------

void test_config_reads_the_wifi_mode_and_the_fallback_settings(void) {
    FreshWithDefaults();
    fake::PutFile("config.json", R"({"wifi":{
        "active": true, "mode": "ap", "rounds": 3, "apWindowSeconds": 45,
        "ap": {"ssid":"desk-approver","password":"letmein9","channel":11},
        "networks": []}})");

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    const config::Wifi &wifi = config::Get().wifi;
    TEST_ASSERT_TRUE(wifi.active);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(config::WifiMode::kAp), static_cast<int>(wifi.mode));
    TEST_ASSERT_EQUAL_UINT8(3, wifi.rounds_before_ap);
    TEST_ASSERT_EQUAL_UINT16(45, wifi.ap_window_seconds);
    TEST_ASSERT_EQUAL_STRING("desk-approver", wifi.ap_ssid);
    TEST_ASSERT_EQUAL_STRING("letmein9", wifi.ap_password);
    TEST_ASSERT_EQUAL_UINT8(11, wifi.ap_channel);
}

void test_config_an_unknown_wifi_mode_keeps_the_default(void) {
    // The same call §10.8.2 makes about a misspelled zone: two spellings are
    // the whole vocabulary, and reading a third as one of them would be a
    // device quietly doing something nobody asked for. "station" is the
    // plausible wrong word, which is what makes it the one to test.
    FreshWithDefaults();
    fake::PutFile("config.json", R"({"wifi":{"active":true,"mode":"station"}})");

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(config::WifiMode::kClient),
                          static_cast<int>(config::Get().wifi.mode));
}

void test_config_the_access_point_settings_round_trip(void) {
    FreshWithDefaults();
    fake::PutFile("config.json", kGoodJson);
    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());

    config::Get().wifi.mode = config::WifiMode::kAp;
    config::Get().wifi.rounds_before_ap = 4;
    config::Get().wifi.ap_window_seconds = 90;
    snprintf(config::Get().wifi.ap_password, sizeof(config::Get().wifi.ap_password), "s3cretpw");
    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Save());

    config::Get().wifi.mode = config::WifiMode::kClient;  // scribble over it
    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Reload());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(config::WifiMode::kAp),
                          static_cast<int>(config::Get().wifi.mode));
    TEST_ASSERT_EQUAL_UINT8(4, config::Get().wifi.rounds_before_ap);
    TEST_ASSERT_EQUAL_UINT16(90, config::Get().wifi.ap_window_seconds);
    TEST_ASSERT_EQUAL_STRING("s3cretpw", config::Get().wifi.ap_password);
}

void test_config_a_file_with_no_wifi_section_still_has_an_access_point(void) {
    // The fallback AP of §10.9 is what rescues a device that cannot reach a
    // network, so it must not itself depend on the file being complete: a
    // `config.json` with nothing in it has to leave an SSID to raise.
    FreshWithDefaults();
    fake::PutFile("config.json", "{}");

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    TEST_ASSERT_TRUE(config::Get().wifi.ap_ssid[0] != '\0');
    TEST_ASSERT_TRUE(config::Get().wifi.rounds_before_ap > 0);
    TEST_ASSERT_TRUE(config::Get().wifi.ap_window_seconds > 0);
}

// --- A fixed address for a network (§10.9) -------------------------------

void test_config_parses_a_dotted_quad_first_octet_in_the_low_byte(void) {
    uint32_t value = 0xDEADBEEF;
    TEST_ASSERT_TRUE(config::ParseIpv4("0.0.0.0", &value));
    TEST_ASSERT_EQUAL_HEX32(0u, value);

    TEST_ASSERT_TRUE(config::ParseIpv4("255.255.255.255", &value));
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFu, value);

    // The byte order is the whole point of this test: `esp_netif_ip_info_t`
    // and the console's own `%u.%u.%u.%u` both put the first octet in the low
    // byte, and getting it backwards gives a plausible address on a network
    // nobody is on.
    TEST_ASSERT_TRUE(config::ParseIpv4("192.168.1.42", &value));
    TEST_ASSERT_EQUAL_HEX32(0x2A01A8C0u, value);
}

void test_config_refuses_an_address_that_is_not_one(void) {
    const char *const rubbish[] = {
        "",  "192.168.1", "192.168.1.2.3", "192.168.1.256", "192.168.1.-1",
        "192.168.1.a", " 192.168.1.1", "192.168.1.1 ", "192.168..1", "1.2.3.4\n",
        "0x7f.0.0.1",
    };
    for (const char *text : rubbish) {
        uint32_t value = 0;
        TEST_ASSERT_FALSE_MESSAGE(config::ParseIpv4(text, &value), text);
    }
    uint32_t ignored = 0;
    TEST_ASSERT_FALSE(config::ParseIpv4(nullptr, &ignored));
}

void test_config_refuses_a_leading_zero_rather_than_reading_it_as_octal(void) {
    // `inet_aton("010.1.1.1")` is 8.1.1.1 — the C library's oldest trap, and
    // one that produces a *working-looking* address on the wrong subnet. An
    // operator who writes `010` means ten, so the honest answer is to refuse
    // rather than to pick one of the two meanings.
    uint32_t value = 0;
    TEST_ASSERT_FALSE(config::ParseIpv4("010.1.1.1", &value));
    TEST_ASSERT_FALSE(config::ParseIpv4("192.168.01.1", &value));
    // A single zero octet is not a leading zero.
    TEST_ASSERT_TRUE(config::ParseIpv4("10.0.0.1", &value));
}

void test_config_reads_a_static_address_for_one_network(void) {
    FreshWithDefaults();
    fake::PutFile("config.json", R"({"wifi":{"networks":[
        {"ssid":"home","password":"x"},
        {"ssid":"office","password":"y","ip":{"static":true,
         "address":"10.0.0.42","netmask":"255.255.255.0","gateway":"10.0.0.1",
         "dns1":"10.0.0.1","dns2":"1.1.1.1"}}]}})");

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    const config::Wifi &wifi = config::Get().wifi;
    TEST_ASSERT_EQUAL_UINT8(2, wifi.network_count);

    // **Per network, not per device** — the home entry stays on DHCP, which is
    // the case that makes this worth having at all.
    TEST_ASSERT_FALSE(wifi.networks[0].ip.enabled);
    TEST_ASSERT_TRUE(wifi.networks[1].ip.enabled);
    TEST_ASSERT_EQUAL_STRING("10.0.0.42", wifi.networks[1].ip.address);
    TEST_ASSERT_EQUAL_STRING("255.255.255.0", wifi.networks[1].ip.netmask);
    TEST_ASSERT_EQUAL_STRING("10.0.0.1", wifi.networks[1].ip.gateway);
    TEST_ASSERT_EQUAL_STRING("10.0.0.1", wifi.networks[1].ip.dns1);
    TEST_ASSERT_EQUAL_STRING("1.1.1.1", wifi.networks[1].ip.dns2);
}

void test_config_a_static_block_with_no_dns_is_still_static(void) {
    // The house firmware requires both DNS servers before it will call a
    // static config valid. A LAN with no resolver of its own is an ordinary
    // thing and the NATS URL of §10.3 is an address rather than a name, so
    // the entries are optional here.
    FreshWithDefaults();
    fake::PutFile("config.json", R"({"wifi":{"networks":[{"ssid":"lab","ip":{
        "static":true,"address":"10.0.0.9","netmask":"255.255.255.0",
        "gateway":"10.0.0.1"}}]}})");

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    TEST_ASSERT_TRUE(config::Get().wifi.networks[0].ip.enabled);
    TEST_ASSERT_EQUAL_STRING("", config::Get().wifi.networks[0].ip.dns1);
}

void test_config_a_broken_static_address_falls_back_to_dhcp(void) {
    // **The network is kept and the static config is dropped**, which is the
    // only combination that leaves a device reachable: refusing the whole
    // entry would lose a working SSID over a typo in an optional field, and
    // honouring half of it would give an interface with an address and no
    // route.
    const char *const broken[] = {
        R"({"static":true,"netmask":"255.255.255.0","gateway":"10.0.0.1"})",
        R"({"static":true,"address":"10.0.0.300","netmask":"255.255.255.0","gateway":"10.0.0.1"})",
        R"({"static":true,"address":"10.0.0.9","gateway":"10.0.0.1"})",
        R"({"static":true,"address":"10.0.0.9","netmask":"255.255.255.0"})",
        R"({"static":true,"address":"10.0.0.9","netmask":"nonsense","gateway":"10.0.0.1"})",
    };
    for (const char *ip : broken) {
        FreshWithDefaults();
        char json[512] = {};
        snprintf(json, sizeof(json), R"({"wifi":{"networks":[{"ssid":"lab","ip":%s}]}})", ip);
        fake::PutFile("config.json", json);

        TEST_ASSERT_EQUAL_INT_MESSAGE(ESP_OK, config::Init(), ip);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, config::Get().wifi.network_count, ip);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("lab", config::Get().wifi.networks[0].ssid, ip);
        TEST_ASSERT_FALSE_MESSAGE(config::Get().wifi.networks[0].ip.enabled, ip);
    }
}

void test_config_a_bad_dns_entry_does_not_disable_the_address(void) {
    // DNS is the optional half, so a typo there costs name resolution and not
    // the interface. The entry is dropped rather than passed on.
    FreshWithDefaults();
    fake::PutFile("config.json", R"({"wifi":{"networks":[{"ssid":"lab","ip":{
        "static":true,"address":"10.0.0.9","netmask":"255.255.255.0",
        "gateway":"10.0.0.1","dns1":"not-an-address"}}]}})");

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    TEST_ASSERT_TRUE(config::Get().wifi.networks[0].ip.enabled);
    TEST_ASSERT_EQUAL_STRING("", config::Get().wifi.networks[0].ip.dns1);
}

void test_config_a_static_address_round_trips(void) {
    FreshWithDefaults();
    fake::PutFile("config.json", kGoodJson);
    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());

    config::StaticIp &ip = config::Get().wifi.networks[0].ip;
    ip.enabled = true;
    snprintf(ip.address, sizeof(ip.address), "192.168.7.7");
    snprintf(ip.netmask, sizeof(ip.netmask), "255.255.255.0");
    snprintf(ip.gateway, sizeof(ip.gateway), "192.168.7.1");
    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Save());

    config::Get().wifi.networks[0].ip = {};  // scribble over it
    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Reload());
    TEST_ASSERT_TRUE(config::Get().wifi.networks[0].ip.enabled);
    TEST_ASSERT_EQUAL_STRING("192.168.7.7", config::Get().wifi.networks[0].ip.address);
    TEST_ASSERT_EQUAL_STRING("192.168.7.1", config::Get().wifi.networks[0].ip.gateway);
}

void test_config_a_network_on_dhcp_writes_no_ip_block(void) {
    // Every entry carrying an empty `ip` object would be noise in a file
    // people edit by hand, and §10.15 already warns that a write is what the
    // struct says rather than what the file said. So the block appears when
    // there is something in it.
    FreshWithDefaults();
    fake::PutFile("config.json", kGoodJson);
    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Save());

    char back[config::kMaxFileSize] = {};
    TEST_ASSERT_TRUE(fake::GetFile("config.json", back, sizeof(back)));
    TEST_ASSERT_NULL(std::strstr(back, "\"ip\""));
}

// --- The internet check (§10.9) ------------------------------------------

void test_config_reads_the_internet_check(void) {
    FreshWithDefaults();
    fake::PutFile("config.json", R"({"internet":{"check":false,"intervalSeconds":30,
        "timeoutMs":500,"failures":4,"targets":["8.8.4.4","192.168.1.1"]}})");

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    const config::InternetCheck &net = config::Get().internet;
    TEST_ASSERT_FALSE(net.check);
    TEST_ASSERT_EQUAL_UINT16(30, net.interval_seconds);
    TEST_ASSERT_EQUAL_UINT16(500, net.timeout_ms);
    TEST_ASSERT_EQUAL_UINT8(4, net.failures_before_offline);
    TEST_ASSERT_EQUAL_UINT8(2, net.target_count);
    TEST_ASSERT_EQUAL_STRING("8.8.4.4", net.targets[0]);
    TEST_ASSERT_EQUAL_STRING("192.168.1.1", net.targets[1]);
}

void test_config_defaults_ping_three_operators_once_a_minute(void) {
    // The shape §10.9 asks for, and 8.8.8.8 by name because the request was
    // for it. Three rather than one: a network that drops ICMP to a single
    // operator is common and is not an outage.
    config::Data defaults = {};
    config::FillDefaults(&defaults);
    TEST_ASSERT_TRUE(defaults.internet.check);
    TEST_ASSERT_EQUAL_UINT16(60, defaults.internet.interval_seconds);
    TEST_ASSERT_EQUAL_UINT8(3, defaults.internet.target_count);
    TEST_ASSERT_EQUAL_STRING("8.8.8.8", defaults.internet.targets[0]);
}

void test_config_drops_a_target_that_is_not_an_address(void) {
    // **A hostname is refused as firmly as a typo.** There is no resolver in
    // an ICMP echo, so `google.com` here would be a check that can never pass
    // — and a check that can never pass reads as an outage that never ends.
    FreshWithDefaults();
    fake::PutFile("config.json",
                  R"({"internet":{"targets":["google.com","8.8.8.8","10.0.0.400",42]}})");

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    TEST_ASSERT_EQUAL_UINT8(1, config::Get().internet.target_count);
    TEST_ASSERT_EQUAL_STRING("8.8.8.8", config::Get().internet.targets[0]);
}

void test_config_an_empty_target_list_means_none_not_the_defaults(void) {
    // The operator wrote it down. Falling back to the built-in list would be
    // the device pinging Google after being told not to.
    FreshWithDefaults();
    fake::PutFile("config.json", R"({"internet":{"check":true,"targets":[]}})");

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    TEST_ASSERT_EQUAL_UINT8(0, config::Get().internet.target_count);
}

void test_config_an_interval_of_zero_is_floored_not_taken(void) {
    // This is somebody else's server being asked. A probe loop with no
    // interval is a flood with a friendly name.
    FreshWithDefaults();
    fake::PutFile("config.json", R"({"internet":{"intervalSeconds":0}})");

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    TEST_ASSERT_TRUE(config::Get().internet.interval_seconds >= 5);
}

void test_config_the_internet_check_round_trips(void) {
    FreshWithDefaults();
    fake::PutFile("config.json", kGoodJson);
    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());

    config::Get().internet.check = false;
    config::Get().internet.interval_seconds = 120;
    config::Get().internet.target_count = 1;
    snprintf(config::Get().internet.targets[0], config::kIpTextSize, "9.9.9.9");
    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Save());

    config::Get().internet.check = true;
    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Reload());
    TEST_ASSERT_FALSE(config::Get().internet.check);
    TEST_ASSERT_EQUAL_UINT16(120, config::Get().internet.interval_seconds);
    TEST_ASSERT_EQUAL_UINT8(1, config::Get().internet.target_count);
    TEST_ASSERT_EQUAL_STRING("9.9.9.9", config::Get().internet.targets[0]);
}

void test_config_a_sync_interval_of_zero_is_off_not_floored(void) {
    // The mirror image of the internet check above, and deliberately the other
    // answer: a probe list with no interval is a flood, but a clock that is
    // told never to sync has said something meaningful. Flooring this to an
    // hour would be a device asking a stranger's server 24 times a day because
    // somebody typed a zero meaning "don't".
    FreshWithDefaults();
    fake::PutFile("config.json", R"({"time":{"syncHours":0}})");

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    TEST_ASSERT_EQUAL_UINT8(0, config::Get().time.sync_hours);
}

void test_config_no_sntp_server_means_no_sync_rather_than_a_guess(void) {
    // Both spellings of "there is nothing to ask": the key absent, and the key
    // present and empty. Neither may fall back to a compiled-in host — a
    // device that syncs against a server nobody wrote down is the mistake
    // `internet.targets` already refuses, and a device that keeps trying one
    // that is not there is an error every interval, forever.
    FreshWithDefaults();
    fake::PutFile("config.json", R"({"time":{"zone":"UTC","syncHours":6}})");
    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    TEST_ASSERT_EQUAL_STRING("", config::Get().time.sntp_server);

    FreshWithDefaults();
    fake::PutFile("config.json", R"({"time":{"sntp":"","syncHours":6}})");
    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    TEST_ASSERT_EQUAL_STRING("", config::Get().time.sntp_server);
}

void test_config_the_sync_interval_round_trips(void) {
    FreshWithDefaults();
    fake::PutFile("config.json", kGoodJson);
    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());

    config::Get().time.sync_hours = 24;
    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Save());

    config::Get().time.sync_hours = 1;
    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Reload());
    TEST_ASSERT_EQUAL_UINT8(24, config::Get().time.sync_hours);
    // And the file it shares a section with is intact: a writer that forgot a
    // sibling field is how a save turns into a quiet reset.
    TEST_ASSERT_EQUAL_STRING("time.google.com", config::Get().time.sntp_server);
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

    char back[config::kMaxFileSize] = {};
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

void test_config_valid_json_that_is_not_an_object_is_restored(void) {
    // **Parseable is not usable.** `[]`, `42` and `"hello"` are all valid JSON,
    // and every field lookup against them answers null — so the file used to
    // read as "an object with nothing in it": the device came up on the
    // defaults, said nothing, and left the rubbish on the filesystem to do the
    // same next boot. §10.15's restore is for exactly this, and this is the
    // shape of unusable that still parses.
    const char *const not_configs[] = {"[]", "42", "\"hello\"", "[{\"audio\":{\"volume\":9}}]"};

    for (const char *json : not_configs) {
        FreshWithDefaults();
        fake::PutFile("config.json", json);

        TEST_ASSERT_EQUAL_INT_MESSAGE(ESP_OK, config::Init(), json);
        TEST_ASSERT_TRUE(config::Loaded());

        // Restored means the file was replaced, not just that memory looks
        // sane — otherwise the next boot hits the same thing again.
        char back[config::kMaxFileSize] = {};
        TEST_ASSERT_TRUE(fake::GetFile("config.json", back, sizeof(back)));
        TEST_ASSERT_NOT_NULL_MESSAGE(std::strstr(back, "volume"), json);
        TEST_ASSERT_NOT_NULL_MESSAGE(std::strstr(back, "nats"), json);
    }
}

void test_config_a_string_too_long_for_its_field_is_refused_not_truncated(void) {
    // `CopyString` says why: a silently shortened SSID or URL fails to connect
    // and gives no hint which half of it the device is using. So the field
    // keeps the value it had — which for a boot-time parse is the default.
    FreshWithDefaults();
    char json[512] = {};
    char long_url[config::kUrlSize + 40] = {};
    std::memset(long_url, 'u', sizeof(long_url) - 1);
    snprintf(json, sizeof(json), R"({"nats":{"url":"nats://%s:4222"}})", long_url);
    fake::PutFile("config.json", json);

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());

    config::Data defaults = {};
    config::FillDefaults(&defaults);
    TEST_ASSERT_EQUAL_STRING(defaults.nats.url, config::Get().nats.url);
}

void test_config_a_network_whose_ssid_does_not_fit_is_dropped(void) {
    // The same refusal, one layer down, and here it has a second effect worth
    // pinning: a slot whose `ssid` was refused stays empty, and an empty ssid
    // is what makes the network not count. Half a network is not a network.
    FreshWithDefaults();
    fake::PutFile("config.json",
                  R"({"wifi":{"networks":[
                      {"ssid":"012345678901234567890123456789012345","password":"x"},
                      {"ssid":"desk","password":"y"}]}})");

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    TEST_ASSERT_EQUAL_UINT8(1, config::Get().wifi.network_count);
    TEST_ASSERT_EQUAL_STRING("desk", config::Get().wifi.networks[0].ssid);
}

void test_config_a_negative_number_is_clamped_and_not_wrapped(void) {
    // `CopyNumber` clamps into a `long` before the cast. Without the clamp,
    // −5 reaches `uint8_t` as 251 and −1 reaches `uint16_t` as 65535 — a
    // brightness and a blank timeout that are both wrong in a way that looks
    // deliberate.
    FreshWithDefaults();
    fake::PutFile("config.json",
                  R"({"display":{"brightness":-5,"dimSeconds":-1,"blankSeconds":99999},
                      "audio":{"volume":-20}})");

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    TEST_ASSERT_EQUAL_UINT8(0, config::Get().display.brightness);
    TEST_ASSERT_EQUAL_UINT16(0, config::Get().display.dim_seconds);
    TEST_ASSERT_EQUAL_UINT16(65535, config::Get().display.blank_seconds);
    TEST_ASSERT_EQUAL_UINT8(0, config::Get().audio.volume_percent);
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

// --- `KEY` at boot (§10.15) ------------------------------------------------
//
// The button is not here — whether it was held is the caller's answer, because
// this layer knows about a file and has never heard of a GPIO (§10.14.2). What
// *is* here is everything that answer decides, and the ordering that makes the
// button worth having at all: this runs **before** `Init()`, because the failure
// it exists for is a config that stops the device booting, and a restore that
// runs after the parse cannot rescue that.

void test_config_key_not_held_at_boot_changes_nothing(void) {
    // "Released early, nothing happens" — and nothing is the whole assertion:
    // not a read, not a write, and not a line for the operator to act on.
    FreshWithDefaults();
    fake::PutFile("config.json", kGoodJson);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(config::RestoreOutcome::kNotRequested),
                          static_cast<int>(config::RestoreAtBoot(false)));
    TEST_ASSERT_NULL(config::BootRestoreText());

    char back[1024] = {};
    TEST_ASSERT_TRUE(fake::GetFile("config.json", back, sizeof(back)));
    TEST_ASSERT_EQUAL_STRING(kGoodJson, back);

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    TEST_ASSERT_EQUAL_STRING("desk", config::Get().wifi.networks[0].ssid);
}

void test_config_key_held_at_boot_puts_the_defaults_back(void) {
    FreshWithDefaults();
    fake::PutFile("config.json", kGoodJson);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(config::RestoreOutcome::kRestored),
                          static_cast<int>(config::RestoreAtBoot(true)));

    // The boot continues, and what it reads is the file the button just wrote
    // — which is what "boot continues on the defaults" means.
    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    TEST_ASSERT_EQUAL_UINT8(0, config::Get().wifi.network_count);

    char defaults[config::kMaxFileSize] = {};
    TEST_ASSERT_TRUE(fake::GetFile("config.init.json", defaults, sizeof(defaults)));
    char back[config::kMaxFileSize] = {};
    TEST_ASSERT_TRUE(fake::GetFile("config.json", back, sizeof(back)));
    TEST_ASSERT_EQUAL_STRING(defaults, back);
}

void test_config_the_boot_restore_leaves_the_registration_alone(void) {
    // §10.15's whole reason for two files, reached by the button rather than by
    // a bad parse: settings are a minute's work, a registration is a token
    // minted on the host and typed over USB.
    FreshWithDefaults();
    fake::PutFile("config.json", kGoodJson);
    fake::PutFile("registration.json", R"({"key_id":"approver-esp32","server_key":"AAAA"})");

    TEST_ASSERT_EQUAL_INT(static_cast<int>(config::RestoreOutcome::kRestored),
                          static_cast<int>(config::RestoreAtBoot(true)));

    char back[256] = {};
    TEST_ASSERT_TRUE(fake::GetFile("registration.json", back, sizeof(back)));
    TEST_ASSERT_NOT_NULL(std::strstr(back, "approver-esp32"));
    TEST_ASSERT_NOT_NULL(std::strstr(back, "AAAA"));
}

void test_config_a_boot_restore_with_no_defaults_keeps_the_settings(void) {
    // A missing `config.init.json` is a build error (§10.15) — but the honest
    // runtime behaviour is that the restore did **not** happen, and the settings
    // that were there are still there. Destroying a working config to report a
    // missing one would be the worst of both.
    Fresh();  // no `config.init.json`
    fake::PutFile("config.json", kGoodJson);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(config::RestoreOutcome::kFailed),
                          static_cast<int>(config::RestoreAtBoot(true)));

    char back[1024] = {};
    TEST_ASSERT_TRUE(fake::GetFile("config.json", back, sizeof(back)));
    TEST_ASSERT_EQUAL_STRING(kGoodJson, back);

    TEST_ASSERT_EQUAL_INT(ESP_OK, config::Init());
    TEST_ASSERT_EQUAL_STRING("desk", config::Get().wifi.networks[0].ssid);
}

void test_config_a_boot_restore_with_no_filesystem_is_a_failure_not_a_crash(void) {
    fake::MountStorage();
    fake::UnmountStorage();

    TEST_ASSERT_EQUAL_INT(static_cast<int>(config::RestoreOutcome::kFailed),
                          static_cast<int>(config::RestoreAtBoot(true)));
}

void test_config_the_boot_restore_says_which_of_the_three_happened(void) {
    // §10.15: "a restore the operator cannot confirm is a restore they will do
    // twice". One string, so the screen and the console cannot drift apart.
    FreshWithDefaults();
    fake::PutFile("config.json", kGoodJson);

    config::RestoreAtBoot(false);
    TEST_ASSERT_NULL(config::BootRestoreText());

    config::RestoreAtBoot(true);
    TEST_ASSERT_NOT_NULL(config::BootRestoreText());
    const char *restored = config::BootRestoreText();

    Fresh();
    fake::PutFile("config.json", kGoodJson);
    config::RestoreAtBoot(true);
    TEST_ASSERT_NOT_NULL(config::BootRestoreText());
    // Two different outcomes must not read as the same sentence.
    TEST_ASSERT_NOT_EQUAL(0, std::strcmp(restored, config::BootRestoreText()));
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

    RUN_TEST(test_config_reads_the_wifi_mode_and_the_fallback_settings);
    RUN_TEST(test_config_an_unknown_wifi_mode_keeps_the_default);
    RUN_TEST(test_config_the_access_point_settings_round_trip);
    RUN_TEST(test_config_a_file_with_no_wifi_section_still_has_an_access_point);

    RUN_TEST(test_config_parses_a_dotted_quad_first_octet_in_the_low_byte);
    RUN_TEST(test_config_refuses_an_address_that_is_not_one);
    RUN_TEST(test_config_refuses_a_leading_zero_rather_than_reading_it_as_octal);
    RUN_TEST(test_config_reads_a_static_address_for_one_network);
    RUN_TEST(test_config_a_static_block_with_no_dns_is_still_static);
    RUN_TEST(test_config_a_broken_static_address_falls_back_to_dhcp);
    RUN_TEST(test_config_a_bad_dns_entry_does_not_disable_the_address);
    RUN_TEST(test_config_a_static_address_round_trips);
    RUN_TEST(test_config_a_network_on_dhcp_writes_no_ip_block);

    RUN_TEST(test_config_reads_the_internet_check);
    RUN_TEST(test_config_defaults_ping_three_operators_once_a_minute);
    RUN_TEST(test_config_drops_a_target_that_is_not_an_address);
    RUN_TEST(test_config_an_empty_target_list_means_none_not_the_defaults);
    RUN_TEST(test_config_an_interval_of_zero_is_floored_not_taken);
    RUN_TEST(test_config_the_internet_check_round_trips);
    RUN_TEST(test_config_a_sync_interval_of_zero_is_off_not_floored);
    RUN_TEST(test_config_no_sntp_server_means_no_sync_rather_than_a_guess);
    RUN_TEST(test_config_the_sync_interval_round_trips);

    RUN_TEST(test_config_a_string_too_long_for_its_field_is_refused_not_truncated);
    RUN_TEST(test_config_a_network_whose_ssid_does_not_fit_is_dropped);
    RUN_TEST(test_config_a_negative_number_is_clamped_and_not_wrapped);

    RUN_TEST(test_config_a_missing_file_is_restored);
    RUN_TEST(test_config_a_file_that_is_not_json_is_restored);
    RUN_TEST(test_config_valid_json_that_is_not_an_object_is_restored);
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

    RUN_TEST(test_config_key_not_held_at_boot_changes_nothing);
    RUN_TEST(test_config_key_held_at_boot_puts_the_defaults_back);
    RUN_TEST(test_config_the_boot_restore_leaves_the_registration_alone);
    RUN_TEST(test_config_a_boot_restore_with_no_defaults_keeps_the_settings);
    RUN_TEST(test_config_a_boot_restore_with_no_filesystem_is_a_failure_not_a_crash);
    RUN_TEST(test_config_the_boot_restore_says_which_of_the_three_happened);

    RUN_TEST(test_config_a_named_zone_fills_in_its_posix_rule);
    RUN_TEST(test_config_an_unknown_zone_does_not_silently_become_utc);
}
