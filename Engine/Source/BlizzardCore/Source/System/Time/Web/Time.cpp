#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cmath>
#include <cstdint>
#include <ctime>
#include <windows.h>
#include "Engine/Source/BlizzardCore/Source/System/Time/Time.h"

// Blizzard::Time::Timestamp counts nanoseconds from 2000-01-01 00:00:00 UTC.
static const int64_t kUnixToTimestampEpochSeconds = 946684800;

const int32_t System_Time::s_daysBeforeMonth[13] = { 0, 31, 59, 90, 120, 151, 182, 212, 243, 273, 304, 334, 365 };

uint64_t System_Time::timeScaleCycles = 0xFFFFFFFFFFFFFFFF;

bool System_Time::s_usePerformanceCounter;

Blizzard::Lock::Atomic System_Time::s_tickLock;

int64_t System_Time::s_absBegin;

uint32_t System_Time::s_lastTickCount;

double System_Time::timeScaleMilliseconds;

Blizzard::Time::Timestamp System_Time::s_gmBegin;

uint64_t System_Time::s_tickCountHigh;

double System_Time::timeScaleNanoseconds;

double System_Time::timeScaleSeconds;

double System_Time::timeScaleMicroseconds;

static int64_t MonotonicNanoseconds() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

Blizzard::Time::Timestamp Blizzard::Time::FromFileTime(uint64_t fileTime) {
    if (fileTime <= 0x0077A5D6AA8BF851) {
        return static_cast<Timestamp>(0x8000000000000000);
    }

    if (fileTime <= 0x030701FFA04E87AE) {
        return static_cast<Timestamp>(fileTime * 100 + 0x5143382561530000);
    }

    return 0x7FFFFFFFFFFFFFFF;
}

uint64_t System_Time::GetTickCount64() {
    return static_cast<uint64_t>(MonotonicNanoseconds() / 1000000);
}

void Blizzard::Time::BreakTime(Timestamp time, TimeRec& rec) {
    // Floor division so pre-2000 timestamps keep a non-negative nanosecond field.
    int64_t seconds = time / 1000000000;
    int32_t nanosecond = static_cast<int32_t>(time % 1000000000);

    if (nanosecond < 0) {
        nanosecond += 1000000000;
        seconds--;
    }

    time_t unixSeconds = static_cast<time_t>(seconds + kUnixToTimestampEpochSeconds);
    struct tm tm;
    gmtime_r(&unixSeconds, &tm);

    rec.day = tm.tm_mday;
    rec.hour = tm.tm_hour;
    rec.minute = tm.tm_min;
    rec.second = tm.tm_sec;
    rec.nanosecond = nanosecond;
    rec.weekday = tm.tm_wday;
    rec.year = tm.tm_year + 1900;
    rec.month = tm.tm_mon + 1;
    rec.yearday = tm.tm_yday;
}

Blizzard::Time::Timestamp Blizzard::Time::MakeTime(const TimeRec& rec) {
    struct tm tm = {};
    tm.tm_year = rec.year - 1900;
    tm.tm_mon = rec.month - 1;
    tm.tm_mday = rec.day;
    tm.tm_hour = rec.hour;
    tm.tm_min = rec.minute;
    tm.tm_sec = rec.second;

    int64_t seconds = static_cast<int64_t>(timegm(&tm)) - kUnixToTimestampEpochSeconds;

    return seconds * 1000000000 + rec.nanosecond;
}

bool System_Time::TimeInit() {
    s_usePerformanceCounter = true;
    timeScaleCycles = 1000000000;
    s_absBegin = MonotonicNanoseconds();

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    s_gmBegin = (static_cast<int64_t>(ts.tv_sec) - kUnixToTimestampEpochSeconds) * 1000000000 + ts.tv_nsec;

    timeScaleNanoseconds = 1.0;
    timeScaleMicroseconds = 0.001;
    timeScaleMilliseconds = 0.000001;
    timeScaleSeconds = 0.000000001;

    return true;
}

Blizzard::Time::Timestamp Blizzard::Time::GetTimestamp() {
    if (System_Time::timeScaleCycles == 0xFFFFFFFFFFFFFFFF) {
        System_Time::TimeInit();
    }

    // Wall clock at startup plus the monotonic time since, as on Windows, so it never steps backwards.
    return System_Time::s_gmBegin + (MonotonicNanoseconds() - System_Time::s_absBegin);
}

int64_t Blizzard::Time::Milliseconds() {
    return MonotonicNanoseconds() / 1000000;
}

#endif
