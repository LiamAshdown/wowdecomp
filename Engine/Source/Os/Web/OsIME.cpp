#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cstdint>
#include <cstring>
#include "Engine/Source/Os/OsIME.h"

// No IME on the web yet: committed text arrives as OS_INPUT_CHAR events instead.

int32_t OsIMEGetLanguageMode() {
    return 0;
}

int32_t OsIMEGetCompositionString(char* buffer, uint32_t bufferSize) {
    memset(buffer, 0, bufferSize);
    return 0;
}

int32_t OsIMEGetCompositionResult(char* buffer, uint32_t bufferSize) {
    memset(buffer, 0, bufferSize);
    return 0;
}

int32_t OsIMEGetClauseInfo(uint32_t& clauseStart, uint32_t& clauseEnd, uint32_t& cursor) {
    return 0;
}

void OsIMEEnable(int32_t enable) {
}

void OsIMESync() {
}

void OsIMESetLanguageMode(int32_t native) {
}

void OsIMEInitialize() {
}

void OsIMEDestroy() {
}

int32_t OsIMEGetCandidates(uint32_t listMask, uint32_t& pageStart, uint32_t& pageSize, uint32_t& count, uint32_t& selection, TSGrowableArray<OsIMECandidate>& candidates) {
    candidates.Clear();
    return 0;
}

#endif
