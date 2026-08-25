#pragma once

// `<strings.h>` is POSIX and MSVC does not ship it. `components/timezone`
// includes it for one function, so this is that one function under the name it
// expects — not a fake, a spelling.

#include <cstring>

inline int strcasecmp(const char *a, const char *b) { return _stricmp(a, b); }
inline int strncasecmp(const char *a, const char *b, size_t n) { return _strnicmp(a, b, n); }
