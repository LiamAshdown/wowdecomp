#if defined(WOWDECOMP_PLATFORM_WEB)

#include "Engine/Source/Storm/H/storm.h"
#include "WoW/Source/Client.h"

// Browser entry point. -sPROXY_TO_PTHREAD runs main() on a pthread with an 8 MB stack (-sSTACK_SIZE), which stands
// in for the Win64 build's 8 MB main fiber and lets the game block like it does on Windows.
int main() {
    StormRtlInitialize();
    return CommonMain();
}

#endif
