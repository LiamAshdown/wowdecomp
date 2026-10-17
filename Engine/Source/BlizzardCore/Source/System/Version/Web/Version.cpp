#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cstdint>
#include "Engine/Source/BlizzardCore/Source/System/Version/Version.h"

// No PE version resources in a browser. Callers treat a failure as "version unknown".
bool Blizzard::Version::GetFileVersion(const char* path, FileVersion& version) {
    version.m_major = 0;
    version.m_minor = 0;
    version.m_build = 0;
    version.m_revision = 0;

    return false;
}

#endif
