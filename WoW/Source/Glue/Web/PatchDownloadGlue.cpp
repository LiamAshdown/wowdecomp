#if defined(WOWDECOMP_PLATFORM_WEB)

#include "WoW/Source/Glue/CGlueMgr.h"

// A downloaded patch cannot be run from the browser.
bool CGlueMgr::PatchDownloadExecuteCommand(const char* path, const char* arguments) {
    return false;
}

#endif
