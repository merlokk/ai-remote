#pragma once

// **Where the bus is** — one string in `config.json` turned into a host and a
// port (CLAUDE.md §10.3, §10.15).
//
// It is here rather than in `config` because `nats.url` is not a fact about a
// file: it is the one format this component defines. The shape is the one §10.15
// argues for — text in the file, the parsed form at the driver, and something
// pure between them that a host test can hold to account.
//
// **This header includes `<cstdint>`/`<cstddef>` and nothing else**, which is
// what puts it under Unity with no board (§10.11) next to `link_policy.h`.
//
// Strict on purpose, and every refusal below is a decision rather than a
// missing feature: a URL is the one setting on this device that an operator
// types in full, and the moment to catch a typo is while they are still
// looking at the screen. `config::ParseIpv4` states the same rule from the
// other end of the same file.

#include <cstddef>
#include <cstdint>

namespace nats {

// 63 characters and a terminator, which is `config::kUrlSize` — the field this
// is parsed out of cannot hold more, so nothing longer can ever arrive.
inline constexpr size_t kHostSize = 64;

// The port every NATS server listens on unless it was told otherwise, and the
// one part of an address nobody remembers.
inline constexpr uint16_t kDefaultPort = 4222;

struct Endpoint {
    char host[kHostSize];
    uint16_t port;
};

// `nats://host:port`, `host:port`, `nats://host` and a bare `host` — the four
// spellings of the same thing, and nothing else.
//
// **Refused, each for a stated reason** (the tests carry them one by one): an
// empty host; a port that is not 1..65535; `ws://`, `wss://` and `tls://`,
// which name transports this device does not speak (§10.3, §10.4) and which
// silently ignored would be a device connecting in a way the string does not
// say; a path, credentials, a bracketed IPv6 literal, and anything with space
// around it.
//
// **Writes nothing on a refusal**, so a typo costs the typo and not the
// connection that is already up.
bool ParseUrl(const char *url, Endpoint *out);

// Whether an edit is worth dropping a working connection for.
bool Same(const Endpoint &a, const Endpoint &b);

}  // namespace nats
