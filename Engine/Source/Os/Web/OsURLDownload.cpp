#if defined(WOWDECOMP_PLATFORM_WEB)

#include "Engine/Source/Os/OsURLDownload.h"

bool OsURLDownload(const char* url, OsURLDownloadCallback callback, void* param) {
    // ponytail: always fails; a fetch()-backed version can come with the bridge work if anything needs it.
    return false;
}

#endif
