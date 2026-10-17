#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cstdint>
#include <unistd.h>
#include "Engine/Source/BlizzardCore/Source/System/Hardware/Hardware.h"

Blizzard::Hardware::ProcessorFeatures System_Hardware::s_processorFeatures;

bool System_Hardware::s_initialized;

Blizzard::Lock::Atomic System_Hardware::s_initCount;

// wasm has no CPUID and the SSE/MMX/3DNow paths are not compiled in, so no feature bits are reported;
// callers then take their portable code.
bool System_Hardware::ReadCPUID(CPUIDInfo* info) {
    return false;
}

void System_Hardware::InitProcessorFeatures() {
    long processors = sysconf(_SC_NPROCESSORS_ONLN);

    s_processorFeatures.m_vendor = 0;
    s_processorFeatures.m_features = 0;
    s_processorFeatures.m_processorCount = processors > 0 ? static_cast<uint32_t>(processors) : 1;
    s_processorFeatures.m_coreCount = s_processorFeatures.m_processorCount;
}

uint32_t Blizzard::Hardware::GetProcessorFeatures() {
    if (!System_Hardware::s_initialized) {
        if (System_Hardware::s_initCount.Increment() == 1) {
            System_Hardware::InitProcessorFeatures();
            System_Hardware::s_initialized = true;
            return System_Hardware::s_processorFeatures.m_features;
        }

        while (!System_Hardware::s_initialized) {
            usleep(1000);
        }
    }

    return System_Hardware::s_processorFeatures.m_features;
}

#endif
