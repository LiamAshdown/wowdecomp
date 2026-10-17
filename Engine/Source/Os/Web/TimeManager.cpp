#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cstdint>
#include <ctime>
#include <new>
#include <sched.h>
#include <unistd.h>
#include "Engine/Source/Os/System.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Storm/H/storm.h"

static OsTimeManager* s_OsTimeMgr;

// Clocks are nanoseconds of CLOCK_MONOTONIC (performance.now() underneath).
static int64_t IOsWebClockNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return static_cast<int64_t>(ts.tv_sec) * 1000000000ll + ts.tv_nsec;
}

OsTimeManager::TimingMethod OsTimeManager::Calibrate() {
    // The monotonic clock needs no calibration; the Win64 version spins for ~250 ms here.
    this->m_performanceFrequency = 1000000000ll;
    this->m_timingTestError = 0;

    return SystemMethod2;
}

uint32_t OsTimeGetTestError() {
    return s_OsTimeMgr->m_timingTestError;
}

OsTimeManager::TimingMethod OsTimeGetTimingMethod() {
    return s_OsTimeMgr->m_timingMethod;
}

const char* GetTimingMethodName(OsTimeManager::TimingMethod method) {
    switch (method) {
        case OsTimeManager::NotSet:
            return "[Not Set]";

        case OsTimeManager::BestAvailable:
            return "[Best Available]";

        case OsTimeManager::SystemMethod1:
            return "GetTickCount";

        case OsTimeManager::SystemMethod2:
            return "QueryPerformanceCounter";

        default:
            return "[Unknown]";
    }
}

uint64_t OsTimeManager::Snapshot() {
    return static_cast<int64_t>(static_cast<double>(IOsWebClockNs()) * this->m_scaleToMs + this->m_timeBegin);
}

uint32_t OsGetAsyncTimeMs() {
    return static_cast<uint32_t>(s_OsTimeMgr->Snapshot());
}

uint64_t OsGetAsyncTimeClocks() {
    return IOsWebClockNs();
}

int64_t OsGetAsyncClocksPerSecond() {
    return 1000000000ll;
}

OsTimeManager::OsTimeManager(OsTimeManager::TimingMethod method) {
    this->m_timingMethod = NotSet;
    this->m_timingMethod = this->Calibrate();
    this->m_scaleToMs = 1000.0 / static_cast<double>(this->m_performanceFrequency);
    this->m_timeBegin = 0.0;
}

void OsTimeManagerCreate(OsTimeManager::TimingMethod method) {
    s_OsTimeMgr = new (SMemAlloc(sizeof(OsTimeManager), __FILE__, __LINE__, SMEM_FLAG_ZEROMEMORY)) OsTimeManager(method);
}

void OsTimeManagerDestroy() {
    if (s_OsTimeMgr) {
        delete s_OsTimeMgr;
        s_OsTimeMgr = nullptr;
    }
}

void OsSleep(uint32_t milliseconds) {
    if (milliseconds) {
        usleep(milliseconds * 1000);
    } else {
        sched_yield();
    }
}

#endif
