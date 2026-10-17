#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "Engine/Source/BlizzardCore/Source/System/Debug/Debug.h"
#include "Engine/Source/BlizzardCore/Source/System/String/String.h"
#include "Engine/Source/BlizzardCore/Source/System/Time/Time.h"

void Blizzard::Debug::Breakpoint() {
}

// The Windows version also appends to Errors/Assert.log; the console is the only sink worth having in a browser.
void Blizzard::Debug::DefaultCallback(const char* expression, const char* message, const char* fileName, uint32_t lineNumber) {
    char text[2048];
    char canonical[256];
    Blizzard::Time::TimeRec rec;

    Blizzard::Time::BreakTime(Blizzard::Time::GetTimestamp(), rec);
    Blizzard::String::Format(text, sizeof(text), "%04d/%02d/%02d %02d:%02d:%02d  %s(%d) : Assertion failed: %s  \"%s\"\n", rec.year, rec.month, rec.day, rec.hour, rec.minute, rec.second, CanonicalStr(fileName, canonical, sizeof(canonical)), lineNumber, expression, message);
    fputs(text, stderr);
    abort();
}

void System_Debug::ShutdownProc() {
}

#endif
