#if defined(WOWDECOMP_PLATFORM_WEB)

#include "Engine/Source/Storm/H/storm.h"

// Win64 resolves TryEnterCriticalSection from kernel32 here; nothing to look up on the web.
void SServerInitialize() {
}

void SServerDestroy() {
}

#endif
