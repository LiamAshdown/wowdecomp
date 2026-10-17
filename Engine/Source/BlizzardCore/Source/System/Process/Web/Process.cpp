#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <unistd.h>
#include "Engine/Source/BlizzardCore/Source/System/Process/Process.h"

// There is no executable on web. Pretend Wow.exe sits in the directory the client was started in: the root of the
// virtual file system in a browser, the current directory under Node.
static const std::string& ProcessDirectory() {
    static const std::string dir = [] {
        char cwd[1024];
        std::string d = getcwd(cwd, sizeof(cwd)) ? cwd : "/";
        return d.back() == '/' || d.back() == '\\' ? d : d + "/";
    }();
    return dir;
}

bool Blizzard::Process::GetProcessPath(char* buffer, int32_t buffersize) {
    if (!buffer || buffersize <= 0) {
        return false;
    }

    strncpy(buffer, (ProcessDirectory() + "Wow.exe").c_str(), buffersize);
    buffer[buffersize - 1] = '\0';

    return true;
}

bool Blizzard::Process::GetProcessDirectory(char* buffer, int32_t buffersize) {
    if (buffer && buffersize > 0) {
        strncpy(buffer, ProcessDirectory().c_str(), buffersize);
        buffer[buffersize - 1] = '\0';
    }

    return true;
}

// A browser tab cannot spawn processes.
bool Blizzard::Process::LaunchChildProcessWithArguments(const char* path, const char* arguments, void (*callback)(void*), void* param) {
    return false;
}

void Blizzard::Process::Sleep(uint32_t milliseconds) {
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = static_cast<long>(milliseconds % 1000) * 1000000L;
    nanosleep(&ts, nullptr);
}

#endif
