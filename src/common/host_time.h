#pragma once

// Host clocks and calendar conversion, one implementation per OS (host_time.cpp).
// Times in "FILETIME units" are 100 ns ticks since 1601-01-01 UTC, as the Xbox kernel uses.

#include <cstdint>

namespace hc09::host
{
    // Milliseconds since an arbitrary start (monotonic).
    uint64_t TickCountMs();
    // Current UTC time in FILETIME units.
    uint64_t SystemTimeFileTime();
    // Ticks per second of the counter that recompiled mftb reads (__rdtsc() in ppc_context.h).
    uint64_t TimebaseFrequency();
    // Hint to the CPU inside spin-wait loops.
    void CpuRelax();

    struct TimeFields
    {
        uint16_t year, month, day, hour, minute, second, milliseconds, weekday;  // weekday: 0 = Sunday
    };
    // Calendar fields of a FILETIME value (proleptic Gregorian, UTC).
    TimeFields FileTimeToFields(uint64_t fileTime);
    // FILETIME value of calendar fields; false when a field is out of range (weekday is ignored).
    bool FieldsToFileTime(const TimeFields& fields, uint64_t& fileTime);
}
