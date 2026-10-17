#if defined(WOWDECOMP_PLATFORM_WEB)

// Web backend for the stacked filesystem: POSIX calls against Emscripten's virtual FS.
//
// Paths arrive Windows-style (backslashes, any case). Resolve() converts them to '/' and, when the exact path is
// missing, walks the components case-insensitively using cached directory listings.
//
// File access goes through WebVfs, which serves the read-only game files over HTTP in a browser (see WebVfs.h).

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <windows.h>
#include "Engine/Source/BlizzardCore/Source/System/File/File.h"
#include "Engine/Source/BlizzardCore/Source/System/File/Web/WebVfs.h"
#include "Engine/Source/BlizzardCore/Source/System/Memory/MemoryStorm.h"
#include "Engine/Source/BlizzardCore/Source/System/String/Path.h"
#include "Engine/Source/BlizzardCore/Source/System/String/String.h"
#include "Engine/Source/Storm/H/storm.h"

#undef CreateDirectory
#undef GetFreeSpace
#undef RemoveDirectory

// The io mode never changes on web, so no read_first/detect_io_mode dance.
System_File::IOFunc System_File::read_func = System_File::read_overlapped;
System_File::IOFunc System_File::write_func = System_File::write_overlapped;

namespace {

// ---- path resolution ---------------------------------------------------------------------------------------------

struct DirCache {
    std::mutex m_lock;
    // directory path -> (lowercased entry name -> actual entry name)
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> m_dirs;
};

DirCache& Cache() {
    static DirCache cache;
    return cache;
}

// Call after anything that creates, removes or renames directory entries.
void InvalidateDirs() {
    DirCache& cache = Cache();
    std::lock_guard<std::mutex> guard(cache.m_lock);
    cache.m_dirs.clear();
}

std::string Lower(const std::string& s) {
    std::string r(s);

    for (size_t i = 0; i < r.size(); i++) {
        r[i] = static_cast<char>(tolower(static_cast<unsigned char>(r[i])));
    }

    return r;
}

// Backslashes to '/', and "C:\x" to "/x".
std::string ToPosix(const char* path) {
    std::string s(path ? path : "");

    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\') {
            s[i] = '/';
        }
    }

    if (s.size() >= 2 && s[1] == ':' && isalpha(static_cast<unsigned char>(s[0]))) {
        s.erase(0, 2);
    }

    return s;
}

bool FindEntry(const std::string& dir, const std::string& name, std::string& actual) {
    DirCache& cache = Cache();
    std::lock_guard<std::mutex> guard(cache.m_lock);

    auto it = cache.m_dirs.find(dir);

    if (it == cache.m_dirs.end()) {
        std::unordered_map<std::string, std::string> entries;
        std::vector<std::pair<std::string, bool>> listing;
        WebVfs::ListDirectory(dir, listing);

        for (size_t i = 0; i < listing.size(); i++) {
            entries.emplace(Lower(listing[i].first), listing[i].first);
        }

        it = cache.m_dirs.emplace(dir, std::move(entries)).first;
    }

    auto found = it->second.find(Lower(name));

    if (found == it->second.end()) {
        return false;
    }

    actual = found->second;
    return true;
}

// Returns true if the path exists. `out` is always set to the best native path: the real-case path when it exists,
// otherwise the longest case-corrected prefix with the rest as given (usable for creating files).
bool Resolve(const char* in, std::string& out) {
    out = ToPosix(in);

    if (out.empty()) {
        return false;
    }

    if (WebVfs::access(out.c_str(), F_OK) == 0) {
        return true;
    }

    std::string abs(out);

    if (abs[0] != '/') {
        char cwd[1024];

        if (!::getcwd(cwd, sizeof(cwd))) {
            return false;
        }

        abs = std::string(cwd) + "/" + abs;
    }

    std::string cur;
    bool ok = true;
    size_t pos = 0;

    while (pos < abs.size()) {
        size_t end = abs.find('/', pos);

        if (end == std::string::npos) {
            end = abs.size();
        }

        std::string comp = abs.substr(pos, end - pos);
        pos = end + 1;

        if (comp.empty() || comp == ".") {
            continue;
        }

        if (comp == "..") {
            size_t slash = cur.rfind('/');
            cur.erase(slash == std::string::npos ? 0 : slash);
            continue;
        }

        std::string actual;

        if (ok && FindEntry(cur.empty() ? "/" : cur, comp, actual)) {
            cur += "/" + actual;
        } else {
            ok = false;
            cur += "/" + comp;
        }
    }

    out = cur.empty() ? "/" : cur;
    return ok;
}

// ---- helpers -----------------------------------------------------------------------------------------------------

int32_t FdOf(Blizzard::File::StreamRecord* stream) {
    return static_cast<int32_t>(reinterpret_cast<intptr_t>(stream->m_fileHandle));
}

bool HasHandle(Blizzard::File::StreamRecord* stream) {
    return stream->m_fileHandle != INVALID_HANDLE_VALUE;
}

// Blizzard::Time::Timestamp is nanoseconds since 2000-01-01 UTC.
int64_t ToTimestamp(const timespec& ts) {
    return (static_cast<int64_t>(ts.tv_sec) - 946684800) * 1000000000 + ts.tv_nsec;
}

void FromTimestamp(int64_t time, timespec* ts) {
    int64_t seconds = time / 1000000000;
    int64_t nanos = time % 1000000000;

    if (nanos < 0) {
        nanos += 1000000000;
        seconds--;
    }

    ts->tv_sec = static_cast<time_t>(seconds + 946684800);
    ts->tv_nsec = static_cast<long>(nanos);
}

void FillInfo(const struct stat& st, Blizzard::File::FileInfo* info) {
    bool isDir = S_ISDIR(st.st_mode);
    int32_t attributes = isDir ? 0x40 : 0x20;

    if (!(st.st_mode & 0222)) {
        attributes |= 0x1;
    }

    info->m_size = st.st_size;
    info->m_createTime = ToTimestamp(st.st_ctim);
    info->m_modTime = ToTimestamp(st.st_mtim);
    info->m_accessTime = ToTimestamp(st.st_atim);
    info->m_attributes = attributes;
    info->m_regularFile = !isDir;
    info->m_existence = isDir ? 2 : 1;
}

void CopyOut(char* buffer, int32_t size, const std::string& value) {
    if (!buffer || size <= 0) {
        return;
    }

    size_t count = std::min<size_t>(value.size(), static_cast<size_t>(size) - 1);
    memcpy(buffer, value.data(), count);
    buffer[count] = '\0';
}

// Maps the stacked FS mode bits (see Win64 ToCreateFlags): 0x1 read, 0x2 write, 0x100/0x400 create+truncate,
// 0x800 create-new, 0x1000 open-existing, otherwise open-always.
int ToOpenFlags(uint32_t mode) {
    int flags = O_RDONLY;

    if (mode & 0x2) {
        flags = (mode & 0x1) ? O_RDWR : O_WRONLY;
    }

    if ((mode & 0x100) || (mode & 0x400)) {
        flags |= O_CREAT | ((mode & 0x800) ? O_EXCL : O_TRUNC);
    } else if (mode & 0x800) {
        flags |= O_CREAT | O_EXCL;
    } else if (!(mode & 0x1000)) {
        flags |= O_CREAT;
    }

    return flags;
}

int ToWhence(int32_t posMode) {
    return posMode == 0 ? SEEK_SET : (posMode == 1 ? SEEK_CUR : SEEK_END);
}

// The single place file bytes are read from. offset < 0 reads at the current position.
int64_t WebRead(Blizzard::File::StreamRecord* file, void* buffer, int32_t count, int64_t offset) {
    uint8_t* data = static_cast<uint8_t*>(buffer);
    int64_t total = 0;

    while (total < count) {
        ssize_t got = offset < 0 ? WebVfs::read(FdOf(file), data + total, count - total) : WebVfs::pread(FdOf(file), data + total, count - total, offset + total);

        if (got < 0) {
            return -1;
        }

        if (got == 0) {
            break;
        }

        total += got;
    }

    return total;
}

// The single place file bytes are written to. offset < 0 writes at the current position.
int64_t WebWrite(Blizzard::File::StreamRecord* file, const void* buffer, int32_t count, int64_t offset) {
    const uint8_t* data = static_cast<const uint8_t*>(buffer);
    int64_t total = 0;

    while (total < count) {
        ssize_t put = offset < 0 ? WebVfs::write(FdOf(file), data + total, count - total) : WebVfs::pwrite(FdOf(file), data + total, count - total, offset + total);

        if (put <= 0) {
            return -1;
        }

        total += put;
    }

    return total;
}

bool NormalizeAbsolute(std::string path, std::string& out) {
    if (path.empty() || path[0] != '/') {
        char cwd[1024];

        if (!::getcwd(cwd, sizeof(cwd))) {
            return false;
        }

        path = std::string(cwd) + "/" + path;
    }

    std::vector<std::string> parts;
    size_t pos = 0;

    while (pos <= path.size()) {
        size_t end = path.find('/', pos);

        if (end == std::string::npos) {
            end = path.size();
        }

        std::string comp = path.substr(pos, end - pos);
        pos = end + 1;

        if (comp.empty() || comp == ".") {
            continue;
        }

        if (comp == "..") {
            if (!parts.empty()) {
                parts.pop_back();
            }

            continue;
        }

        parts.push_back(comp);
    }

    out.clear();

    for (size_t i = 0; i < parts.size(); i++) {
        out += "/" + parts[i];
    }

    if (out.empty()) {
        out = "/";
    }

    return true;
}

}

// ---- attributes and names ----------------------------------------------------------------------------------------

// The stacked FS attribute bits (0x1 readonly, 0x2 hidden, 0x4 system, 0x8 archive, 0x10 temporary, 0x20 normal,
// 0x40 directory) are used as-is; there are no Windows attribute values to translate on web.
int32_t System_File::ToNativeAttributes(int32_t attributes) {
    return attributes;
}

int32_t System_File::FromNativeAttributes(int32_t attributes) {
    return attributes;
}

void System_File::FromNativeName(char* buffer, uint32_t buffersize, const char* str) {
    CopyOut(buffer, static_cast<int32_t>(buffersize), std::string(str ? str : ""));
}

// ---- reads and writes --------------------------------------------------------------------------------------------

bool System_File::ReadP(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    OVERLAPPED overlapped;
    overlapped.Offset = static_cast<DWORD>(parms->m_pos);
    overlapped.OffsetHigh = static_cast<DWORD>(parms->m_pos >> 32);
    overlapped.Internal = 0;
    overlapped.InternalHigh = 0;
    overlapped.hEvent = nullptr;

    return read_func(parms->m_stream, parms->m_buffer, &overlapped, &parms->m_amount);
}

bool System_File::WriteP(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    OVERLAPPED overlapped;
    overlapped.Offset = static_cast<DWORD>(parms->m_pos);
    overlapped.OffsetHigh = static_cast<DWORD>(parms->m_pos >> 32);
    overlapped.Internal = 0;
    overlapped.InternalHigh = 0;
    overlapped.hEvent = nullptr;

    return write_func(parms->m_stream, parms->m_buffer, &overlapped, &parms->m_amount);
}

bool System_File::read_overlapped(Blizzard::File::StreamRecord* file, void* buffer, OVERLAPPED* overlapped, int32_t* count) {
    if (!HasHandle(file)) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    if (!*count) {
        return true;
    }

    int64_t offset = overlapped ? ((static_cast<int64_t>(overlapped->OffsetHigh) << 32) | overlapped->Offset) : -1;
    int64_t got = WebRead(file, buffer, *count, offset);

    if (got >= 0) {
        *count = static_cast<int32_t>(got);
        return true;
    }

    *count = 0;
    Blizzard::File::SetLastError(5);
    Blizzard::File::AddToLastErrorStack(5, Blizzard::String::QuickFormat<1024>("Web Read - %s", file->m_pathname).m_buffer, 1);

    return false;
}

bool System_File::write_overlapped(Blizzard::File::StreamRecord* file, void* buffer, OVERLAPPED* overlapped, int32_t* count) {
    if (!HasHandle(file)) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    if (!*count) {
        return true;
    }

    file->m_infoValid = false;
    int64_t offset = overlapped ? ((static_cast<int64_t>(overlapped->OffsetHigh) << 32) | overlapped->Offset) : -1;
    int64_t put = WebWrite(file, buffer, *count, offset);

    if (put >= 0) {
        *count = static_cast<int32_t>(put);
        return true;
    }

    *count = 0;
    Blizzard::File::SetLastError(6);
    Blizzard::File::AddToLastErrorStack(6, Blizzard::String::QuickFormat<1024>("Web Write - %s", file->m_pathname).m_buffer, 1);

    return false;
}

bool System_File::Read(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    return read_overlapped(parms->m_stream, parms->m_buffer, nullptr, &parms->m_amount);
}

bool System_File::Write(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    return write_overlapped(parms->m_stream, parms->m_buffer, nullptr, &parms->m_amount);
}

// ---- open / close / position -------------------------------------------------------------------------------------

bool System_File::Open(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    int32_t mode = parms->m_mode;
    char path[1024];
    Blizzard::String::MakeBackslashPath(parms->m_path, path, sizeof(path));

    std::string native;
    bool existed = Resolve(path, native);
    int fd = WebVfs::open(native.c_str(), ToOpenFlags(mode), 0666);

    if (fd < 0) {
        Blizzard::File::SetLastError(4);
        Blizzard::File::AddToLastErrorStack(4, Blizzard::String::QuickFormat<1024>("Web Open - %s", path).m_buffer, 1);
        return false;
    }

    if (!existed) {
        InvalidateDirs();
    }

    Blizzard::File::StreamRecord* stream = parms->m_stream;

    if (!stream) {
        uint32_t length = static_cast<uint32_t>(strlen(path));
        stream = static_cast<Blizzard::File::StreamRecord*>(SMemAlloc(sizeof(Blizzard::File::StreamRecord) + length + 1, __FILE__, __LINE__, SMEM_FLAG_ZEROMEMORY));
        stream->m_pathname = reinterpret_cast<char*>(stream + 1);
        Blizzard::String::Copy(stream->m_pathname, path, length + 1);
    }

    stream->m_fileHandle = reinterpret_cast<void*>(static_cast<intptr_t>(fd));
    stream->m_mode = mode;
    stream->m_infoValid = false;
    stream->m_info.m_path = stream->m_pathname;
    GetFileInfoByFile(stream);

    if (mode & 0x200) {
        WebVfs::lseek(fd, 0, SEEK_END);
    }

    parms->m_stream = stream;

    return true;
}

bool System_File::Close(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    Blizzard::File::StreamRecord* stream = parms->m_stream;

    if (HasHandle(stream)) {
        WebVfs::close(FdOf(stream));
    }

    SMemFree(stream, __FILE__, __LINE__, 0);

    return true;
}

bool System_File::Flush(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    if (!HasHandle(parms->m_stream)) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    return true;
}

bool System_File::GetPos(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    Blizzard::File::StreamRecord* stream = parms->m_stream;
    off_t pos;

    if (!HasHandle(stream) || (pos = WebVfs::lseek(FdOf(stream), 0, SEEK_CUR)) < 0) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    parms->m_pos = pos;

    return true;
}

bool System_File::SetPos(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    Blizzard::File::StreamRecord* stream = parms->m_stream;

    if (!HasHandle(stream)) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    return WebVfs::lseek(FdOf(stream), static_cast<off_t>(parms->m_pos), ToWhence(parms->m_posMode)) >= 0;
}

bool System_File::SetEOF(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    Blizzard::File::StreamRecord* stream = parms->m_stream;

    if (!HasHandle(stream)) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    int fd = FdOf(stream);
    off_t previous = WebVfs::lseek(fd, 0, SEEK_CUR);
    off_t target = WebVfs::lseek(fd, static_cast<off_t>(parms->m_pos), ToWhence(parms->m_posMode));

    if (previous < 0 || target < 0) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    bool result = WebVfs::ftruncate(fd, target) == 0;
    WebVfs::lseek(fd, previous, SEEK_SET);
    stream->m_infoValid = false;

    return result;
}

// Windows reopened the handle to toggle FILE_FLAG_NO_BUFFERING; there is nothing to switch on web.
bool System_File::SetCacheMode(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    Blizzard::File::StreamRecord* stream = parms->m_stream;
    stream->m_mode = parms->m_mode | (stream->m_mode & ~0x40);

    return true;
}

bool System_File::Create(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    Blizzard::File::SetLastError(9);

    return false;
}

// ---- file info ---------------------------------------------------------------------------------------------------

void System_File::GetFileInfoByFile(Blizzard::File::StreamRecord* file) {
    if (file->m_infoValid) {
        return;
    }

    file->m_infoValid = false;
    struct stat st;

    if (!HasHandle(file) || WebVfs::fstat(FdOf(file), &st) != 0) {
        return;
    }

    int64_t streamedSize = file->m_info.m_size;
    FillInfo(st, &file->m_info);

    if (file->m_streaming && file->m_streaming->m_entry) {
        file->m_info.m_size = streamedSize;
    }

    file->m_infoValid = true;
}

bool System_File::Exists(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    std::string native;
    struct stat st;

    if (!Resolve(parms->m_path, native) || WebVfs::stat(native.c_str(), &st) != 0) {
        parms->m_info->m_existence = 0;
    } else {
        parms->m_info->m_existence = S_ISDIR(st.st_mode) ? 2 : 1;
    }

    return true;
}

bool System_File::GetFileInfo(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    if (parms->m_path) {
        std::string native;
        struct stat st;

        if (!Resolve(parms->m_path, native) || WebVfs::stat(native.c_str(), &st) != 0) {
            Blizzard::File::SetLastError(8);
            return false;
        }

        FillInfo(st, parms->m_info);
        parms->m_info->m_path = nullptr;

        return true;
    }

    if (parms->m_stream) {
        GetFileInfoByFile(parms->m_stream);
        Blizzard::File::StreamRecord* stream = parms->m_stream;
        parms->m_info = &stream->m_info;

        if (!stream->m_info.m_path) {
            stream->m_info.m_path = stream->m_pathname;
        }

        return parms->m_stream->m_infoValid;
    }

    Blizzard::File::SetLastError(8);

    return false;
}

bool System_File::IsReadOnly(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    std::string native;
    struct stat st;

    if (Resolve(parms->m_path, native) && WebVfs::stat(native.c_str(), &st) == 0) {
        return !(st.st_mode & 0222);
    }

    Blizzard::File::SetLastError(8);

    return false;
}

bool System_File::SetAttributes(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    std::string native;

    if (parms->m_has.m_all & 0x10) {
        timespec times[2];
        times[0].tv_sec = 0;
        times[0].tv_nsec = UTIME_OMIT;
        FromTimestamp(parms->m_info->m_modTime, &times[1]);

        if (!HasHandle(parms->m_stream) || ::futimens(FdOf(parms->m_stream), times) != 0) {
            Blizzard::File::SetLastError(8);
            return false;
        }

        parms->m_stream->m_info.m_modTime = parms->m_info->m_modTime;
        parms->m_has.m_all &= ~0x10u;
    }

    if (parms->m_has.m_all & 0x4) {
        if (!Resolve(parms->m_path, native) || ::chmod(native.c_str(), (parms->m_info->m_attributes & 0x1) ? 0444 : 0666) != 0) {
            Blizzard::File::SetLastError(8);
            return false;
        }

        parms->m_has.m_all &= ~0x4u;
    }

    return parms->m_has.m_all == 0;
}

bool System_File::GetFreeSpace(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    const char* path = parms->m_path;

    if (path && *path) {
        // ponytail: the virtual FS has no quota; report plenty so free-space checks pass.
        parms->m_pos = 1LL << 40;
        return true;
    }

    Blizzard::File::SetLastError(8);

    return false;
}

// ---- names and working directory ---------------------------------------------------------------------------------

bool System_File::GetRootChars(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    std::string univ(parms->m_path ? parms->m_path : "");

    for (size_t i = 0; i < univ.size(); i++) {
        if (univ[i] == '\\') {
            univ[i] = '/';
        }
    }

    if (univ.size() >= 2 && univ[1] == ':') {
        parms->m_pos = (univ.size() > 2 && univ[2] == '/') + 2;
    } else if (!univ.empty() && univ[0] == '/') {
        parms->m_pos = 1;
    } else {
        parms->m_pos = 0;
    }

    return true;
}

bool System_File::IsAbsolutePath(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    const char* path = parms->m_path;

    if (path[0] == '\\' || path[0] == '/' || (isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':' && (path[2] == '\\' || path[2] == '/') && path[3])) {
        return true;
    }

    Blizzard::File::SetLastError(8);

    return false;
}

bool System_File::MakeAbsolutePath(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    std::string absolute;

    if (!NormalizeAbsolute(ToPosix(parms->m_path), absolute)) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    const char* path = parms->m_path;
    size_t length = path ? strlen(path) : 0;

    if (length && (path[length - 1] == '/' || path[length - 1] == '\\') && absolute != "/") {
        absolute += "/";
    }

    CopyOut(parms->m_cwdbuf, parms->m_cwdbuflen, absolute);

    return true;
}

bool System_File::GetWorkingDirectory(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    if (!parms->m_cwdbuf || parms->m_cwdbuflen <= 0) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    *parms->m_cwdbuf = '\0';
    char buffer[1024];

    if (!::getcwd(buffer, sizeof(buffer))) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    CopyOut(parms->m_cwdbuf, parms->m_cwdbuflen, buffer);

    return true;
}

bool System_File::SetWorkingDirectory(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    if (!parms->m_path) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    std::string native;

    return Resolve(parms->m_path, native) && ::chdir(native.c_str()) == 0;
}

// ---- directories -------------------------------------------------------------------------------------------------

bool System_File::ProcessDirFast(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    void* cookie = parms->m_cookie;
    const char* path = parms->m_path;
    Blizzard::File::ProcessDirCallback callback = parms->m_dircallback;

    std::string native;
    // Collect first: callbacks may delete entries while we walk. Sort case-insensitively to match NTFS ordering.
    std::vector<std::pair<std::string, bool>> entries;

    if (!Resolve(path, native) || !WebVfs::ListDirectory(native, entries)) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    std::sort(entries.begin(), entries.end(), [](const std::pair<std::string, bool>& a, const std::pair<std::string, bool>& b) {
        return Lower(a.first) < Lower(b.first);
    });

    Blizzard::File::ProcessDirParms dirParms;
    dirParms.m_pathRoot = path;
    dirParms.m_cookie = cookie;

    for (size_t i = 0; i < entries.size(); i++) {
        dirParms.m_filename = entries[i].first.c_str();
        dirParms.m_isDir = entries[i].second;

        if (!callback(dirParms)) {
            return false;
        }
    }

    return true;
}

bool System_File::CreateDirectory(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    const char* path = parms->m_path;

    if (!path) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    std::string posix = ToPosix(path);
    std::string native;

    if (parms->m_recursive) {
        size_t pos = 0;

        while (pos < posix.size()) {
            size_t end = posix.find('/', pos);

            if (end == std::string::npos) {
                end = posix.size();
            }

            pos = end + 1;

            if (end == 0) {
                continue;
            }

            struct stat st;
            Resolve(posix.substr(0, end).c_str(), native);

            if (WebVfs::stat(native.c_str(), &st) == 0) {
                if (!S_ISDIR(st.st_mode)) {
                    Blizzard::File::SetLastError(8);
                    return false;
                }

                continue;
            }

            int result = ::mkdir(native.c_str(), 0777);
            int error = errno;
            InvalidateDirs();

            if (result != 0 && error != EEXIST) {
                Blizzard::File::SetLastError(8);
                return false;
            }
        }

        return true;
    }

    Resolve(posix.c_str(), native);

    if (::mkdir(native.c_str(), 0777) == 0) {
        InvalidateDirs();
        return true;
    }

    return errno == EEXIST || errno == EACCES;
}

bool System_File::RemoveDirectory(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    const char* path = parms->m_path;

    if (parms->m_recursive) {
        return Blizzard::File::RemoveDirectoryAndContents(path, false);
    }

    if (!path) {
        Blizzard::File::SetLastError(8);
        return false;
    }

    std::string native;

    if (!Resolve(path, native) || ::rmdir(native.c_str()) != 0) {
        return false;
    }

    InvalidateDirs();

    return true;
}

// ---- delete / move / copy ----------------------------------------------------------------------------------------

bool System_File::Delete(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    std::string native;

    if (!Resolve(parms->m_path, native) || WebVfs::unlink(native.c_str()) != 0) {
        return false;
    }

    InvalidateDirs();

    return true;
}

bool System_File::Move(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    std::string src;
    std::string dst;

    // Like MoveFile, refuse to overwrite.
    if (!Resolve(parms->m_path, src) || Resolve(parms->m_newpath, dst) || WebVfs::rename(src.c_str(), dst.c_str()) != 0) {
        return false;
    }

    InvalidateDirs();

    return true;
}

bool System_File::Copy(Blizzard::File::Filesystem* fs, System_File::Stacked::FileParms* parms) {
    bool overwrite = parms->m_overwrite;

    Blizzard::File::StreamRecord* input;

    if (!Blizzard::File::Open(parms->m_path, 0x1005, input)) {
        Blizzard::File::SetLastError(2);
        return false;
    }

    Blizzard::File::StreamRecord* output;

    if (!Blizzard::File::Open(parms->m_newpath, (overwrite ? 0x400 : 0xC00) | 0x2, output)) {
        Blizzard::File::Close(input);
        Blizzard::File::SetLastError(4);
        return false;
    }

    int64_t size = Blizzard::File::GetFileInfo(input)->m_size;
    int64_t offset = 0;
    bool result = true;
    void* buffer = Blizzard::Memory::Allocate(size >= 0xA00000 ? 0xA00000 : static_cast<uint32_t>(size));

    while (result && offset < size) {
        int32_t count = size - offset >= 0xA00000 ? 0xA00000 : static_cast<int32_t>(size - offset);

        if (!Blizzard::File::Read(input, buffer, offset, &count) || !count || !Blizzard::File::Write(output, buffer, offset, &count)) {
            result = false;
        } else {
            offset += count;
        }
    }

    Blizzard::Memory::Free(buffer);
    Blizzard::File::Close(input);
    Blizzard::File::Close(output);

    if (!result) {
        Blizzard::File::Delete(parms->m_newpath);
    }

    return result;
}

#endif
