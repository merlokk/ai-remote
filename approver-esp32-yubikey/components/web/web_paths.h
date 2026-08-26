#pragma once

// What a URL is allowed to reach on the filesystem (CLAUDE.md §10.16).
//
// **This file includes `<cstddef>` and nothing else**, which is the shape every
// decision-holding file in this firmware has (§10.11): the state ranking, the
// LED's arithmetic, the Wi-Fi policy, the bus link, CTAPHID's framing. The socket
// and the handler live next door in `web_server.cpp` and have no rules in them.
//
// One rule is the reason this file exists at all, and it is not about tidiness:
//
//   **the same filesystem holds `config.json` and `fido.json`.** SPIFFS carries
//   the pages *and* the Wi-Fi passphrase, the pinned handler key, the factory
//   defaults (§10.15) **and the enrolment** (§10.18) — so a static file server
//   that serves whatever is asked for hands the WPA password to anyone who types
//   `/config.json`. §10.15's rule is that a password is a secret from the moment
//   it is typed: never logged, never in a console dump. A URL is a bigger hole
//   than either.
//
// **And on this board the second file makes the rule sharper than it is next
// door.** `fido.json` holds the `ikm` and the key handle the ARKG derivation of
// §10.18 needs — not a private key, which is why §10.6 can say there is none on
// this board, but the half of the pair *this device* contributes. §10.18's
// property that "the key alone cannot approve anything either" is exactly the
// statement that those bytes are not public, and a `/fido.json` served to a LAN
// would retire it. The whitelist below is what makes that unrepresentable rather
// than remembered: `.json` is not on it, and a secret added to that filesystem
// next year is refused by a rule that already exists.
//
// So the answer is a **whitelist of extensions**, not a blacklist of names: the
// server serves the kinds of file a page is made of and nothing else. A secret
// added to that filesystem later is refused by a rule that already exists rather
// than by somebody remembering to add its name — which is the difference between
// a policy and a patch.
//
// Two smaller rules that come from the same place:
//
//   * **SPIFFS is flat** (`storage.h` says so), so a name has no directories in
//     it and a URL with a second `/` is refused outright. Traversal is not
//     defended against — it is unrepresentable;
//   * **no percent-decoding.** A `%` in a path is refused rather than decoded,
//     because decoding is where the traversal bugs live (`%2e%2e`, and the
//     double-decode after it). A configuration page has no need of it.

#include <cstddef>

namespace web {

// SPIFFS stores names of at most 32 bytes — `storage::kMaxNameLength`, tied to it
// by a `static_assert` where the two meet, because this file includes nothing.
inline constexpr size_t kMaxNameLength = 32;

// What `/` means. The one name a URL may reach without naming it.
inline constexpr const char *kIndexName = "index.html";

// What a refusal and a missing file both look like. **A page rather than a line
// of text**, because a phone that asked for the wrong thing is a phone with no way
// back: the file carries a link home, and a device whose filesystem does not have
// it falls back to the plain sentence (`web_server.cpp` does that, and it is not
// an error path worth a branch of its own on the device).
//
// It is a page on the whitelist like any other, which is deliberate: nothing here
// gets to open a file by a rule the rest of the server does not follow.
inline constexpr const char *kNotFoundName = "404.html";

// And what a request with no credential gets (§10.16, `web_auth.h`). The same
// argument as the page above, with one addition that is the whole reason it is a
// page at all: a browser shows this body only after somebody *cancels* the
// credential dialog, so it is the one screen that has to say what the device
// wants and where the two words are set. It is on the whitelist like any other,
// for the reason `404.html` is.
inline constexpr const char *kUnauthorisedName = "401.html";

// **Does this URL confirm a restart** (§10.16). The one thing this server can do
// that changes the device, and the whole of what makes it two steps rather than
// one: `POST /api/reboot?confirm=reboot`.
//
// The word is in the *query* rather than in a body because a body is a read with
// a length to bound and a timeout to get wrong, and this is a device where a
// handler blocking on a socket is a frame nobody sees. What it buys is small and
// real: a stray `POST` from a scanner, a link somebody sent, or a page reloaded by
// mistake is not a device that restarts.
//
// It is **not** authentication and must not be read as any: §10.3 puts the trust
// boundary at the router, so anyone on that LAN can send the word. `web_server.h`
// says what that costs and why a reboot is the only action allowed to be here at
// all — it takes nothing away that comes back by itself. On this board that
// sentence is checkable: the enrolment and the registration are both files, and a
// device that has restarted is one `key selftest` away from proving it.
bool ConfirmsReboot(const char *uri);

// A URL into a bare file name for `storage::ResolvePath`, or false.
//
// **False is the safe answer and there is only one of it**, deliberately: a
// refusal here becomes a 404 with no detail, because "that extension is not
// served" and "there is no such file" are the same sentence to somebody probing
// for `config.json`. The log line on the device side is where the difference
// belongs.
//
// A query string and a fragment are stripped — `/index.html?saved=1` is the
// same page.
bool UriToName(const char *uri, char *out, size_t capacity);

// The `Content-Type` for a name that `UriToName` accepted. Never null: an
// accepted extension always has one, which is the same list read the other way
// round.
const char *ContentType(const char *name);

}  // namespace web
