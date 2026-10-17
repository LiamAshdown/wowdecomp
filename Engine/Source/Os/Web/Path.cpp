#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <windows.h>
#include "Engine/Source/Os/Path.h"

// There is no executable on web; pretend Wow.exe sits in the directory the client was started in (see
// Blizzard::Process::GetProcessPath).
uint32_t GetModuleFileNameUTF8(void* module, char* path, uint32_t size) {
    if (!size) {
        return 0;
    }

    char cwd[1024];
    const char* dir = getcwd(cwd, sizeof(cwd)) ? cwd : "";
    snprintf(path, size, "%s/Wow.exe", dir);

    return static_cast<uint32_t>(strlen(path));
}

// No command line on web.
CommandLineUTF8::CommandLineUTF8() {
    m_commandLine = new char[1];
    m_commandLine[0] = '\0';
}

CommandLineUTF8::~CommandLineUTF8() {
    delete[] m_commandLine;
}

#endif
