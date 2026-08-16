#include "endpoint.h"

#include <cstring>

namespace nats {

namespace {

// The one scheme this device speaks. Everything else is named in the header
// and refused below rather than stripped and hoped about.
constexpr const char kScheme[] = "nats://";
constexpr size_t kSchemeLength = sizeof(kScheme) - 1;

bool StartsWith(const char *text, const char *prefix) {
    return std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

// Decimal, 1..65535, and the whole of what it was given. `strtoul` would take
// `42a2` as 42 and leave the rest for somebody else to notice, which on a port
// number means connecting to a server that is not the one written down.
bool ParsePort(const char *text, uint16_t *out) {
    if (text[0] == '\0') {
        return false;
    }
    uint32_t value = 0;
    for (const char *p = text; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        value = value * 10u + static_cast<uint32_t>(*p - '0');
        if (value > 65535u) {
            return false;
        }
    }
    if (value == 0) {
        return false;
    }
    *out = static_cast<uint16_t>(value);
    return true;
}

}  // namespace

bool ParseUrl(const char *url, Endpoint *out) {
    if (url == nullptr || out == nullptr) {
        return false;
    }

    // **Another scheme is refused, not stripped.** `ws://` and `wss://` are in
    // the client (§10.4) and are not wired up here; `tls://` is §10.3's
    // eventual fix rather than today's. A URL that promises a transport the
    // socket will not use is worse than no URL at all — the operator would be
    // left watching a connection that never happens.
    const char *rest = url;
    if (StartsWith(rest, kScheme)) {
        rest += kSchemeLength;
    } else if (std::strstr(rest, "://") != nullptr) {
        return false;
    }

    // Credentials, a path and a query are all things a NATS address does not
    // have. Ignored, each of them would be a device connecting somewhere other
    // than where the string says.
    if (std::strpbrk(rest, "@/? \t[]") != nullptr) {
        return false;
    }

    const char *colon = std::strchr(rest, ':');
    const size_t host_length = colon == nullptr ? std::strlen(rest)
                                                : static_cast<size_t>(colon - rest);
    if (host_length == 0 || host_length >= kHostSize) {
        return false;
    }

    uint16_t port = kDefaultPort;
    if (colon != nullptr && !ParsePort(colon + 1, &port)) {
        return false;
    }

    // Written only once everything has been agreed to, which is what the
    // header promises and what makes a refusal free.
    std::memcpy(out->host, rest, host_length);
    out->host[host_length] = '\0';
    out->port = port;
    return true;
}

bool Same(const Endpoint &a, const Endpoint &b) {
    return a.port == b.port && std::strcmp(a.host, b.host) == 0;
}

}  // namespace nats
