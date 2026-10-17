#if defined(WOWDECOMP_PLATFORM_WEB)

// Streaming (on-demand CDN download) is not used on web: every file is local, nothing is ever queued.

#include <cstdint>
#include <windows.h>
#include "Engine/Source/BlizzardCore/Source/Packages/Streaming/Streaming.h"

bool Blizzard::Streaming::CancelQueue(int32_t priority) {
    return true;
}

bool Blizzard::Streaming::RemoveCompletion(Completion* completion, int32_t priority) {
    return false;
}

// false means "nothing queued, nothing to wait for".
bool Blizzard::Streaming::QueueCompletion(Completion* completion, int32_t priority) {
    completion->m_result = 0;
    completion->m_done = false;
    completion->m_success = false;

    return false;
}

bool Blizzard::Streaming::MakeLocalAsync(Blizzard::File::StreamRecord* file, int64_t offset, int32_t size, int32_t priority, const char* name) {
    return true;
}

bool Blizzard::Streaming::MakeLocalBlocking(Blizzard::File::StreamRecord* file, int64_t offset, int32_t size, int32_t priority, const char* name) {
    return true;
}

// 1 = the whole range is local.
int32_t Blizzard::Streaming::CheckFileRange(const char* path, int64_t offset, int32_t size, const char* name, int32_t flags) {
    return 1;
}

int32_t Blizzard::Streaming::GetIsLocalAmount(const char* path, int64_t offset, int32_t size, const char* name) {
    return size;
}

bool Blizzard::Streaming::InitializeStreaming(const char* manifest, const char* storagePath, const char* variant, const char* dataPath, bool required) {
    return false;
}

bool Blizzard::Streaming::IsStreamedFile(const char* path) {
    return false;
}

void StopStreaming() {
}

char* GetComponentVersionData(uint32_t* size) {
    *size = 0;

    return nullptr;
}

#endif
