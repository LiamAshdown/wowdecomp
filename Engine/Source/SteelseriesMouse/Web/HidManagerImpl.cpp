#if defined(WOWDECOMP_PLATFORM_WEB)

#include <new>
#include "Engine/Source/SteelseriesMouse/HidManager.h"
#include "Engine/Source/Storm/H/storm.h"

HidManager::~HidManager() {
}

namespace {
    // No HID devices exist in the browser.
    class NullHidManager : public HidManager {
        public:
        bool EnumerateDevices() override { return false; }
        void SetWindow(void* window) override {}
        uint32_t GetDeviceCount() override { return 0; }
        bool WriteWait(const void* data, uint32_t size) override { return false; }
        bool Write(const void* data, uint32_t size) override { return false; }
        bool SetFeature(void* data, uint32_t size) override { return false; }
    };
}

HidManager* HidManager::Create() {
    return new (SMemAlloc(sizeof(NullHidManager), __FILE__, __LINE__, 0)) NullHidManager();
}

#endif
