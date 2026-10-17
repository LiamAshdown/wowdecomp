#if defined(WOWDECOMP_PLATFORM_WEB)

#include "WoW/Source/Battlenet/Battlenet.h"

// There are no native battle.net libraries to load in the browser.
Battlenet::Library* Battlenet::Library::Load(const char* path, Allocator& allocator) {
    return nullptr;
}

#endif
