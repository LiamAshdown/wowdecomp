#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include "Engine/Source/Storm/Source/IOUnit/IOFileUnit.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/Storm/Source/SOption.h"

// IOFileParam carries Win32 CreateFile arguments. Only the few values below are ever passed.
#define WEB_GENERIC_WRITE 0x40000000
#define WEB_CREATE_NEW 1
#define WEB_CREATE_ALWAYS 2
#define WEB_OPEN_EXISTING 3
#define WEB_OPEN_ALWAYS 4
#define WEB_TRUNCATE_EXISTING 5

// m_handle holds the file descriptor plus one, so that descriptor 0 is not a null handle.
static int HandleToFd(void* handle) {
    return static_cast<int>(reinterpret_cast<intptr_t>(handle) - 1);
}

namespace Storm {

static void* OpenFileHandle(const IOFileParam* param) {
    int flags = (param->m_access & WEB_GENERIC_WRITE) ? O_RDWR : O_RDONLY;

    switch (param->m_creation) {
        case WEB_CREATE_NEW:
            flags |= O_CREAT | O_EXCL;
            break;
        case WEB_CREATE_ALWAYS:
            flags |= O_CREAT | O_TRUNC;
            break;
        case WEB_OPEN_ALWAYS:
            flags |= O_CREAT;
            break;
        case WEB_TRUNCATE_EXISTING:
            flags |= O_TRUNC;
            break;
        case WEB_OPEN_EXISTING:
        default:
            break;
    }

    char path[1024];
    SStrCopy(path, param->m_name, sizeof(path));

    for (char* c = path; *c; c++) {
        if (*c == '\\') {
            *c = '/';
        }
    }

    int fd = open(path, flags, 0666);

    return fd < 0 ? nullptr : reinterpret_cast<void*>(static_cast<intptr_t>(fd) + 1);
}

IOUnit::IOUnit() {
    this->m_refcount = nullptr;
    this->m_count = 1;
}

void IOUnit::Release() {
    IORefcount* refcount = this->m_refcount;

    if (--this->m_count == 0) {
        delete this;
    }

    if (refcount) {
        refcount->Release();
    }
}

IOUnit::~IOUnit() {
}

IOWin32FileUnit::IOWin32FileUnit(void* handle) {
    this->m_handle = handle;
    this->m_position = -1;
}

// pread/pwrite take an explicit offset, so there is no file position to keep in sync.
bool IOWin32FileUnit::Seek(int64_t offset) {
    this->m_position = offset;
    return true;
}

bool IOWin32FileUnit::Read(void* buffer, int64_t offset, uint32_t count, uint32_t* bytesRead) {
    uint32_t minimum = 1;
    uint32_t localRead;

    if (!bytesRead) {
        minimum = count;
        bytesRead = &localRead;
    }

    *bytesRead = 0;

    while (*bytesRead < count) {
        ssize_t result = pread(HandleToFd(this->m_handle), static_cast<uint8_t*>(buffer) + *bytesRead, count - *bytesRead, offset + *bytesRead);

        if (result < 0) {
            return false;
        }

        if (result == 0) {
            break;
        }

        *bytesRead += static_cast<uint32_t>(result);
    }

    return *bytesRead >= minimum;
}

bool IOWin32FileUnit::Write(const void* buffer, int64_t offset, uint32_t count) {
    uint32_t written = 0;

    while (written < count) {
        ssize_t result = pwrite(HandleToFd(this->m_handle), static_cast<const uint8_t*>(buffer) + written, count - written, offset + written);

        if (result <= 0) {
            return false;
        }

        written += static_cast<uint32_t>(result);
    }

    return true;
}

bool IOWin32FileUnit::Flush() {
    return fsync(HandleToFd(this->m_handle)) == 0;
}

bool IOWin32FileUnit::GetFileSize(int64_t* size) {
    off_t end = lseek(HandleToFd(this->m_handle), 0, SEEK_END);

    *size = end < 0 ? 0 : end;

    return end >= 0;
}

bool IOWin32FileUnit::SetFileSize(int64_t size) {
    return ftruncate(HandleToFd(this->m_handle), size) == 0;
}

void* IOWin32FileUnit::Win32Handle() {
    return this->m_handle;
}

IOWin32FileUnit::~IOWin32FileUnit() {
    close(HandleToFd(this->m_handle));
    this->m_handle = nullptr;
}

IOWin32FileUnit* IOWin32FileUnit::Open(const IOFileParam* param) {
    void* handle = OpenFileHandle(param);

    if (!handle) {
        return nullptr;
    }

    return new IOWin32FileUnit(handle);
}

// FILE_FLAG_NO_BUFFERING only exists to bypass the Windows cache. Sector alignment means nothing here, so the
// unbuffered wrapper is never built.
IOUnit* IOFileUnit::Create(const IOFileParam& param, IOUnit* next) {
    if (next) {
        return nullptr;
    }

    return IOWin32FileUnit::Open(&param);
}

}

#endif
