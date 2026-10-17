#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cstdint>
#include <windows.h>
#include "Engine/Source/BlizzardCore/Source/Packages/iounit/IOFileUnit.h"
#include "Engine/Source/BlizzardCore/Source/System/File/File.h"
#include "Engine/Source/BlizzardCore/Source/System/Memory/MemoryStorm.h"

// Windows bypassed the cache with an unbuffered handle; on web the stacked filesystem is the only path.
bool IOFileUnit::ReadNative(uint32_t offset, const char* path, void* buffer, uint32_t count, uint32_t* bytesRead) {
    Blizzard::File::StreamRecord* file;

    if (!Blizzard::File::Open(path, 0x1005, file)) {
        return false;
    }

    int32_t amount = static_cast<int32_t>(count);
    bool result = Blizzard::File::Read(file, buffer, static_cast<int64_t>(offset), &amount);
    Blizzard::File::Close(file);

    if (result && bytesRead) {
        *bytesRead = static_cast<uint32_t>(amount);
    }

    return result;
}

IOUnbufferedFileUnit::IOUnbufferedFileUnit(Blizzard::File::StreamRecord* stream, bool ownsMemory) : IOFileUnit(stream, ownsMemory) {
    this->m_size = Blizzard::File::GetFileInfo(stream)->m_size;
    this->m_alignMask = 0xFFF;
    this->m_buffer = Blizzard::Memory::Allocate(this->m_alignMask + 0x100000);
    this->m_alignedBuffer = reinterpret_cast<void*>(~static_cast<uintptr_t>(this->m_alignMask) & (reinterpret_cast<uintptr_t>(this->m_buffer) + this->m_alignMask));
}

#endif
