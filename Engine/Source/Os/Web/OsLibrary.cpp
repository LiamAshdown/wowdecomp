#if defined(WOWDECOMP_PLATFORM_WEB)

#include "Engine/Source/Os/OsLibrary.h"

// No dynamic loading in the browser build.

void* OsLoadLibrary(const char* fileName) {
    return nullptr;
}

void OsFreeLibrary(void* module) {
}

void* OsGetLibraryEntryPoint(void* module, const char* name) {
    return nullptr;
}

#endif
