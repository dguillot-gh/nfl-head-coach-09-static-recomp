#pragma once

// Case-insensitive ASCII string comparison on every host (MSVC has _stricmp, POSIX strcasecmp).

#include <cstddef>

#ifdef _WIN32
#include <cstring>
#else
#include <strings.h>
#endif

namespace hc09
{
    inline int CompareNoCase(const char* a, const char* b)
    {
#ifdef _WIN32
        return _stricmp(a, b);
#else
        return strcasecmp(a, b);
#endif
    }

    inline int CompareNoCase(const char* a, const char* b, size_t length)
    {
#ifdef _WIN32
        return _strnicmp(a, b, length);
#else
        return strncasecmp(a, b, length);
#endif
    }
}
