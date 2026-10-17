#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cstdint>
#include <cstring>
#include <emscripten.h>
#include <unistd.h>
#include "Engine/Source/Os/Processor.h"
#include "Engine/Source/Os/System.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Storm/H/storm.h"

static int32_t s_sleepInBackground = 1;
static uint32_t s_backgroundSleepMs;
static int32_t s_cpuLogEnabled;

uint32_t OsGetNumberOfProcessors() {
    long count = sysconf(_SC_NPROCESSORS_ONLN);

    return count > 0 ? static_cast<uint32_t>(count) : 1;
}

int32_t OsSleepInBackground() {
    return s_sleepInBackground;
}

void OsSetSleepInBackground(int32_t sleep) {
    s_sleepInBackground = sleep;
}

uint32_t OsGetBackgroundSleepMs() {
    return s_backgroundSleepMs;
}

void OsSetBackgroundSleepMs(uint32_t milliseconds) {
    s_backgroundSleepMs = milliseconds;
}

OsType OsGetVersion() {
    return OsType_Linux;
}

void OsGetVersionString(char* buffer, int32_t size) {
    SStrCopy(buffer, "Linux", size);
}

// The auth challenge still identifies the client as the original Windows x86 build.
uint32_t OsGetProcessorId() {
    return 0x783836;
}

uint32_t OsGetOsId() {
    return 0x57696E;
}

static int32_t IOsWebCopyName(const char* name, char* buffer, uint32_t* size) {
    uint32_t length = static_cast<uint32_t>(strlen(name));

    if (!buffer || !size || *size <= length) {
        if (size) {
            *size = length + 1;
        }

        return 0;
    }

    memcpy(buffer, name, length + 1);
    *size = length;

    return 1;
}

int32_t OsGetComputerName(char* buffer, uint32_t* size) {
    return IOsWebCopyName("browser", buffer, size);
}

int32_t OsGetUserName(char* buffer, uint32_t* size) {
    return IOsWebCopyName("player", buffer, size);
}

uint64_t OsGetPhysicalMemory() {
    // ponytail: fixed 4 GB (the wasm32 ceiling); navigator.deviceMemory is capped at 8 and rounded anyway.
    return 4ull * 1024 * 1024 * 1024;
}

void* CreateNamedEventA(const char* name, int32_t manualReset) {
    // Single-instance guard on Windows; nothing to guard in a tab.
    return nullptr;
}

void OsValidateFunctionPointer(const void* ptr) {
    // wasm function pointers are table indices, not addresses, so there is no .text range to check.
}

int32_t OsIsRemoteSession() {
    return 0;
}

int32_t OsLaunchURL(const char* url) {
    if (!url || !*url) {
        return 0;
    }

    // Popup blockers may swallow this when it is not inside a user gesture.
    MAIN_THREAD_ASYNC_EM_ASM({ window.open(UTF8ToString($0), '_blank', 'noopener'); }, url);

    return 1;
}

void OsSystemEnableCpuLog() {
    s_cpuLogEnabled = 1;
}

uint32_t OsGetProcessorFeaturesEx(int32_t& vendor) {
    vendor = 4;

    // TSC, MMX, SSE, SSE2 plus the "valid" bit (see the Win64 decode of cpuid feature bits).
    return 0x80000017;
}

uint64_t OsGetProcessorTicksPerSecond() {
    // Only compared against 1.5 GHz to pick a CPU tier; report a modern desktop.
    return 3000000000ull;
}

#endif
