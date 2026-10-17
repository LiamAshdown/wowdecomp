#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cstdint>
#include <windows.h>
#include "Engine/Source/BlizzardCore/Source/System/Lock/Lock.h"

int32_t Blizzard::Lock::Atomic::Increment() {
    return InterlockedIncrement(reinterpret_cast<volatile LONG*>(&this->m_value));
}

int32_t Blizzard::Lock::Atomic::Decrement() {
    return InterlockedDecrement(reinterpret_cast<volatile LONG*>(&this->m_value));
}

#endif
