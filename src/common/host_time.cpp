#include "common/host_time.h"

#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#else
#include <time.h>
#endif

namespace hc09::host
{
    namespace
    {
        constexpr uint64_t kTicksPerSecond = 10'000'000;
        constexpr uint64_t kTicksPerDay = 86'400 * kTicksPerSecond;
        // Days from 1601-01-01 to 1970-01-01.
        constexpr int64_t kDaysTo1970 = 134'774;

        // Days since 1970-01-01 of a civil date, and back (Howard Hinnant's algorithms).
        int64_t DaysFromCivil(int64_t year, unsigned month, unsigned day)
        {
            year -= month <= 2;
            const int64_t era = (year >= 0 ? year : year - 399) / 400;
            const unsigned yearOfEra = unsigned(year - era * 400);
            const unsigned dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
            const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
            return era * 146'097 + int64_t(dayOfEra) - 719'468;
        }

        void CivilFromDays(int64_t days, int64_t& year, unsigned& month, unsigned& day)
        {
            days += 719'468;
            const int64_t era = (days >= 0 ? days : days - 146'096) / 146'097;
            const unsigned dayOfEra = unsigned(days - era * 146'097);
            const unsigned yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36'524 - dayOfEra / 146'096) / 365;
            const unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
            const unsigned monthIndex = (5 * dayOfYear + 2) / 153;
            day = dayOfYear - (153 * monthIndex + 2) / 5 + 1;
            month = monthIndex < 10 ? monthIndex + 3 : monthIndex - 9;
            year = int64_t(yearOfEra) + era * 400 + (month <= 2);
        }

        bool IsLeapYear(unsigned year)
        {
            return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
        }
    }

#ifdef _WIN32
    uint64_t TickCountMs()
    {
        return GetTickCount64();
    }

    uint64_t SystemTimeFileTime()
    {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    }

    uint64_t TimebaseFrequency()
    {
        // The TSC has no reported rate: measure it against the performance counter over 50 ms.
        static const uint64_t frequency = [] {
            LARGE_INTEGER qpcFrequency, start, end;
            QueryPerformanceFrequency(&qpcFrequency);
            QueryPerformanceCounter(&start);
            const uint64_t tscStart = __rdtsc();
            do
                QueryPerformanceCounter(&end);
            while (end.QuadPart - start.QuadPart < qpcFrequency.QuadPart / 20);
            const uint64_t tscEnd = __rdtsc();
            return (tscEnd - tscStart) * uint64_t(qpcFrequency.QuadPart) / uint64_t(end.QuadPart - start.QuadPart);
        }();
        return frequency;
    }

    void CpuRelax()
    {
        YieldProcessor();
    }
#else
    uint64_t TickCountMs()
    {
        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        return uint64_t(now.tv_sec) * 1000 + uint64_t(now.tv_nsec) / 1'000'000;
    }

    uint64_t SystemTimeFileTime()
    {
        timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        return (uint64_t(kDaysTo1970) * 86'400 + uint64_t(now.tv_sec)) * kTicksPerSecond + uint64_t(now.tv_nsec) / 100;
    }

    uint64_t TimebaseFrequency()
    {
#if defined(__aarch64__)
        // __rdtsc() reads the generic timer (cntvct_el0) on ARM64; its rate is in cntfrq_el0.
        uint64_t frequency;
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
        return frequency;
#else
#error "TimebaseFrequency: no implementation for this CPU"
#endif
    }

    void CpuRelax()
    {
#if defined(__aarch64__)
        __asm__ volatile("yield");
#elif defined(__x86_64__)
        __builtin_ia32_pause();
#endif
    }
#endif

    TimeFields FileTimeToFields(uint64_t fileTime)
    {
        const int64_t days = int64_t(fileTime / kTicksPerDay);
        uint64_t rest = fileTime % kTicksPerDay;
        int64_t year;
        unsigned month, day;
        CivilFromDays(days - kDaysTo1970, year, month, day);
        TimeFields fields{};
        fields.year = uint16_t(year);
        fields.month = uint16_t(month);
        fields.day = uint16_t(day);
        fields.hour = uint16_t(rest / (3600 * kTicksPerSecond));
        rest %= 3600 * kTicksPerSecond;
        fields.minute = uint16_t(rest / (60 * kTicksPerSecond));
        rest %= 60 * kTicksPerSecond;
        fields.second = uint16_t(rest / kTicksPerSecond);
        fields.milliseconds = uint16_t(rest % kTicksPerSecond / 10'000);
        fields.weekday = uint16_t((days + 1) % 7);  // 1601-01-01 was a Monday
        return fields;
    }

    bool FieldsToFileTime(const TimeFields& fields, uint64_t& fileTime)
    {
        static constexpr uint8_t kMonthDays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        if (fields.year < 1601 || fields.year > 30827 || fields.month < 1 || fields.month > 12 || fields.day < 1 ||
            fields.hour > 23 || fields.minute > 59 || fields.second > 59 || fields.milliseconds > 999)
            return false;
        const unsigned monthDays = kMonthDays[fields.month - 1] + (fields.month == 2 && IsLeapYear(fields.year) ? 1 : 0);
        if (fields.day > monthDays)
            return false;
        const int64_t days = DaysFromCivil(fields.year, fields.month, fields.day) + kDaysTo1970;
        fileTime = uint64_t(days) * kTicksPerDay +
            ((uint64_t(fields.hour) * 60 + fields.minute) * 60 + fields.second) * kTicksPerSecond +
            uint64_t(fields.milliseconds) * 10'000;
        return true;
    }
}
