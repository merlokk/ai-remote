#include "web_server.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "board.h"
#include "config.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nats_link.h"
#include "registrar.h"
#include "storage.h"
#include "web_auth.h"
#include "web_paths.h"
#include "web_settings.h"
#include "wifi_manager.h"

namespace web {
namespace {

constexpr const char *TAG = "web";

// This file is the only place that can tie the two together: `web_paths.h`
// includes nothing and `storage.h` has never heard of a URL.
static_assert(kMaxNameLength == storage::kMaxNameLength,
              "a URL could name a file SPIFFS cannot store");

// And the same tie for the credential: `web_auth.h` includes `<cstddef>` and
// `<cstdint>` and has never heard of `config.json`, so this is the one place that
// can say the buffer it sizes its base64 from is the pair of fields the operator
// actually fills in. A field grown without this number is a device nobody can log
// into, refused inside `BasicCredential` with no way to see why.
static_assert(config::kWebUserSize - 1 + 1 + config::kWebPasswordSize - 1 + 1 ==
                  kMaxCredentialSize,
              "the credential fields and the buffer that encodes them have drifted");

httpd_handle_t g_server = nullptr;

// **The handle above is changed from two tasks, and that is what this guards.**
// `Maintain()` runs on the Wi-Fi manager's task five times a second; `web cycle`
// runs on the console's; both call `Start`/`Stop`. Two tasks passing the same
// `g_server == nullptr` check is two `httpd_stop` calls on one handle, which is
// `httpd_delete` freeing the same four blocks twice — and the fault lands later,
// inside the allocator, in whichever task calls `free` next. §10.16 has the
// decoded stack.
//
// Static, like every other FreeRTOS object here (§10.14.1), and **not
// recursive** — which is why the work below is split into `…Locked` halves the
// way `Es8311`'s is (§10.14.3): the public entry points take it, and nothing
// that already holds it calls something that would take it again.
StaticSemaphore_t g_lock_storage;
SemaphoreHandle_t g_lock = nullptr;

// **Somebody else owns the lifetime right now.** Only `web cycle` sets this, and
// only for the length of its loop: while it is true the reconciler answers
// `kNothing` whatever the world does, so the diagnostic's rounds are its own
// rather than a race with a tick. `web_policy.h` argues it where the rule lives.
bool g_held = false;

// Guards the handle for the length of a call. A lock this device cannot fail to
// take — every path here is a start or a stop, both bounded — so the wait is
// `portMAX_DELAY` rather than §10.14.3's timeout-and-skip, which is the right
// answer for a bus that a dropped frame can survive and the wrong one for a
// pointer that must not be freed twice.
class Held {
  public:
    Held() { xSemaphoreTake(g_lock, portMAX_DELAY); }
    ~Held() { xSemaphoreGive(g_lock); }
    Held(const Held &) = delete;
    Held &operator=(const Held &) = delete;
};

// **One buffer, in `.bss`, and it is safe because `esp_http_server` is one
// task.** The instance serves one request at a time — that is what
// `max_open_sockets` bounds, not concurrency — so the chunk buffer cannot be
// re-entered. The alternative is 1 KB on the server's own stack, which is what
// the house firmware of §10.14.4 does with 2,500 and then has to raise the stack
// for (§10.14.1: nothing of ours allocates, and nothing large goes on a stack).
char g_chunk[kChunkBytes];

// The JSON of `/api/status`, built with `snprintf` rather than cJSON: the fields
// are fixed, there is no user input in it, and a document assembled on the heap
// would be one more thing to leak in the measurement this component exists for.
//
// **One kilobyte, in `.bss`, and the whole document is written in one call** —
// which is what makes the size a compile-time question rather than a runtime one:
// `snprintf` truncates, so the worst a document that outgrew this can do is arrive
// as invalid JSON that the page reports as such. It is measured rather than
// guessed: the fields below come to a little over 500 bytes with the longest SSID
// and key id this device can hold.
char g_json[1024];

Status g_status;

// **One buffer for a form on its way in and a document on its way out**, because
// no request is ever both: the POST reads a body and answers a sentence, the GET
// writes a document and reads nothing. `esp_http_server` serves one request at a
// time — that is what `max_open_sockets` bounds, not concurrency — so this cannot
// be re-entered, which is the argument `g_chunk` above already makes.
//
// In `.bss`, never on the server's 4 KB stack, and never grown to fit what
// somebody sent (§10.14.1). 2 KB holds four networks with a 32-byte name, a
// 64-byte key and an address block each, with room for whitespace.
char g_body[kMaxSettingsBody + 1];
char *const g_wifi_json = g_body;
constexpr size_t kWifiJsonSize = kMaxSettingsBody + 1;

// The scan results, as `wifimgr::Scan` fills them. **Static because a scan is
// 16 × ~40 bytes and the server task has 4 KB of stack**, and the handler is the
// only thing that touches them.
constexpr size_t kMaxScan = 10;
wifi::ScanResult g_scan[kMaxScan];

// **The `Authorization` header on its way in.** In `.bss` like every other
// buffer here, and bounded before a byte is read: `httpd_req_get_hdr_value_len`
// is asked first, and a value longer than this is refused as a wrong credential
// rather than read into anything. A header is the one field whose length whoever
// sent it chooses.
//
// `Basic ` plus the longest credential this device can hold, plus room for the
// extra whitespace a hand-written client leaves around it - which
// `web_auth.cpp` tolerates and therefore has to be able to see.
constexpr size_t kMaxAuthHeader = kMaxEncodedSize + 24;
char g_auth[kMaxAuthHeader];

DiagnosticsPrinter g_diagnostics = nullptr;

// Where the approval counters come from, and why it is a hook rather than a call:
// `web_server.h` has the cycle that makes it one.
ApprovalsProbe g_approvals = nullptr;

// **A `FILE *` that writes into the response, and it has to know whose printf it
// is.** `picolibc`'s `stdout` is one global pointer, not a per-task one — so for
// the length of the dump, *every* task's `printf` arrives here, including an
// `ESP_LOG` line from the screen task. Two things follow, and the second is the
// one that matters:
//
//   * a foreign line must not land in the middle of the response — it would be
//     read as part of the dump;
//   * and it absolutely must not reach `httpd_resp_send_chunk`, which would then
//     be writing to the socket from a task that does not own the request.
//
// So the cookie records the task that opened it. Its own writes become chunks;
// anybody else's go where they were going in the first place — the console —
// which is also why nothing is lost from the boot log while a page is served.
struct Sink {
    httpd_req_t *request;
    TaskHandle_t owner;
    FILE *console;
    bool broken;
};

Sink g_sink;

int SinkWrite(void *cookie, const char *data, int size) {
    Sink *sink = static_cast<Sink *>(cookie);
    if (size <= 0) {
        return 0;
    }
    if (xTaskGetCurrentTaskHandle() != sink->owner) {
        if (sink->console != nullptr) {
            std::fwrite(data, 1, static_cast<size_t>(size), sink->console);
        }
        return size;
    }
    if (sink->broken) {
        return size;  // the browser went away; swallow the rest of the dump
    }
    if (httpd_resp_send_chunk(sink->request, data, static_cast<size_t>(size)) != ESP_OK) {
        sink->broken = true;
    }
    return size;
}

// What was asked for, and when it is worth asking the framework again.
Desired g_desired = Desired::kAuto;

// **A failed start is not retried five times a second.** `Maintain` runs on the
// Wi-Fi manager's 200 ms tick, so a server that cannot start — a port already
// taken, a heap too small — would otherwise be a log line every fifth of a
// second for the life of the device. Five seconds, and a state change clears it
// (`SetDesired` and `Apply` both do), because an operator who just typed
// something expects it to be tried now.
constexpr uint32_t kRetryAfterMs = 5000;
uint32_t g_retry_at_ms = 0;
bool g_retry_pending = false;

void Count(uint32_t *field) { *field = *field + 1; }

// The heap numbers, in one place so the three call sites cannot drift.
void Sample() {
    g_status.free_now = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    g_status.low_water =
        static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}

// **A page rather than a sentence, when there is one on the filesystem.** A
// phone that arrived at the wrong URL - or at a locked one - has no back button
// that helps and no console to type in, so the page carries a link home. It is
// streamed through the same chunk buffer as any other file, and the name is a
// constant from `web_paths.h` rather than spelled here: the whitelist has an
// opinion about these two files as well, which is the hole this shape keeps shut.
//
// False when there is no such file, so the caller can fall back to its sentence -
// which is what a device flashed before the page existed answers. `*err` carries
// what went wrong on the wire and means anything only when this returned true.
bool SendStatusPage(httpd_req_t *request, const char *name, esp_err_t *err) {
    char path[storage::kMaxPathLength] = {};
    if (!storage::ResolvePath(name, path, sizeof path)) {
        return false;
    }
    FILE *page = std::fopen(path, "rb");
    if (page == nullptr) {
        return false;
    }
    httpd_resp_set_type(request, ContentType(name));
    for (;;) {
        const size_t read = std::fread(g_chunk, 1, sizeof g_chunk, page);
        if (read == 0) {
            break;
        }
        if (httpd_resp_send_chunk(request, g_chunk, read) != ESP_OK) {
            std::fclose(page);
            *err = ESP_FAIL;
            return true;
        }
    }
    std::fclose(page);
    *err = httpd_resp_send_chunk(request, nullptr, 0);
    return true;
}

// A refusal and a missing file are **the same answer to whoever asked**, which is
// §10.16's rule: "that extension is not served" tells somebody probing for
// `config.json` that it is there. The difference is counted and logged on this
// side and nowhere else.

esp_err_t NotFound(httpd_req_t *request) {
    httpd_resp_set_status(request, "404 Not Found");

    esp_err_t err = ESP_OK;
    if (SendStatusPage(request, kNotFoundName, &err)) {
        return err;
    }

    // **And the sentence when there is not**, which is what a device flashed
    // before that file existed answers: a 404 that failed to be a 404 would be
    // the confusing outcome.
    httpd_resp_set_type(request, "text/plain");
    return httpd_resp_sendstr(request, "not found\n");
}

// --- The gate (CLAUDE.md §10.16) ------------------------------------------
//
// **Every route goes through this, the reads included.** `web.write` already
// refuses the forms; what it cannot do is keep `/api/devstatus` - a few hundred
// lines about this device, its bus and its registration - off a network. So with
// a credential configured, nothing is served without one, and with none
// configured nothing changes at all. `web_auth.h` has that rule and the three
// others; what is here is the socket.
esp_err_t Unauthorised(httpd_req_t *request) {
    Count(&g_status.unauthorised);
    httpd_resp_set_status(request, "401 Unauthorized");
    // **This header is what makes a browser ask.** Without it a phone shows the
    // body and no dialog, and there is then no way to type a password into this
    // device at all - which is the one mistake that turns authentication into a
    // lockout.
    std::snprintf(g_json, sizeof g_json, "Basic realm=\"%s\", charset=\"UTF-8\"", kAuthRealm);
    httpd_resp_set_hdr(request, "WWW-Authenticate", g_json);
    // And nothing about a refused request is cached by anything, ever - unlike the
    // pages, which are cached for a day (`File` below says why).
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");

    esp_err_t err = ESP_OK;
    if (SendStatusPage(request, kUnauthorisedName, &err)) {
        return err;
    }
    httpd_resp_set_type(request, "text/plain");
    return httpd_resp_sendstr(request, "unauthorized\n");
}

// True when this request may proceed. **It fails closed on every path it cannot
// answer** - a header longer than this device can hold, a read that comes back
// short - because the other kind of mistake is a network rather than a retype
// (`web_auth.h`, rule 3).
//
// The credential is read out of the live config on each request rather than
// cached at start, so `config set` and `config reload` take effect without the
// server having to be cycled - the same call every other reader here makes.
bool Allowed(httpd_req_t *request) {
    const config::Web &web = config::Get().web;
    if (!AuthRequired(web.user, web.password)) {
        return true;
    }

    const size_t length = httpd_req_get_hdr_value_len(request, "Authorization");
    if (length == 0 || length + 1 > sizeof g_auth) {
        return false;
    }
    if (httpd_req_get_hdr_value_str(request, "Authorization", g_auth, sizeof g_auth) != ESP_OK) {
        return false;
    }
    return Authorised(web.user, web.password, g_auth);
}

// --- The handlers --------------------------------------------------------

esp_err_t StatusApi(httpd_req_t *request) {
    // **The gate, and it is the first statement in every handler** (§10.16): a
    // request with no credential must not reach a counter, a buffer or the I2C
    // bus. With nothing configured this is a comparison against two empty strings
    // and lets everybody through, which is what it did before authentication
    // existed.
    if (!Allowed(request)) {
        return Unauthorised(request);
    }

    Count(&g_status.api);
    Sample();

    const esp_app_desc_t *app = esp_app_get_description();
    const wifimgr::Snapshot wifi = wifimgr::Get();
    size_t total = 0;
    size_t used = 0;
    storage::Info(&total, &used);

    const nats::Status bus = nats::Get();
    // Zeros and an empty reason on a device where nobody registered a probe, which
    // is a device with no responder rather than a device with nothing to say.
    Approvals approvals;
    if (g_approvals != nullptr) {
        g_approvals(&approvals);
    }
    // **The battery is an I2C read from the httpd task**, under the lease like
    // every other one (§10.14.3) - the `devstatus` dump next door already does a
    // dozen of them from here, and a lease it cannot get costs this document one
    // field rather than the request.
    pmic::Status battery = {};
    const bool battery_read =
        board::Pmic().Present() && board::Pmic().Read(&battery) == ESP_OK;

    // **No password, and no key.** §10.15 keeps a passphrase out of every log
    // line and every console dump; a URL is a wider audience than either. What is
    // here is state - the same class of thing the status pages of §10.8.5 show,
    // and the `key_id` is a public name rather than a secret (§10.2).
    //
    // The three groups are what the front page is *for*: whether this device could
    // answer a request at all (the bus, the registration, the subscription), what
    // it has answered (§7's counters), and whether it is healthy (heap, uptime,
    // battery).
    std::snprintf(g_json, sizeof g_json,
                  "{\"firmware\":\"%s\",\"idf\":\"%s\",\"uptime_s\":%" PRIu32
                  ",\"heap_free\":%" PRIu32 ",\"heap_low\":%" PRIu32 ",\"spiffs_used\":%u"
                  ",\"spiffs_total\":%u,\"wifi\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d"
                  ",\"ip\":\"%u.%u.%u.%u\""
                  ",\"bus\":\"%s\",\"bus_up\":%s,\"registered\":%s,\"key_id\":\"%s\""
                  ",\"ready\":%s,\"subscribed\":%s,\"blocked_by\":\"%s\""
                  ",\"received\":%" PRIu32 ",\"allowed\":%" PRIu32 ",\"denied\":%" PRIu32
                  ",\"replied\":%" PRIu32
                  ",\"battery\":%d,\"battery_mv\":%u,\"charging\":%s,\"usb\":%s"
                  ",\"web_files\":%" PRIu32 ",\"web_api\":%" PRIu32 "}",
                  app != nullptr ? app->version : "?", app != nullptr ? app->idf_ver : "?",
                  static_cast<uint32_t>(esp_timer_get_time() / 1000000),
                  g_status.free_now, g_status.low_water, static_cast<unsigned>(used),
                  static_cast<unsigned>(total), wifimgr::Name(wifi.state), wifi.radio.ssid,
                  static_cast<int>(wifi.radio.rssi),
                  static_cast<unsigned>(wifi.radio.ip & 0xFF),
                  static_cast<unsigned>((wifi.radio.ip >> 8) & 0xFF),
                  static_cast<unsigned>((wifi.radio.ip >> 16) & 0xFF),
                  static_cast<unsigned>((wifi.radio.ip >> 24) & 0xFF),
                  nats::Name(bus.state),
                  bus.state == nats::State::kConnected ? "true" : "false",
                  registration::Registered() ? "true" : "false", registration::KeyId(),
                  approvals.ready ? "true" : "false", approvals.subscribed ? "true" : "false",
                  approvals.blocked_by, approvals.received,
                  approvals.allowed, approvals.denied, approvals.replied,
                  battery_read ? battery.battery_percent : -1,
                  static_cast<unsigned>(battery_read ? battery.battery_mv : 0),
                  battery_read && battery.charging ? "true" : "false",
                  battery_read && battery.vbus_present ? "true" : "false", g_status.requests,
                  g_status.api);

    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, g_json);
}

// `devstatus`, the console's own dump, over HTTP (§10.16).
esp_err_t DevStatusApi(httpd_req_t *request) {
    // **The gate, and it is the first statement in every handler** (§10.16): a
    // request with no credential must not reach a counter, a buffer or the I2C
    // bus. With nothing configured this is a comparison against two empty strings
    // and lets everybody through, which is what it did before authentication
    // existed.
    if (!Allowed(request)) {
        return Unauthorised(request);
    }

    Count(&g_status.api);
    if (g_diagnostics == nullptr) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        httpd_resp_set_type(request, "text/plain");
        return httpd_resp_sendstr(request, "no diagnostics wired up\n");
    }

    httpd_resp_set_type(request, "text/plain");
    g_sink.request = request;
    g_sink.owner = xTaskGetCurrentTaskHandle();
    g_sink.console = stdout;
    g_sink.broken = false;

    FILE *sink = funopen(&g_sink, nullptr, SinkWrite, nullptr, nullptr);
    if (sink == nullptr) {
        httpd_resp_set_status(request, "500 Internal Server Error");
        return httpd_resp_sendstr(request, "no stream\n");
    }
    // **Unbuffered, deliberately.** A buffer here would be shared with whatever
    // other task's `printf` the global `stdout` catches, and the whole point of
    // the cookie above is that foreign text never ends up in this response. The
    // cost is one small chunk per `printf` — a few hundred of them for the whole
    // dump, which is a diagnostic nobody is timing.
    setvbuf(sink, nullptr, _IONBF, 0);

    FILE *saved = stdout;
    stdout = sink;
    g_diagnostics();
    stdout = saved;
    std::fclose(sink);

    Sample();
    // The httpd task's own margin, measured while it was doing the heaviest thing
    // it does — reading a dozen I²C registers through the lease and formatting a
    // few hundred lines (§10.14.1: the low-water mark is the number).
    g_status.task_stack_low =
        static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t));
    if (g_sink.broken) {
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(request, nullptr, 0);
}

// --- The settings pages (§10.16) ------------------------------------------

// Whether a form may be submitted at all. `config.json`'s `web.write`, and the
// one field a form cannot reach (`web_settings.h` — it is refused by the
// whitelist, which is a test).
bool WritingAllowed() { return config::Get().web.write; }

esp_err_t Refuse(httpd_req_t *request, const char *status, const char *why) {
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    // The reason travels as a string the page can show. It says what is wrong with
    // the *document*, never what is on the filesystem.
    std::snprintf(g_json, sizeof g_json, "{\"ok\":false,\"error\":\"%s\"}", why);
    return httpd_resp_sendstr(request, g_json);
}

// **The same document the POST takes, read back** — `wifi` and `nats`, in the
// shape `web_settings.h` whitelists, so a page fills a form from the answer and
// submits the answer. Two things travel with it that are not settings: `writable`,
// so a read-only device can grey its own buttons out rather than failing when one
// is pressed, and `state`, which is what the radio is *doing* (§10.9's pair, and
// the page shows both).
//
// **No passphrase leaves this device** (§10.15): a record says whether it is
// `secured` and that is the whole of what a page is told about a key. Which is why
// the POST has its "absent means keep it" rule — the two halves are one design.
esp_err_t SettingsGet(httpd_req_t *request) {
    // **The gate, and it is the first statement in every handler** (§10.16): a
    // request with no credential must not reach a counter, a buffer or the I2C
    // bus. With nothing configured this is a comparison against two empty strings
    // and lets everybody through, which is what it did before authentication
    // existed.
    if (!Allowed(request)) {
        return Unauthorised(request);
    }

    Count(&g_status.api);
    const config::Wifi &wifi = config::Get().wifi;
    const wifimgr::Snapshot now = wifimgr::Get();

    int written = std::snprintf(
        g_wifi_json, kWifiJsonSize,
        "{\"writable\":%s,\"max\":%u,\"nats\":{\"url\":\"%s\"}"
        ",\"state\":{\"wifi\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,\"ip\":\"%u.%u.%u.%u\""
        ",\"failure\":\"%s\"}"
        ",\"wifi\":{\"mode\":\"%s\""
        ",\"ap\":{\"ssid\":\"%s\",\"secured\":%s,\"channel\":%u}"
        ",\"networks\":[",
        WritingAllowed() ? "true" : "false", static_cast<unsigned>(config::kMaxNetworks),
        config::Get().nats.url, wifimgr::Name(now.state), now.radio.ssid,
        static_cast<int>(now.radio.rssi), static_cast<unsigned>(now.radio.ip & 0xFF),
        static_cast<unsigned>((now.radio.ip >> 8) & 0xFF),
        static_cast<unsigned>((now.radio.ip >> 16) & 0xFF),
        static_cast<unsigned>((now.radio.ip >> 24) & 0xFF),
        wifi::Radio::Name(now.radio.failure),
        wifi.active ? (wifi.mode == config::WifiMode::kAp ? "ap" : "client") : "off",
        wifi.ap_ssid, wifi.ap_password[0] != '\0' ? "true" : "false",
        static_cast<unsigned>(wifi.ap_channel));

    for (uint8_t i = 0; i < wifi.network_count && written > 0; ++i) {
        const config::Network &network = wifi.networks[i];
        const int room = static_cast<int>(kWifiJsonSize) - written;
        if (room <= 1) {
            break;
        }
        written += std::snprintf(
            g_wifi_json + written, static_cast<size_t>(room),
            "%s{\"ssid\":\"%s\",\"secured\":%s,\"ip\":{\"static\":%s,\"address\":\"%s\""
            ",\"netmask\":\"%s\",\"gateway\":\"%s\",\"dns1\":\"%s\",\"dns2\":\"%s\"}}",
            i == 0 ? "" : ",", network.ssid, network.password[0] != '\0' ? "true" : "false",
            network.ip.enabled ? "true" : "false", network.ip.address, network.ip.netmask,
            network.ip.gateway, network.ip.dns1, network.ip.dns2);
    }
    if (written > 0 && written < static_cast<int>(kWifiJsonSize) - 4) {
        std::snprintf(g_wifi_json + written, kWifiJsonSize - static_cast<size_t>(written),
                      "]}}");
    }

    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, g_wifi_json);
}

// What is on the air, for the picker on that page. **It blocks for a second or
// two**, which on this task is allowed and on the screen task would not be
// (§10.8.6 gave the scan its own task for exactly that reason): somebody asked
// for it, and a request is a thing that takes as long as it takes.
esp_err_t WifiScanApi(httpd_req_t *request) {
    // **The gate, and it is the first statement in every handler** (§10.16): a
    // request with no credential must not reach a counter, a buffer or the I2C
    // bus. With nothing configured this is a comparison against two empty strings
    // and lets everybody through, which is what it did before authentication
    // existed.
    if (!Allowed(request)) {
        return Unauthorised(request);
    }

    Count(&g_status.api);
    size_t found = 0;
    const esp_err_t err = wifimgr::Scan(g_scan, kMaxScan, &found);
    if (err != ESP_OK) {
        // A radio that refused is its own answer rather than an empty list —
        // §10.8.6's rule that `nothing on the air` and `the radio refused` send
        // somebody in different directions.
        return Refuse(request, "503 Service Unavailable", "the radio would not scan");
    }

    int written = std::snprintf(g_wifi_json, kWifiJsonSize, "{\"networks\":[");
    size_t listed = 0;
    for (size_t i = 0; i < found && written > 0; ++i) {
        if (g_scan[i].ssid[0] == '\0') {
            continue;  // hidden: a row with no name is nothing to pick (§10.8.6)
        }
        bool already = false;
        for (size_t j = 0; j < i; ++j) {
            if (std::strcmp(g_scan[j].ssid, g_scan[i].ssid) == 0) {
                already = true;  // the same name twice is one network to a person
                break;
            }
        }
        if (already) {
            continue;
        }
        const int room = static_cast<int>(kWifiJsonSize) - written;
        if (room <= 1) {
            break;
        }
        written += std::snprintf(g_wifi_json + written, static_cast<size_t>(room),
                                 "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secured\":%s,\"channel\":%u}",
                                 listed == 0 ? "" : ",", g_scan[i].ssid,
                                 static_cast<int>(g_scan[i].rssi),
                                 g_scan[i].secured ? "true" : "false",
                                 static_cast<unsigned>(g_scan[i].channel));
        ++listed;
    }
    if (written > 0 && written < static_cast<int>(kWifiJsonSize) - 3) {
        std::snprintf(g_wifi_json + written, kWifiJsonSize - static_cast<size_t>(written),
                      "]}");
    }

    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, g_wifi_json);
}

// Read a request body into `g_body`, bounded. A body longer than the buffer is
// **refused rather than truncated**, because half a settings document is a
// document nobody sent.
bool ReadBody(httpd_req_t *request, size_t *length) {
    *length = 0;
    if (request->content_len > kMaxSettingsBody) {
        return false;
    }
    size_t taken = 0;
    while (taken < request->content_len) {
        const int got = httpd_req_recv(request, g_body + taken,
                                       request->content_len - taken);
        if (got <= 0) {
            // A socket that stopped mid-body. Nothing is applied — the caller
            // refuses, and the operator presses the button again.
            return false;
        }
        taken += static_cast<size_t>(got);
    }
    g_body[taken] = '\0';
    *length = taken;
    return true;
}

// **The one route that changes what this device does.** The whitelist, the
// refusals and the write-only passwords are all `web_settings.cpp`'s; what is
// here is the socket and the two things only this side knows: whether writing is
// allowed at all, and who to tell afterwards.
esp_err_t SettingsApi(httpd_req_t *request) {
    // **The gate, and it is the first statement in every handler** (§10.16): a
    // request with no credential must not reach a counter, a buffer or the I2C
    // bus. With nothing configured this is a comparison against two empty strings
    // and lets everybody through, which is what it did before authentication
    // existed.
    if (!Allowed(request)) {
        return Unauthorised(request);
    }

    Count(&g_status.api);
    if (!WritingAllowed()) {
        ESP_LOGW(TAG, "a settings write was refused: config.json has web.write false");
        return Refuse(request, "403 Forbidden", "this device is serving read-only");
    }

    size_t length = 0;
    if (!ReadBody(request, &length)) {
        return Refuse(request, "400 Bad Request", "that body did not arrive whole");
    }

    const WriteOutcome outcome = ApplySettings(g_body, length, &config::Get());
    if (outcome.result != WriteResult::kOk) {
        ESP_LOGW(TAG, "settings refused: %s (%s)", WriteResultText(outcome.result),
                 outcome.detail);
        std::snprintf(g_json, sizeof g_json,
                      "{\"ok\":false,\"error\":\"%s\",\"detail\":\"%s\"}",
                      WriteResultText(outcome.result), outcome.detail);
        httpd_resp_set_status(request, "400 Bad Request");
        httpd_resp_set_type(request, "application/json");
        return httpd_resp_sendstr(request, g_json);
    }

    // **The bus is told and the radio is not**, which is the asymmetry
    // `web_settings.h` argues: `nats::Apply` drops a connection the operator is not
    // looking at, and `wifimgr::Apply` would drop the link this page arrived over,
    // mid-edit. The retry is its own button.
    if (outcome.nats_changed) {
        nats::Apply();
    }
    ESP_LOGI(TAG, "settings written from a page - in memory only, 'save' persists them");
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(
        request, "{\"ok\":true,\"saved\":false,\"note\":\"in memory only until you save\"}");
}

// `POST /api/action?do=save|reload|retry|reconnect` — the four verbs that are not
// a field. One route rather than four, and the parsing is `web_settings.cpp`'s.
esp_err_t ActionApi(httpd_req_t *request) {
    // **The gate, and it is the first statement in every handler** (§10.16): a
    // request with no credential must not reach a counter, a buffer or the I2C
    // bus. With nothing configured this is a comparison against two empty strings
    // and lets everybody through, which is what it did before authentication
    // existed.
    if (!Allowed(request)) {
        return Unauthorised(request);
    }

    Count(&g_status.api);
    if (!WritingAllowed()) {
        return Refuse(request, "403 Forbidden", "this device is serving read-only");
    }

    const Action action = ActionFromUri(request->uri);
    esp_err_t err = ESP_OK;
    switch (action) {
        case Action::kSave:
            err = config::Save();
            break;
        case Action::kReload:
            // **This one can take the page with it**, and that is the honest
            // behaviour rather than a bug: `config::Reload` fires the change hook,
            // which re-applies the network list — so a reload that restores a
            // different network is a reload that drops this connection. The page
            // says so before it asks.
            err = config::Reload();
            break;
        case Action::kWifiRetry:
            wifimgr::Apply();
            break;
        case Action::kBusRetry:
            nats::Apply();
            nats::ConnectNow();
            break;
        case Action::kNone:
            return Refuse(request, "400 Bad Request", "no such action");
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s failed: %s", ActionName(action), esp_err_to_name(err));
        return Refuse(request, "500 Internal Server Error", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "%s, asked for over http", ActionName(action));
    std::snprintf(g_json, sizeof g_json, "{\"ok\":true,\"did\":\"%s\"}", ActionName(action));
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, g_json);
}

// --- The one thing this server can do (§10.16) ----------------------------
//
// `POST /api/reboot?confirm=reboot`. Every other route reads; this one is the
// exception the repository owner asked for, and three rules are what make it
// defensible rather than merely convenient:
//
//   * **it needs the word** (`web_paths.h`), so a stray POST from a scanner, a
//     link somebody sent or a page reloaded by mistake is not a device that
//     restarts. That is a confirmation and not authentication - §10.3 already puts
//     the trust boundary at the router, so anybody on that LAN can send it;
//   * **it is the safest write there is.** A reboot takes nothing away that does
//     not come back by itself in ten seconds, which is exactly §10.7's argument
//     for the *console's* `reboot` needing no confirmation word. What it can be
//     abused for is a denial of service, and §10.10 already accepts that class of
//     thing from anything that can reach the bus: ten seconds of a device
//     answering nothing is a hook that times out and a question that goes back to
//     its own terminal. It cannot produce a verdict, and that is the line;
//   * **and it says what it costs.** `config set` writes to memory and `config
//     save` reaches the filesystem (§10.15), so a reboot is where unsaved edits
//     go. The page says so to the operator; this says it to the log.
esp_err_t RebootApi(httpd_req_t *request) {
    // **The gate, and it is the first statement in every handler** (§10.16): a
    // request with no credential must not reach a counter, a buffer or the I2C
    // bus. With nothing configured this is a comparison against two empty strings
    // and lets everybody through, which is what it did before authentication
    // existed.
    if (!Allowed(request)) {
        return Unauthorised(request);
    }

    Count(&g_status.api);
    if (!ConfirmsReboot(request->uri)) {
        httpd_resp_set_status(request, "400 Bad Request");
        httpd_resp_set_type(request, "application/json");
        return httpd_resp_sendstr(request, "{\"ok\":false,\"error\":\"not confirmed\"}");
    }

    ESP_LOGW(TAG, "reboot asked for over HTTP - anything set and not saved is gone");
    httpd_resp_set_type(request, "application/json");
    const esp_err_t err = httpd_resp_sendstr(request, "{\"ok\":true,\"rebooting\":true}");

    // **The answer has to leave before the chip does**, which is §10.7's finding
    // about the console's `reboot` arriving at a port that goes down with the
    // chip: the response is sitting in a TCP buffer, and restarting on the next
    // statement takes it with the stack. Half a second is the browser seeing
    // "rebooting" rather than a connection that died - and blocking the server
    // task here costs nothing, since the one thing it could do next is not exist.
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(500));

    // Nothing is quiesced first, deliberately, and for §10.7's reason: a
    // `config.json` write interrupted here is the power cut §10.15 already
    // recovers from at boot.
    esp_restart();
    return err;  // not reached: the chip is gone by here
}

esp_err_t File(httpd_req_t *request) {
    // **The gate, and it is the first statement in every handler** (§10.16): a
    // request with no credential must not reach a counter, a buffer or the I2C
    // bus. With nothing configured this is a comparison against two empty strings
    // and lets everybody through, which is what it did before authentication
    // existed.
    if (!Allowed(request)) {
        return Unauthorised(request);
    }

    char name[kMaxNameLength + 1] = {};
    if (!UriToName(request->uri, name, sizeof name)) {
        Count(&g_status.refused);
        ESP_LOGW(TAG, "refused '%s' - not a page this server serves", request->uri);
        return NotFound(request);
    }

    char path[storage::kMaxPathLength] = {};
    if (!storage::ResolvePath(name, path, sizeof path)) {
        Count(&g_status.refused);
        return NotFound(request);
    }

    FILE *file = std::fopen(path, "rb");
    if (file == nullptr) {
        Count(&g_status.not_found);
        ESP_LOGI(TAG, "no %s on the filesystem", path);
        return NotFound(request);
    }

    Count(&g_status.requests);
    httpd_resp_set_type(request, ContentType(name));
    // **A phone should fetch these once.** Every byte re-sent is a byte through a
    // 2,880-byte window on a device with 18 KB of heap, and a page's stylesheet and
    // script do not change between two visits to the same firmware. A day is long
    // enough to make a second visit free and short enough that a reflash is not a
    // support call — and the pages that carry *state* are the API answers, which say
    // nothing about caching and are therefore not cached.
    // **A phone should fetch this once.** Every byte re-sent is a byte through the
    // radio's own transmit buffers on a device that has run out of them once
    // already (§10.16), and a page's script does not change between two visits to
    // the same firmware. The API answers say nothing about caching and are
    // therefore not cached — they are the state.
    httpd_resp_set_hdr(request, "Cache-Control", "max-age=86400");

    // **Streamed, never read whole.** `splash.bin` is 460,800 bytes and lives on
    // the same filesystem; a handler that read a file into RAM would be one
    // request away from the allocator failing under a device that also holds
    // LVGL's 64 KB pool and the Wi-Fi stack.
    for (;;) {
        const size_t read = std::fread(g_chunk, 1, sizeof g_chunk, file);
        if (read == 0) {
            break;
        }
        g_status.bytes = g_status.bytes + static_cast<uint32_t>(read);
        if (httpd_resp_send_chunk(request, g_chunk, read) != ESP_OK) {
            // The browser went away mid-page. Close the file and stop: the
            // chunked response is already broken, and sending the terminator into
            // a dead socket is one more error to log for nothing.
            std::fclose(file);
            ESP_LOGW(TAG, "%s: the other end went away", name);
            return ESP_FAIL;
        }
    }
    std::fclose(file);
    Sample();
    return httpd_resp_send_chunk(request, nullptr, 0);
}

}  // namespace

esp_err_t StartLocked() {
    if (g_server != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!storage::Mounted()) {
        ESP_LOGW(TAG, "no filesystem, so there are no pages to serve");
        return ESP_ERR_INVALID_STATE;
    }
    // **And the network stack has to exist, which on this device is not a
    // given.** §10.9 brings lwIP up lazily — `esp_netif_init` happens inside the
    // radio's first use — so with `wifi.active` false there are no mailboxes for
    // a listening socket to be created against. `httpd_start` does not return an
    // error for that: it walks into `assert failed: tcpip_send_msg_wait_sem
    // (Invalid mbox)` and reboots the device. Found by doing exactly that on a
    // freshly flashed board, which is the one state where the radio is off by
    // default.
    if (!wifimgr::Get().stack_up) {
        ESP_LOGW(TAG, "the network stack is down - 'wifi mode client' or 'wifi mode ap' first");
        return ESP_ERR_INVALID_STATE;
    }

    // Taken **before** anything is allocated, so that `web` can print the
    // difference rather than an absolute number nobody can compare (§10.9 records
    // what a heap figure with no instant attached is worth: nothing).
    g_status.free_before_start =
        static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = kPort;
    config.task_priority = kTaskPriority;
    config.stack_size = kTaskStackBytes;
    config.max_open_sockets = kMaxSockets;
    config.max_uri_handlers = 10;
    // **LRU purge on**, which `HTTPD_DEFAULT_CONFIG` leaves off: with three
    // sockets a browser that leaves one open would otherwise make the fourth
    // request fail rather than closing the oldest.
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;

    const esp_err_t err = httpd_start(&g_server, &config);
    if (err != ESP_OK) {
        g_server = nullptr;
        ESP_LOGE(TAG, "not started: %s", esp_err_to_name(err));
        return err;
    }

    // The API first, because `/*` below matches everything and the first match
    // wins.
    const httpd_uri_t api = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = StatusApi,
        .user_ctx = nullptr,
    };
    const httpd_uri_t devstatus = {
        .uri = "/api/devstatus",
        .method = HTTP_GET,
        .handler = DevStatusApi,
        .user_ctx = nullptr,
    };
    // **A POST, not a GET**, so that no link, prefetch or crawler can reach it -
    // the wildcard below matches `GET` only, which is what keeps every other route
    // read-only by construction rather than by intent.
    const httpd_uri_t reboot = {
        .uri = "/api/reboot",
        .method = HTTP_POST,
        .handler = RebootApi,
        .user_ctx = nullptr,
    };
    // **One document, both ways**: the GET fills a form and the POST takes it back.
    // Two handlers for one URI, which is what `esp_http_server` wants for two
    // methods — and it is why the file handler below is `GET`-only rather than a
    // catch-all.
    const httpd_uri_t settings_get = {
        .uri = "/api/settings",
        .method = HTTP_GET,
        .handler = SettingsGet,
        .user_ctx = nullptr,
    };
    const httpd_uri_t scan = {
        .uri = "/api/wifi/scan",
        .method = HTTP_GET,
        .handler = WifiScanApi,
        .user_ctx = nullptr,
    };
    const httpd_uri_t settings = {
        .uri = "/api/settings",
        .method = HTTP_POST,
        .handler = SettingsApi,
        .user_ctx = nullptr,
    };
    const httpd_uri_t action = {
        .uri = "/api/action",
        .method = HTTP_POST,
        .handler = ActionApi,
        .user_ctx = nullptr,
    };
    const httpd_uri_t files = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = File,
        .user_ctx = nullptr,
    };
    if (httpd_register_uri_handler(g_server, &api) != ESP_OK ||
        httpd_register_uri_handler(g_server, &devstatus) != ESP_OK ||
        httpd_register_uri_handler(g_server, &reboot) != ESP_OK ||
        httpd_register_uri_handler(g_server, &settings_get) != ESP_OK ||
        httpd_register_uri_handler(g_server, &scan) != ESP_OK ||
        httpd_register_uri_handler(g_server, &settings) != ESP_OK ||
        httpd_register_uri_handler(g_server, &action) != ESP_OK ||
        httpd_register_uri_handler(g_server, &files) != ESP_OK) {
        // A server with no handlers answers 404 to everything, which reads as a
        // broken device rather than a failed start. Take it back down.
        httpd_stop(g_server);
        g_server = nullptr;
        ESP_LOGE(TAG, "handlers would not register");
        return ESP_FAIL;
    }

    g_status.running = true;
    g_status.port = kPort;
    Sample();
    ESP_LOGI(TAG, "serving on port %u - %" PRIu32 " bytes of heap went into it",
             static_cast<unsigned>(kPort), g_status.free_before_start - g_status.free_now);
    return ESP_OK;
}

esp_err_t StopLocked() {
    if (g_server == nullptr) {
        return ESP_OK;
    }
    const esp_err_t err = httpd_stop(g_server);
    if (err != ESP_OK) {
        // **A failed stop is not a stop, and saying otherwise loses the server.**
        // `httpd_stop` returns before doing anything at all if it cannot send its
        // own shutdown message, so the task is still alive and the socket is still
        // bound; dropping the handle here would leave an instance nobody can ever
        // reach again, while `web` reported it stopped. Keep it, say so, and let
        // the next tick try again.
        ESP_LOGE(TAG, "it would not stop (%s) - still running, will try again",
                 esp_err_to_name(err));
        return err;
    }
    g_server = nullptr;
    g_status.running = false;
    Sample();
    g_status.free_after_stop = g_status.free_now;
    g_status.last_run_returned = static_cast<int32_t>(g_status.free_after_stop) -
                                static_cast<int32_t>(g_status.free_before_start);
    g_status.has_run = true;
    ESP_LOGI(TAG, "stopped - %" PRIu32 " bytes free, %" PRId32 " against before it started",
             g_status.free_after_stop,
             static_cast<int32_t>(g_status.free_after_stop) -
                 static_cast<int32_t>(g_status.free_before_start));
    return err;
}

esp_err_t Start() {
    const Held held;
    return StartLocked();
}

esp_err_t Stop() {
    const Held held;
    return StopLocked();
}

void Hold(bool held) {
    const Held lock;
    g_held = held;
}

bool Running() { return g_server != nullptr; }

void SetDiagnostics(DiagnosticsPrinter printer) { g_diagnostics = printer; }

void SetApprovals(ApprovalsProbe probe) { g_approvals = probe; }

Desired GetDesired() { return g_desired; }

void SetDesired(Desired desired) {
    g_desired = desired;
    // Whatever was learned about a failed start belongs to the state that was
    // asked for before this one.
    g_retry_pending = false;
    // **Not applied here.** The reconcile happens on the manager's tick, within
    // 200 ms, which is the one place that decides whether the network allows it —
    // and one place is what keeps "asked for" and "running" from drifting.
    ESP_LOGI(TAG, "asked for %s", Name(desired));
}

Desired DesiredFromConfig() {
    switch (config::Get().web.mode) {
        case config::WebMode::kOff:
            return Desired::kOff;
        case config::WebMode::kOn:
            return Desired::kOn;
        case config::WebMode::kAuto:
            break;
    }
    return Desired::kAuto;
}

void Apply() { SetDesired(DesiredFromConfig()); }

void Maintain() {
    // **`wifimgr::Get()` takes the manager's own lock**, and this runs on the
    // manager's task — so it is only ever called from outside its pump. The
    // header says so and `wifi_manager.cpp` says so where it calls this.
    const wifimgr::Snapshot wifi = wifimgr::Get();
    const bool ap =
        wifi.state == wifimgr::State::kAp || wifi.state == wifimgr::State::kApWindow;

    // **Two answers to "will there be a network", and the earliest one wins.**
    // The file is what the console has just edited — `wifi mode off` writes it,
    // and this runs before the manager's next pump has even read it, which is
    // exactly the window in which the server has to let go. The snapshot is the
    // policy's own answer, which also covers an override that never touched the
    // file. Either saying off is enough.
    const bool network_wanted = wifimgr::DesiredFromConfig() != wifimgr::Desired::kOff &&
                                wifi.desired != wifimgr::Desired::kOff;

    // **The lock is taken before the answer is read**, because the answer is
    // about `g_server` and so is the action: deciding outside it and acting
    // inside would be the same race one step further along.
    const Held held;
    const Reconcile next =
        Next(g_desired, network_wanted, wifi.stack_up, ap, g_server != nullptr, g_held);
    if (next == Reconcile::kNothing) {
        return;
    }
    if (next == Reconcile::kStop) {
        // The network went away under a running server, or somebody said off.
        StopLocked();
        return;
    }

    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (g_retry_pending && (now - g_retry_at_ms) < kRetryAfterMs) {
        return;
    }
    g_retry_pending = false;
    if (StartLocked() != ESP_OK) {
        g_retry_pending = true;
        g_retry_at_ms = now;
    }
}

esp_err_t Init() {
    // Before the tick handler below is registered, because the first thing that
    // tick does is take it.
    g_lock = xSemaphoreCreateMutexStatic(&g_lock_storage);
    if (g_lock == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    g_desired = DesiredFromConfig();
    // The reconciler, on somebody else's task — `web_server.h` argues why, and
    // the short form is 2.5 KB of permanent RAM not spent.
    wifimgr::OnTick([](void *) { Maintain(); }, nullptr);
    ESP_LOGI(TAG, "asked for %s", Name(g_desired));
    return ESP_OK;
}

Status Get() {
    Status out = g_status;
    out.desired = g_desired;
    out.running = g_server != nullptr;
    // Read out of the live config rather than remembered, the same way the gate
    // reads it: a credential set and not saved is a credential in force, and the
    // readout has to agree with the door.
    out.auth = AuthRequired(config::Get().web.user, config::Get().web.password);
    out.free_now = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    out.low_water = static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    return out;
}

}  // namespace web
