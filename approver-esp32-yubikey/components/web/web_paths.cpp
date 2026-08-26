#include "web_paths.h"

namespace web {
namespace {

// The whitelist, and it is the security boundary rather than a convenience: what
// is not here is not served, whatever else lands on that filesystem later
// (`web_paths.h` says why). `.json` is deliberately absent and `config.json` is
// the reason.
struct Extension {
    const char *suffix;
    const char *type;
};

constexpr Extension kServed[] = {
    {".html", "text/html"},
    {".css", "text/css"},
    {".js", "application/javascript"},
    {".png", "image/png"},
    {".ico", "image/x-icon"},
    {".svg", "image/svg+xml"},
    {".txt", "text/plain"},
};

bool EndsWith(const char *name, size_t length, const char *suffix) {
    size_t suffix_length = 0;
    while (suffix[suffix_length] != '\0') {
        ++suffix_length;
    }
    if (suffix_length == 0 || suffix_length >= length) {
        // `>=` rather than `>`: a name that is *only* an extension — `.html` — is
        // a dotfile, and there is nothing before the dot to be a name.
        return false;
    }
    const char *tail = name + (length - suffix_length);
    for (size_t i = 0; i < suffix_length; ++i) {
        // Lower-case only. A URL is case-sensitive here because SPIFFS is, so
        // `/INDEX.HTML` is a different file and refusing it is the honest answer
        // rather than opening one that was not asked for.
        if (tail[i] != suffix[i]) {
            return false;
        }
    }
    return true;
}

bool Allowed(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == '-';
}

}  // namespace

bool UriToName(const char *uri, char *out, size_t capacity) {
    if (uri == nullptr || out == nullptr || capacity == 0) {
        return false;
    }
    if (uri[0] != '/') {
        return false;
    }

    // Everything from a `?` or a `#` belongs to the request, not to the file.
    size_t length = 0;
    while (uri[1 + length] != '\0' && uri[1 + length] != '?' && uri[1 + length] != '#') {
        ++length;
    }
    const char *name = uri + 1;

    if (length == 0) {
        // `/`, or `/?saved=1`. The one name a URL may reach without naming it.
        name = kIndexName;
        length = 0;
        while (kIndexName[length] != '\0') {
            ++length;
        }
    }

    if (length > kMaxNameLength || length + 1 > capacity) {
        // **Nothing is written on a refusal**, the rule §10.2 keeps about the
        // signing bytes: a truncated name is one that opens a different file.
        return false;
    }

    for (size_t i = 0; i < length; ++i) {
        if (!Allowed(name[i])) {
            // A `/` lands here, which is what makes traversal unrepresentable
            // rather than defended against — SPIFFS is flat, so a name never
            // contains one. So does a `%`, which is why nothing is decoded.
            return false;
        }
    }
    if (name[0] == '.') {
        return false;
    }

    bool served = false;
    for (const Extension &extension : kServed) {
        if (EndsWith(name, length, extension.suffix)) {
            served = true;
            break;
        }
    }
    if (!served) {
        return false;
    }

    for (size_t i = 0; i < length; ++i) {
        out[i] = name[i];
    }
    out[length] = '\0';
    return true;
}

bool ConfirmsReboot(const char *uri) {
    if (uri == nullptr) {
        return false;
    }
    static constexpr char kWord[] = "confirm=reboot";
    constexpr size_t kWordLength = sizeof kWord - 1;

    // Walk to the query, then over the parameters in it. Written as a scan rather
    // than as a `strstr` for one reason and it is a real one: `strstr` finds the
    // word inside `xconfirm=reboot` and inside `confirm=reboots`, and both of
    // those are a URL that did **not** confirm anything.
    const char *at = uri;
    while (*at != '\0' && *at != '?' && *at != '#') {
        ++at;
    }
    if (*at != '?') {
        return false;
    }
    ++at;

    for (;;) {
        // One parameter: from here to the next `&`, or to the end of the query.
        size_t length = 0;
        while (at[length] != '\0' && at[length] != '&' && at[length] != '#') {
            ++length;
        }
        if (length == kWordLength) {
            size_t i = 0;
            while (i < kWordLength && at[i] == kWord[i]) {
                ++i;
            }
            if (i == kWordLength) {
                return true;
            }
        }
        if (at[length] != '&') {
            return false;  // the end of the query, and nothing in it said so
        }
        at += length + 1;
    }
}

const char *ContentType(const char *name) {
    if (name != nullptr) {
        size_t length = 0;
        while (name[length] != '\0') {
            ++length;
        }
        for (const Extension &extension : kServed) {
            if (EndsWith(name, length, extension.suffix)) {
                return extension.type;
            }
        }
    }
    // Unreachable through `UriToName`, and kept anyway: a null here would reach
    // `httpd_resp_set_type` and crash a device that was answering a request.
    return "application/octet-stream";
}

}  // namespace web
