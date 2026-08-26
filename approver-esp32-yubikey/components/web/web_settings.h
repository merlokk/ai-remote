#pragma once

// What a form on the configuration site may change (CLAUDE.md §10.16).
//
// §10.16 shipped read-only on the sibling board and said what the write path
// would need before it existed: "a writable config over HTTP is a way to point
// this device at another NATS server, and that needs the authentication question
// answered first". That question has an answer now (`web_auth.h`), and this file
// is the other half of it — the whole of what a request may reach, in one place
// that needs no board to test.
//
// **Four rules, and the first is the one the rest hang off.**
//
//   1. **A whitelist of fields, not a merge into `config.json`.** Nothing outside
//      the document below can be written, and an unknown key is *refused* rather
//      than ignored: a page that misspells a field should fail loudly here rather
//      than quietly do nothing on the device. `web_paths.h` keeps the same shape
//      for what a URL may read, and for the same reason — the settings file holds
//      more than a form should be able to touch, and none of it is on this list.
//   2. **A password can be written and never read back.** §10.15 keeps a
//      passphrase out of every log line and every console dump, so a JSON
//      document served over the LAN is out of the question — `GET /api/settings`
//      says `secured` and nothing else. Which leaves the problem of a form that
//      has to submit *something*: an absent or null password means **keep the one
//      that is there**, matched by SSID, and an empty string means clear it. That
//      is the difference between "I did not retype it" and "this network is
//      open", and it is two tests.
//   3. **Memory only, like every other setter** (§10.15). Applying a document
//      changes the live fields; `config save` is a separate action, so a hostile
//      or mistaken write does not survive a reboot on its own — and `config
//      reload` is the undo.
//   4. **Nothing here reconnects anything.** A network list applied to the radio
//      immediately would drop the very link the page arrived over, mid-edit; the
//      *bus* is the exception, because dropping it costs the operator nothing
//      they are looking at. So the reconnect is its own action and the page has a
//      button for it (§10.9's lesson from `wifi check`, arrived at from the other
//      side: reach for the narrowest thing that changed).
//
// **And on this board the whitelist carries §10.10 rather than merely tidiness.**
// Two of the five sections of `config.json` are about *when a verdict may be
// asked for* and *what this device is saying while it asks*, and neither is
// reachable from here:
//
//   * **`approval` is not on the list.** `touchTimeoutSeconds` is how long a
//     request waits for a fingertip and `denyButton` is whether BOOT can refuse
//     one. A network that could set the timeout to zero could make every request
//     expire unanswered — silence, which is §10.10's *safe* outcome, but silence
//     an operator did not choose — and a network that could switch the deny
//     button off could take away the one refusal that costs no touch. Both are
//     console settings and stay console settings;
//   * **`led` is not on the list either.** §10.17 already compiles the palette in
//     rather than making it settable, because "an operator who can recolour
//     `denied` can build a device that lies about what it did"; the brightness is
//     a file field, and a brightness of zero over HTTP is a device that answers
//     requests with no light at all. The console is where it is turned down.
//
// What this file is **not** is authentication. `web_auth.h` is that, and it is
// off unless a credential is configured; `config.json`'s `web.write` is the
// switch that refuses the whole write path, and TLS stays the real fix.

#include <cstddef>
#include <cstdint>

#include "config.h"

namespace web {

// The biggest document this will read. Four networks with a 32-byte name, a
// 64-byte key and a static address block each is about 900 bytes of JSON, and the
// one other section is a hundred more; 1,280 is that with room for whitespace and
// **no more than that**, because it lives in `.bss`.
inline constexpr size_t kMaxSettingsBody = 1280;

// How a write ended. **Every refusal is its own value**, the rule §10.7 states
// for the console: "that field does not exist" and "that address is not an
// address" send somebody in different directions, and one `false` would send them
// in neither.
enum class WriteResult : uint8_t {
    kOk = 0,
    kTooBig,        // more bytes than `kMaxSettingsBody`
    kNotJson,       // it did not parse
    kNotObject,     // valid JSON that is not an object — §10.15's own trap
    kUnknownField,  // not on the whitelist, named in `detail`
    kBadValue,      // the right field, the wrong kind or an unparseable value
    kTooLong,       // a string longer than the field it goes in
    kTooMany,       // more networks than `config::kMaxNetworks`
};

// What went wrong, in the words the page shows. One outcome, because the first
// refusal stops the document: applying half of a form is worse than refusing it.
struct WriteOutcome {
    WriteResult result = WriteResult::kOk;

    // The field or the value that was refused, for a message a person can act on.
    // Empty on success.
    char detail[48] = {};

    // Which halves moved, so the caller knows what is worth telling. Nothing is
    // reconnected here (rule 4) — this is what lets the *caller* be narrow.
    bool wifi_changed = false;
    bool nats_changed = false;
};

// Apply a document to `into`. **`into` is left untouched on every refusal**,
// which is why it is applied to a copy first — the rule §10.2 keeps about the
// signing bytes: a half-applied write is the one outcome nobody can reason about.
WriteOutcome ApplySettings(const char *body, size_t length, config::Data *into);

const char *WriteResultText(WriteResult result);

// --- The three words the radio has ---------------------------------------
//
// `off`, `client`, `ap` — over the two config fields `config::Wifi::active` and
// `config::Wifi::mode`, which is one more spelling than the enum has because
// `active` false is off whatever the mode says (`config.h` argues that pair).
//
// **The sibling board maps these through `ui::WifiMode`, and this one cannot**:
// that enum exists because it is what a Wi-Fi *screen* cycles through, and there
// is no screen on this board and no `ui::wifi_view.h` to hold it. So the mapping
// lives here, which leaves it with exactly one owner rather than two — and it is
// declared in a header rather than hidden in the `.cpp` so the host tier can
// exercise it directly (§10.11).
enum class WifiWord : uint8_t {
    kOff = 0,
    kClient,
    kAp,
};

// The word, or false. **Refused rather than guessed** — §10.9's rule about an
// unknown mode, arriving over HTTP: "yes" is not "on".
bool WifiWordFrom(const char *word, WifiWord *out);

// And back, for the document the GET hands a form to fill itself from.
const char *WifiWordName(bool active, bool is_ap);

// --- The actions ---------------------------------------------------------
//
// The four verbs the page needs that are not a field: persist what is in memory,
// throw it away, try the network list now, try the bus now. They are a query
// parameter on one endpoint rather than four routes, and the parsing is here for
// the reason everything else in this file is: it is a rule, and rules are tested
// without a board.
enum class Action : uint8_t {
    kNone = 0,   // nothing recognisable was asked for
    kSave,       // `config save`
    kReload,     // `config reload` — the undo, and it drops unsaved edits
    kWifiRetry,  // re-read the network list and start the walk again
    kBusRetry,   // re-read the URL and reconnect
};

// `?do=save`. Case-sensitive and exact, the way `ConfirmsReboot` is: a value that
// merely starts with a verb is not that verb.
Action ActionFromUri(const char *uri);
const char *ActionName(Action action);

}  // namespace web
