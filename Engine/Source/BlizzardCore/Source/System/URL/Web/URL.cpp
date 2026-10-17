#if defined(WOWDECOMP_PLATFORM_WEB)

#include "Engine/Source/BlizzardCore/Source/System/URL/URL.h"

// No HTTP downloads in the browser build (patch/streaming downloads are disabled); report failure to the caller.
bool Blizzard::URL::DownloadURL(const char* url, DownloadCallback* callback) {
    return false;
}

#endif
