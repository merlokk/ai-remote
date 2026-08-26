#pragma once

// **Who may reach the configuration site at all** (CLAUDE.md §10.16).
//
// §10.16 shipped with one open question, written down in as many words: the write
// path moved the trust boundary the moment the first `POST` landed, and moving it
// back means either authentication or binding the server to the access-point
// interface only. This file is the first of those two, modelled on the house
// firmware of §10.14.4, which answers the same question with `users.json` and
// HTTP basic authentication.
//
// **It gates every route, not the write path.** `web.write` already refuses the
// forms; what it cannot do is keep `/api/devstatus` — a few hundred lines about
// this device, its bus, its registration and the key on its OTG port — off a LAN.
// So the rule is the simple one: with credentials configured, *nothing* is served
// without them, the 401 included.
//
// `<cstddef>` and `<cstdint>` and nothing else, which is the shape every
// decision-holding file in this firmware has (§10.11) — the whitelist next door,
// the Wi-Fi policy, the bus link, the LED's arithmetic, the state ranking. Here
// the reason is the strongest of the set: this is the one comparison standing
// between a network and a device that can be pointed at another NATS server, and
// a comparison that needs a board to exercise is one nobody exercises.
//
// **Four rules, and each is a test.**
//
//   1. **The pair is the switch.** Authentication is on when a user *and* a
//      password are configured, and off otherwise — there is no third boolean to
//      disagree with them, which is the call `config::Wifi::active` makes and
//      §10.15 argues ("two fields that can disagree is one bug report nobody can
//      read"). A device flashed with the shipped `config.json`, which has
//      neither, therefore behaves exactly as it did before this file existed.
//   2. **The credential is encoded, never decoded.** The expected `user:password`
//      is base64-encoded once per request and the header is compared against
//      *that*. There is no decoder here on purpose, and it is `web_paths.h`'s
//      argument about percent-decoding arriving from the other side: decoding is
//      where the parsing bugs live, and a device with no decoder has none of
//      them. The cost is honest and small — a client that sends a non-canonical
//      encoding of the right credential is refused, and no client does.
//   3. **Fail closed, always.** Every path that cannot answer the question —
//      no header, a scheme this does not speak, a credential too long for this
//      device to represent, a colon in the user name — answers *no*. A refusal is
//      a person retyping a password; the other kind of mistake is a LAN.
//   4. **The comparison does not stop early.** It runs to the end of the expected
//      string whatever the header says, so the time it takes carries no
//      information about how much of a guess was right. That is cheap here and it
//      is the one class of bug a hand-written compare walks straight into.
//
// **What this is not, and it has to be said plainly: it is not TLS.** Basic
// authentication puts the password on the wire in base64, which is not
// encryption — anybody who can see the packets can read it, and §10.3 already
// says who that is on this LAN. What it buys is real and bounded: the site is no
// longer open to whoever finds the address, and a device on a network its owner
// half-trusts has something better than a switch. TLS stays the real fix, in
// §2.5's company.
//
// And the line §10.10 draws is untouched: **nothing on this site can reach a
// verdict**, with a credential or without one. The only path to `allow` is a
// fingertip on a security key, and this component cannot ask for one.

#include <cstddef>
#include <cstdint>

namespace web {

// What the browser puts in its dialog. The device rather than the network, since
// the whole point of the site is that it is reached before there is a network.
//
// **The sibling board's realm is `approver-esp32`, and these two must not be the
// same string**: an operator with both devices on one desk is looking at two
// saved credentials in one browser, and a realm is the only thing in the dialog
// that says which device is asking.
inline constexpr const char *kAuthRealm = "approver-esp32-yubikey";

// `user:password` plus its terminator, at most — 32 + 1 + 64 + 1. Tied to
// `config::kWebUserSize` and `config::kWebPasswordSize` by a `static_assert`
// where the two meet (`web_server.cpp`), because this file includes neither.
inline constexpr size_t kMaxCredentialSize = 98;

// The same thing base64'd: four characters per three bytes, rounded up, plus a
// terminator. 97 bytes is 132 characters.
inline constexpr size_t kMaxEncodedSize = ((kMaxCredentialSize - 1 + 2) / 3) * 4 + 1;

// The scheme, and the length of it, in one place — the handler logs it and the
// comparison walks it.
inline constexpr const char *kAuthScheme = "Basic";

// **Is the site locked.** Both halves present and non-empty, or it is not — rule
// 1 above. Null is the same as empty, because that is what an unset field of a
// fixed struct looks like from here.
//
// It is also what the readout prints: a site with half a credential in its config
// is open, and an operator has to be able to see that rather than assume it.
bool AuthRequired(const char *user, const char *password);

// `user:password`, base64, into `out`. **False writes nothing** — a truncated
// credential is one that matches something nobody typed (§10.2's rule about the
// signing bytes, arriving here).
//
// False when it does not fit, when `out` is null, and when the *user* contains a
// colon: basic authentication cannot represent that, since `a:b` + `c` and `a` +
// `b:c` are the same bytes on the wire. A colon in the password is fine and has
// to be — everything after the first one is the password.
bool BasicCredential(const char *user, const char *password, char *out, size_t capacity);

// **The whole gate, in one call**, so no handler can ask the two questions in the
// wrong order: it answers true when nothing is configured, and otherwise only for
// a header that carries exactly those credentials.
//
// `header` is the raw `Authorization` value, or null when the request had none —
// which is what a browser's first request looks like, every time.
bool Authorised(const char *user, const char *password, const char *header);

}  // namespace web
