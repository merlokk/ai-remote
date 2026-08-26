#include "web_settings.h"

#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "endpoint.h"

namespace web {
namespace {

// The words the page sends for the radio, against this file's own enum —
// `web_settings.h` says why the mapping lives here rather than in `ui`, and the
// short form is that there is no Wi-Fi screen on this board to own it.
struct ModeWord {
    const char *word;
    WifiWord mode;
};

constexpr ModeWord kModes[] = {
    {"off", WifiWord::kOff},
    {"client", WifiWord::kClient},
    {"ap", WifiWord::kAp},
};

void Say(WriteOutcome *out, WriteResult result, const char *detail) {
    out->result = result;
    std::snprintf(out->detail, sizeof out->detail, "%s", detail != nullptr ? detail : "");
}

// A string field, refused rather than truncated. `config::CopyString` makes the
// same call inside the parser and says why: a half-length SSID fails to connect
// and gives no hint which half is being used.
bool CopyField(const cJSON *value, char *field, size_t size, WriteOutcome *out,
               const char *name) {
    if (!cJSON_IsString(value) || value->valuestring == nullptr) {
        Say(out, WriteResult::kBadValue, name);
        return false;
    }
    if (std::strlen(value->valuestring) >= size) {
        Say(out, WriteResult::kTooLong, name);
        return false;
    }
    std::snprintf(field, size, "%s", value->valuestring);
    return true;
}

// **A password that was not retyped keeps the one that is there.** Absent and
// `null` both mean that; an empty string is a deliberate "this network is open".
// The old value comes from the caller because it is matched by SSID against the
// list as it stands, which is the only way a form that never sees a password can
// leave one alone.
bool CopyPassword(const cJSON *object, char *field, size_t size, const char *keep,
                  WriteOutcome *out, const char *name) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, "password");
    if (value == nullptr || cJSON_IsNull(value)) {
        std::snprintf(field, size, "%s", keep != nullptr ? keep : "");
        return true;
    }
    return CopyField(value, field, size, out, name);
}

bool ReadIp(const cJSON *ip, config::StaticIp *into, const config::StaticIp &keep,
            WriteOutcome *out) {
    // **An absent block keeps the address that is there**, which is the password
    // rule of this file applied to the other field a form does not carry — and it
    // is a bug the sibling board had before its own board found it: the Wi-Fi page
    // has no address fields, so *every* Apply was quietly forgetting a static
    // address somebody had typed on the console. Inherited as a fix rather than
    // re-learned here.
    //
    // An `ip` block that *is* sent replaces it, and `"static": false` inside one is
    // how a network is put back on DHCP (§10.9's own default).
    if (ip == nullptr || cJSON_IsNull(ip)) {
        *into = keep;
        return true;
    }
    *into = {};
    if (!cJSON_IsObject(ip)) {
        Say(out, WriteResult::kBadValue, "wifi.networks[].ip");
        return false;
    }

    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(ip, "static");
    into->enabled = cJSON_IsTrue(enabled);

    struct Field {
        const char *name;
        char *into;
        bool required;
    };
    const Field fields[] = {
        {"address", into->address, true},
        {"netmask", into->netmask, true},
        {"gateway", into->gateway, true},
        {"dns1", into->dns1, false},
        {"dns2", into->dns2, false},
    };
    for (const Field &field : fields) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(ip, field.name);
        if (value == nullptr || cJSON_IsNull(value)) {
            if (into->enabled && field.required) {
                Say(out, WriteResult::kBadValue, field.name);
                return false;
            }
            continue;
        }
        if (!CopyField(value, field.into, config::kIpTextSize, out, field.name)) {
            return false;
        }
        // **Parsed rather than stored and hoped for**, which is §10.9's whole
        // argument about `010`: the config layer already has the strict parser, and
        // the moment to refuse a typo is while somebody is still looking at the
        // form. An empty DNS entry is not an address and is not one being asked
        // for.
        uint32_t ignored = 0;
        if (field.into[0] != '\0' && !config::ParseIpv4(field.into, &ignored)) {
            Say(out, WriteResult::kBadValue, field.into);
            return false;
        }
    }
    return true;
}

bool ReadWifi(const cJSON *wifi, config::Data *into, WriteOutcome *out) {
    if (!cJSON_IsObject(wifi)) {
        Say(out, WriteResult::kBadValue, "wifi");
        return false;
    }

    // The list as it stands, so a password nobody retyped can be found again.
    const config::Wifi before = into->wifi;

    for (const cJSON *item = wifi->child; item != nullptr; item = item->next) {
        const char *key = item->string;
        if (key == nullptr) {
            Say(out, WriteResult::kBadValue, "wifi");
            return false;
        }

        if (std::strcmp(key, "mode") == 0) {
            if (!cJSON_IsString(item) || item->valuestring == nullptr) {
                Say(out, WriteResult::kBadValue, "wifi.mode");
                return false;
            }
            WifiWord word = WifiWord::kOff;
            if (!WifiWordFrom(item->valuestring, &word)) {
                // §10.9's rule about an unknown mode, arriving over HTTP: refused,
                // never guessed. "yes" is not "on".
                Say(out, WriteResult::kBadValue, item->valuestring);
                return false;
            }
            // **Off leaves `wifi.mode` alone**, which is the sibling board's rule
            // and not an omission here either: off is a statement about the radio,
            // and writing the mode as well would move which record the *device*
            // comes back on for an operator who only switched the radio off.
            into->wifi.active = word != WifiWord::kOff;
            if (into->wifi.active) {
                into->wifi.mode = word == WifiWord::kAp ? config::WifiMode::kAp
                                                        : config::WifiMode::kClient;
            }
            continue;
        }

        if (std::strcmp(key, "ap") == 0) {
            if (!cJSON_IsObject(item)) {
                Say(out, WriteResult::kBadValue, "wifi.ap");
                return false;
            }
            const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(item, "ssid");
            if (ssid != nullptr &&
                !CopyField(ssid, into->wifi.ap_ssid, config::kSsidSize, out, "wifi.ap.ssid")) {
                return false;
            }
            if (!CopyPassword(item, into->wifi.ap_password, config::kPasswordSize,
                              before.ap_password, out, "wifi.ap.password")) {
                return false;
            }
            continue;
        }

        if (std::strcmp(key, "networks") == 0) {
            if (!cJSON_IsArray(item)) {
                Say(out, WriteResult::kBadValue, "wifi.networks");
                return false;
            }
            const int count = cJSON_GetArraySize(item);
            if (count > static_cast<int>(config::kMaxNetworks)) {
                // Refused with the bound rather than truncated: a list that came
                // back one network shorter than it was submitted is a network the
                // operator thinks this device knows about.
                Say(out, WriteResult::kTooMany, "wifi.networks");
                return false;
            }

            config::Network fresh[config::kMaxNetworks] = {};
            uint8_t written = 0;
            for (const cJSON *entry = item->child; entry != nullptr; entry = entry->next) {
                if (!cJSON_IsObject(entry)) {
                    Say(out, WriteResult::kBadValue, "wifi.networks[]");
                    return false;
                }
                // **The bound again, inside the loop.** `cJSON_GetArraySize` above
                // has already refused a list that is too long, so this cannot fire —
                // and it is here because on the sibling board the mutation that
                // broke the check above wrote past the end of `fresh` rather than
                // failing a test. A second comparison is cheaper than trusting the
                // first one to stay correct.
                if (written >= config::kMaxNetworks) {
                    Say(out, WriteResult::kTooMany, "wifi.networks");
                    return false;
                }
                config::Network &network = fresh[written];
                const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(entry, "ssid");
                if (!CopyField(ssid, network.ssid, config::kSsidSize, out,
                               "wifi.networks[].ssid")) {
                    return false;
                }
                if (network.ssid[0] == '\0') {
                    // A nameless network is not a network. It is what an empty row
                    // in the form looks like, and dropping it silently is how a
                    // list comes back shorter than it was sent.
                    Say(out, WriteResult::kBadValue, "wifi.networks[].ssid");
                    return false;
                }

                // The password this network already had, if this device knows it
                // under that name.
                const char *keep = "";
                for (uint8_t i = 0; i < before.network_count; ++i) {
                    if (std::strcmp(before.networks[i].ssid, network.ssid) == 0) {
                        keep = before.networks[i].password;
                        break;
                    }
                }
                if (!CopyPassword(entry, network.password, config::kPasswordSize, keep, out,
                                  "wifi.networks[].password")) {
                    return false;
                }
                // The address block this network already had, found the same way its
                // password is: by name.
                config::StaticIp keep_ip = {};
                for (uint8_t i = 0; i < before.network_count; ++i) {
                    if (std::strcmp(before.networks[i].ssid, network.ssid) == 0) {
                        keep_ip = before.networks[i].ip;
                        break;
                    }
                }
                if (!ReadIp(cJSON_GetObjectItemCaseSensitive(entry, "ip"), &network.ip, keep_ip,
                            out)) {
                    return false;
                }
                ++written;
            }

            for (uint8_t i = 0; i < config::kMaxNetworks; ++i) {
                into->wifi.networks[i] = fresh[i];
            }
            into->wifi.network_count = written;
            continue;
        }

        // Not on the list. Named, so a page with a typo in it says which one.
        Say(out, WriteResult::kUnknownField, key);
        return false;
    }

    out->wifi_changed = true;
    return true;
}

bool ReadNats(const cJSON *nats, config::Data *into, WriteOutcome *out) {
    if (!cJSON_IsObject(nats)) {
        Say(out, WriteResult::kBadValue, "nats");
        return false;
    }
    for (const cJSON *item = nats->child; item != nullptr; item = item->next) {
        if (item->string == nullptr || std::strcmp(item->string, "url") != 0) {
            Say(out, WriteResult::kUnknownField, item->string != nullptr ? item->string : "nats");
            return false;
        }
        char url[config::kUrlSize] = {};
        if (!CopyField(item, url, sizeof url, out, "nats.url")) {
            return false;
        }
        // **Empty is off, and anything else has to parse** — the same pair the
        // console's `nats url` makes, and for its reason: a URL that will not parse
        // is a bus the device silently never connects to, which looks exactly like
        // a bus that is down.
        if (url[0] != '\0') {
            nats::Endpoint parsed = {};
            if (!nats::ParseUrl(url, &parsed)) {
                Say(out, WriteResult::kBadValue, url);
                return false;
            }
        }
        std::snprintf(into->nats.url, sizeof into->nats.url, "%s", url);
    }
    out->nats_changed = true;
    return true;
}

}  // namespace

bool WifiWordFrom(const char *word, WifiWord *out) {
    if (word == nullptr || out == nullptr) {
        return false;
    }
    for (const ModeWord &entry : kModes) {
        if (std::strcmp(entry.word, word) == 0) {
            *out = entry.mode;
            return true;
        }
    }
    return false;
}

const char *WifiWordName(bool active, bool is_ap) {
    if (!active) {
        return "off";
    }
    return is_ap ? "ap" : "client";
}

WriteOutcome ApplySettings(const char *body, size_t length, config::Data *into) {
    WriteOutcome out;
    if (body == nullptr || into == nullptr) {
        Say(&out, WriteResult::kNotJson, "");
        return out;
    }
    if (length > kMaxSettingsBody) {
        Say(&out, WriteResult::kTooBig, "");
        return out;
    }

    cJSON *root = cJSON_ParseWithLength(body, length);
    if (root == nullptr) {
        Say(&out, WriteResult::kNotJson, "");
        return out;
    }
    if (!cJSON_IsObject(root)) {
        // Valid JSON that is not an object — `[]`, `42`, `"hello"`. §10.15 records
        // this exact hole in the config parser, where every field lookup answered
        // null and a device came up on the defaults saying nothing.
        Say(&out, WriteResult::kNotObject, "");
        cJSON_Delete(root);
        return out;
    }

    // **A copy, so that a refusal changes nothing.** Half of a form applied is the
    // one outcome nobody can reason about afterwards, and the sections below refuse
    // at the first thing they do not like.
    config::Data staged = *into;
    bool ok = true;
    for (const cJSON *section = root->child; section != nullptr && ok; section = section->next) {
        const char *key = section->string;
        if (key == nullptr) {
            Say(&out, WriteResult::kBadValue, "");
            ok = false;
        } else if (std::strcmp(key, "wifi") == 0) {
            ok = ReadWifi(section, &staged, &out);
        } else if (std::strcmp(key, "nats") == 0) {
            ok = ReadNats(section, &staged, &out);
        } else {
            // **`approval`, `led` and `web` land here, by name.** That is the
            // §10.10 half of `web_settings.h`: the gate's timeout, the deny button,
            // the light's brightness and the site's own credential are all in this
            // file and none of them is reachable from a network.
            Say(&out, WriteResult::kUnknownField, key);
            ok = false;
        }
    }
    cJSON_Delete(root);

    if (!ok) {
        out.wifi_changed = false;
        out.nats_changed = false;
        return out;
    }
    *into = staged;
    out.result = WriteResult::kOk;
    return out;
}

const char *WriteResultText(WriteResult result) {
    switch (result) {
        case WriteResult::kOk:
            return "ok";
        case WriteResult::kTooBig:
            return "that is more settings than this device will read at once";
        case WriteResult::kNotJson:
            return "that did not parse";
        case WriteResult::kNotObject:
            return "that is not a settings document";
        case WriteResult::kUnknownField:
            return "this device has no such setting";
        case WriteResult::kBadValue:
            return "that value is not one this device can use";
        case WriteResult::kTooLong:
            return "that is longer than the field it goes in";
        case WriteResult::kTooMany:
            return "this device remembers four networks";
    }
    return "refused";
}

Action ActionFromUri(const char *uri) {
    if (uri == nullptr) {
        return Action::kNone;
    }
    static constexpr char kKey[] = "do=";
    constexpr size_t kKeyLength = sizeof kKey - 1;

    const char *at = uri;
    while (*at != '\0' && *at != '?' && *at != '#') {
        ++at;
    }
    if (*at != '?') {
        return Action::kNone;
    }
    ++at;

    struct Verb {
        const char *word;
        Action action;
    };
    static constexpr Verb kVerbs[] = {
        {"save", Action::kSave},
        {"reload", Action::kReload},
        {"retry", Action::kWifiRetry},
        {"reconnect", Action::kBusRetry},
    };

    for (;;) {
        size_t length = 0;
        while (at[length] != '\0' && at[length] != '&' && at[length] != '#') {
            ++length;
        }
        if (length > kKeyLength && std::strncmp(at, kKey, kKeyLength) == 0) {
            const char *value = at + kKeyLength;
            const size_t value_length = length - kKeyLength;
            for (const Verb &verb : kVerbs) {
                if (std::strlen(verb.word) == value_length &&
                    std::strncmp(value, verb.word, value_length) == 0) {
                    return verb.action;
                }
            }
            // A `do=` that names nothing. Refused rather than falling through to a
            // later parameter that might: one `do=` decides.
            return Action::kNone;
        }
        if (at[length] != '&') {
            return Action::kNone;
        }
        at += length + 1;
    }
}

const char *ActionName(Action action) {
    switch (action) {
        case Action::kNone:
            return "nothing";
        case Action::kSave:
            return "save";
        case Action::kReload:
            return "reload";
        case Action::kWifiRetry:
            return "retry";
        case Action::kBusRetry:
            return "reconnect";
    }
    return "nothing";
}

}  // namespace web
