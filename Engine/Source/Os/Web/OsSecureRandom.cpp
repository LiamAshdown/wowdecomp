#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cstdint>
#include <unistd.h>
#include "Engine/Source/Os/OsSecureRandom.h"

void OsSecureRandom(uint8_t* buffer, uint32_t size) {
    // getentropy is backed by crypto.getRandomValues and takes at most 256 bytes per call.
    while (size) {
        uint32_t count = size > 256 ? 256 : size;
        getentropy(buffer, count);
        buffer += count;
        size -= count;
    }
}

#endif
