#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cstdint>
#include "Engine/Source/Os/OsVersionHash.h"

// The executable checksum and file version resources are PE concepts; there is no executable to hash.

int32_t IOsVersionChecksum(IOsVersionHashCallback& callback, const char* path, int32_t loaded) {
    return 0;
}

OsFileVersionInfo::OsFileVersionInfo(const char* filename) : m_fileVersion(), m_productVersion() {
    m_data.Clear();
}

#endif
