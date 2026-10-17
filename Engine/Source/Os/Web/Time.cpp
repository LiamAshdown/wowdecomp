#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cstdint>
#include <ctime>
#include "Engine/Source/Os/Time.h"

int32_t OsGetTimeZone() {
    time_t now = time(nullptr);
    tm local;

    if (!localtime_r(&now, &local)) {
        return 0;
    }

    // Minutes east of UTC, like -Bias on Windows.
    return static_cast<int32_t>(local.tm_gmtoff / 60);
}

void OsGetTimeStamp(char* buffer, uint32_t size) {
    time_t now = time(nullptr);
    tm local;

    localtime_r(&now, &local);
    strftime(buffer, size, "%m%d%y_%H%M%S", &local);
}

#endif
