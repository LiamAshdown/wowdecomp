#if defined(WOWDECOMP_PLATFORM_WEB)

#include "Engine/Source/BlizzardCore/Source/System/File/Web/WebVfs.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <dirent.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>
#include <emscripten.h>

EM_JS_DEPS(WebVfs, "$stringToNewUTF8,$UTF8ToString");

// Synchronous XHR from the calling (game or worker) pthread; sync XHR with an arraybuffer response is allowed in
// workers. Returns the manifest text (malloc'd) or null when there is no XMLHttpRequest (Node) or no manifest.
EM_JS(char*, WebVfsFetchManifest, (), {
    if (typeof XMLHttpRequest === "undefined") {
        return 0;
    }
    var xhr = new XMLHttpRequest();
    xhr.open("GET", "/game-manifest.txt", false);
    try {
        xhr.send();
    } catch (e) {
        return 0;
    }
    return xhr.status === 200 ? stringToNewUTF8(xhr.responseText) : 0;
});

// Reads [offset, offset + count) of a game file into dst. Returns the byte count, or -1.
EM_JS(int, WebVfsFetchRange, (const char* path, double offset, int count, void* dst), {
    var xhr = new XMLHttpRequest();
    xhr.open("GET", "/game/" + UTF8ToString(path).split("/").map(encodeURIComponent).join("/"), false);
    xhr.responseType = "arraybuffer";
    xhr.setRequestHeader("Range", "bytes=" + offset + "-" + (offset + count - 1));
    try {
        xhr.send();
    } catch (e) {
        return -1;
    }
    if (xhr.status !== 206 && xhr.status !== 200) {
        return -1;
    }
    var bytes = new Uint8Array(xhr.response);
    var length = Math.min(bytes.length, count);
    HEAPU8.set(bytes.subarray(0, length), dst);
    return length;
});

// One 1 MB block of an immutable game file, from its own URL (/blocks/<path>/<size>-<index>.bin) that the bridge
// marks cacheable for a year, so the browser's disk cache and the CDN keep it across visits. Returns the byte count,
// or -1 (the caller then falls back to a Range request).
EM_JS(int, WebVfsFetchBlock, (const char* path, double size, double index, int count, void* dst), {
    var xhr = new XMLHttpRequest();
    xhr.open("GET", "/blocks/" + UTF8ToString(path).split("/").map(encodeURIComponent).join("/") + "/" + size + "-" + index + ".bin", false);
    xhr.responseType = "arraybuffer";
    try {
        xhr.send();
    } catch (e) {
        return -1;
    }
    if (xhr.status !== 200 || xhr.response.byteLength !== count) {
        return -1;
    }
    HEAPU8.set(new Uint8Array(xhr.response), dst);
    return count;
});

// Whole-file upload / delete in the bridge's user-data overlay. Both return the HTTP status, or -1. The header is what
// keeps other sites from writing here (a cross-site request with it needs a preflight, which the bridge never answers).
// The heap is shared memory, which XMLHttpRequest.send refuses, hence slice() to copy.
EM_JS(int, WebVfsPut, (const char* path, const void* data, int size), {
    if (typeof XMLHttpRequest === "undefined") {
        return -1;
    }
    var xhr = new XMLHttpRequest();
    xhr.open("PUT", "/userdata/" + UTF8ToString(path).split("/").map(encodeURIComponent).join("/"), false);
    xhr.setRequestHeader("X-Wow-Client", "1");
    try {
        xhr.send(HEAPU8.slice(data, data + size));
    } catch (e) {
        return -1;
    }
    return xhr.status;
});

EM_JS(int, WebVfsDelete, (const char* path), {
    if (typeof XMLHttpRequest === "undefined") {
        return -1;
    }
    var xhr = new XMLHttpRequest();
    xhr.open("DELETE", "/userdata/" + UTF8ToString(path).split("/").map(encodeURIComponent).join("/"), false);
    xhr.setRequestHeader("X-Wow-Client", "1");
    try {
        xhr.send();
    } catch (e) {
        return -1;
    }
    return xhr.status;
});

namespace {

const int kRemoteFdBase = 0x40000000;
const int64_t kBlockSize = 1 << 20;
// ponytail: fixed 256 MB block cache; make it a CVar if memory gets tight on big zones.
const size_t kMaxBlocks = 256;

struct RemoteFile {
    std::string m_path;
    int64_t m_size;
};

struct Manifest {
    std::vector<RemoteFile> m_files;
    std::unordered_map<std::string, int> m_fileIndex;
    // directory key -> (child name -> is directory)
    std::unordered_map<std::string, std::map<std::string, bool>> m_dirs;
};

struct RemoteFd {
    int m_file;
    int64_t m_pos;
};

typedef std::shared_ptr<const std::vector<uint8_t>> BlockData;

struct Block {
    uint64_t m_key;
    BlockData m_data;
};

std::once_flag s_manifestOnce;
Manifest* s_manifest;

std::mutex s_fdLock;
std::unordered_map<int, RemoteFd> s_fds;
int s_nextFd = kRemoteFdBase;

// Locally opened files whose path is persisted; both guarded by s_fdLock.
struct WriteFd {
    std::string m_key;
    bool m_dirty;
};

std::unordered_map<int, WriteFd> s_writeFds;
// Persisted files that were deleted (or renamed away) this session, so a manifest copy no longer shows through.
std::unordered_set<std::string> s_deleted;

const int64_t kMaxPersistBytes = 64 << 20;

std::mutex s_cacheLock;
// Blocks fetched over the network so far (FetchCounter).
std::atomic<int32_t> s_fetchCount;
std::list<Block> s_lru;
std::unordered_map<uint64_t, std::list<Block>::iterator> s_blocks;

void AddDir(Manifest& m, const std::string& dir) {
    if (m.m_dirs.count(dir)) {
        return;
    }

    m.m_dirs[dir];

    if (!dir.empty()) {
        size_t slash = dir.rfind('/');
        std::string parent = slash == std::string::npos ? "" : dir.substr(0, slash);
        AddDir(m, parent);
        m.m_dirs[parent][dir.substr(slash + 1)] = true;
    }
}

Manifest* GetManifest() {
    std::call_once(s_manifestOnce, [] {
        char* text = WebVfsFetchManifest();

        if (!text) {
            return;
        }

        // One "size<TAB>path" line per file, path relative to the game root with '/' separators.
        Manifest* m = new Manifest();

        for (char* line = strtok(text, "\n"); line; line = strtok(nullptr, "\n")) {
            char* tab = strchr(line, '\t');

            if (!tab) {
                continue;
            }

            std::string path(tab + 1);

            if (!path.empty() && path.back() == '\r') {
                path.pop_back();
            }

            m->m_fileIndex[path] = static_cast<int>(m->m_files.size());
            m->m_files.push_back({ path, strtoll(line, nullptr, 10) });

            size_t slash = path.rfind('/');
            std::string dir = slash == std::string::npos ? "" : path.substr(0, slash);
            AddDir(*m, dir);
            m->m_dirs[dir][path.substr(slash + 1)] = false;
        }

        free(text);
        s_manifest = m;
    });

    return s_manifest;
}

// Path relative to the root of the file system, '/'-separated, no "." or ".." components.
std::string Key(const char* path) {
    std::string full(path ? path : "");

    if (full.empty() || full[0] != '/') {
        char cwd[1024];
        full = std::string(getcwd(cwd, sizeof(cwd)) ? cwd : "/") + "/" + full;
    }

    std::vector<std::string> parts;
    size_t pos = 0;

    while (pos <= full.size()) {
        size_t end = full.find('/', pos);
        end = end == std::string::npos ? full.size() : end;
        std::string comp = full.substr(pos, end - pos);
        pos = end + 1;

        if (comp == "..") {
            if (!parts.empty()) {
                parts.pop_back();
            }
        } else if (!comp.empty() && comp != ".") {
            parts.push_back(comp);
        }
    }

    std::string key;

    for (size_t i = 0; i < parts.size(); i++) {
        key += (i ? "/" : "") + parts[i];
    }

    return key;
}

bool IsDeleted(const std::string& key) {
    std::lock_guard<std::mutex> guard(s_fdLock);
    return s_deleted.count(key) != 0;
}

int FindFile(const char* path) {
    Manifest* m = GetManifest();

    if (!m) {
        return -1;
    }

    std::string key = Key(path);
    auto it = m->m_fileIndex.find(key);
    return it == m->m_fileIndex.end() || IsDeleted(key) ? -1 : it->second;
}

// Persisted paths: a file below WTF/, Cache/ or Screenshots/ (same list as Tools/Bridge/userdata.mjs; Logs are not
// kept). Only when a bridge answered the manifest request.
bool IsPersisted(const std::string& key) {
    size_t slash = key.find('/');

    if (slash == std::string::npos || !GetManifest()) {
        return false;
    }

    std::string top = key.substr(0, slash);

    for (char& c : top) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }

    return top == "wtf" || top == "cache" || top == "screenshots";
}

void PersistDelete(const std::string& key) {
    {
        std::lock_guard<std::mutex> guard(s_fdLock);
        s_deleted.insert(key);
    }

    int status = WebVfsDelete(key.c_str());

    if (status != 204 && status != -1) {
        printf("WebVfs: persisting delete of %s failed (HTTP %d)\n", key.c_str(), status);
    }
}

// Uploads the local copy of a persisted file.
void PersistUpload(const std::string& key) {
    int fd = ::open(("/" + key).c_str(), O_RDONLY);
    struct stat st;

    if (fd < 0 || ::fstat(fd, &st) != 0 || st.st_size > kMaxPersistBytes) {
        printf("WebVfs: not persisting %s (unreadable or over %d MB)\n", key.c_str(), static_cast<int>(kMaxPersistBytes >> 20));

        if (fd >= 0) {
            ::close(fd);
        }

        return;
    }

    std::vector<uint8_t> data(static_cast<size_t>(st.st_size));
    size_t total = 0;
    ssize_t got;

    while (total < data.size() && (got = ::read(fd, data.data() + total, data.size() - total)) > 0) {
        total += got;
    }

    ::close(fd);

    int status = total == data.size() ? WebVfsPut(key.c_str(), data.data(), static_cast<int>(total)) : -2;

    if (status != 204 && status != -1) {
        printf("WebVfs: persisting %s failed (HTTP %d)\n", key.c_str(), status);
    }
}

void MarkDirty(int fd) {
    std::lock_guard<std::mutex> guard(s_fdLock);
    auto it = s_writeFds.find(fd);

    if (it != s_writeFds.end()) {
        it->second.m_dirty = true;
    }
}

bool IsRemoteDir(const char* path) {
    Manifest* m = GetManifest();
    return m && m->m_dirs.count(Key(path));
}

BlockData GetBlock(int file, int64_t index) {
    uint64_t key = (static_cast<uint64_t>(file) << 40) | static_cast<uint64_t>(index);

    {
        std::lock_guard<std::mutex> guard(s_cacheLock);
        auto it = s_blocks.find(key);

        if (it != s_blocks.end()) {
            s_lru.splice(s_lru.begin(), s_lru, it->second);
            return it->second->m_data;
        }
    }

    // Fetched outside the lock; two threads missing the same block both fetch it, which is harmless.
    const RemoteFile& remote = s_manifest->m_files[file];
    int64_t offset = index * kBlockSize;
    int32_t count = static_cast<int32_t>(std::min(kBlockSize, remote.m_size - offset));
    auto data = std::make_shared<std::vector<uint8_t>>(count > 0 ? count : 0);

    // Data/ only ever holds the shipped game files; everything else may be a player's own (user-data overlay) file.
    bool immutable = remote.m_path.size() > 5 && !strncasecmp(remote.m_path.c_str(), "Data/", 5);

    if (count <= 0) {
        return nullptr;
    }

    s_fetchCount.fetch_add(1, std::memory_order_relaxed);

    if ((!immutable || WebVfsFetchBlock(remote.m_path.c_str(), static_cast<double>(remote.m_size), static_cast<double>(index), count, data->data()) != count)
        && WebVfsFetchRange(remote.m_path.c_str(), static_cast<double>(offset), count, data->data()) != count) {
        return nullptr;
    }

    std::lock_guard<std::mutex> guard(s_cacheLock);

    if (!s_blocks.count(key)) {
        s_lru.push_front({ key, data });
        s_blocks[key] = s_lru.begin();

        if (s_lru.size() > kMaxBlocks) {
            s_blocks.erase(s_lru.back().m_key);
            s_lru.pop_back();
        }
    }

    return data;
}

ssize_t ReadRemote(int file, void* buffer, size_t count, int64_t offset) {
    int64_t size = s_manifest->m_files[file].m_size;
    uint8_t* out = static_cast<uint8_t*>(buffer);
    size_t total = 0;

    while (total < count && offset + static_cast<int64_t>(total) < size) {
        int64_t pos = offset + total;
        BlockData block = GetBlock(file, pos / kBlockSize);

        if (!block) {
            errno = EIO;
            return -1;
        }

        size_t within = static_cast<size_t>(pos % kBlockSize);
        size_t n = std::min(count - total, block->size() - within);
        memcpy(out + total, block->data() + within, n);
        total += n;
    }

    return static_cast<ssize_t>(total);
}

void MakeLocalParents(const char* path) {
    std::string key = Key(path);
    size_t pos = 0;

    while ((pos = key.find('/', pos)) != std::string::npos) {
        ::mkdir(("/" + key.substr(0, pos)).c_str(), 0777);
        pos++;
    }
}

// First write to a manifest file: copy it into the local file system, which then shadows the remote copy.
bool CopyToLocal(int file, const char* path) {
    MakeLocalParents(path);
    int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);

    if (fd < 0) {
        return false;
    }

    std::vector<uint8_t> buffer(kBlockSize);
    int64_t offset = 0;
    ssize_t got;

    while ((got = ReadRemote(file, buffer.data(), buffer.size(), offset)) > 0) {
        ::write(fd, buffer.data(), got);
        offset += got;
    }

    ::close(fd);
    return got == 0;
}

bool GetRemoteFd(int fd, RemoteFd& out) {
    std::lock_guard<std::mutex> guard(s_fdLock);
    auto it = s_fds.find(fd);

    if (it == s_fds.end()) {
        return false;
    }

    out = it->second;
    return true;
}

void FillRemoteStat(int64_t size, bool isDir, struct stat* st) {
    memset(st, 0, sizeof(*st));
    st->st_mode = isDir ? (S_IFDIR | 0555) : (S_IFREG | 0444);
    st->st_size = isDir ? 0 : size;
    st->st_nlink = 1;
}

}

const int32_t* WebVfs::FetchCounter() {
    static_assert(sizeof(s_fetchCount) == sizeof(int32_t) && std::atomic<int32_t>::is_always_lock_free);
    return reinterpret_cast<const int32_t*>(&s_fetchCount);
}

bool WebVfs::IsRemote(int fd) {
    return fd >= kRemoteFdBase;
}

static int OpenInner(const char* path, int flags, int mode) {
    if (::access(path, F_OK) == 0) {
        return ::open(path, flags, mode);
    }

    int file = FindFile(path);

    if (file < 0) {
        if (flags & O_CREAT) {
            MakeLocalParents(path);
        }

        return ::open(path, flags, mode);
    }

    if ((flags & O_CREAT) && (flags & O_EXCL)) {
        errno = EEXIST;
        return -1;
    }

    if ((flags & O_ACCMODE) == O_RDONLY && !(flags & O_TRUNC)) {
        std::lock_guard<std::mutex> guard(s_fdLock);
        int fd = s_nextFd++;
        s_fds[fd] = { file, 0 };
        return fd;
    }

    if (flags & O_TRUNC) {
        MakeLocalParents(path);
    } else if (!CopyToLocal(file, path)) {
        errno = EIO;
        return -1;
    }

    return ::open(path, flags, mode);
}

int WebVfs::open(const char* path, int flags, int mode) {
    // Whether this open creates the file (neither a local nor a manifest copy exists).
    bool created = (flags & O_CREAT) && ::access(path, F_OK) != 0 && FindFile(path) < 0;
    int fd = OpenInner(path, flags, mode);

    if (fd >= 0 && !IsRemote(fd) && (flags & O_ACCMODE) != O_RDONLY) {
        std::string key = Key(path);

        if (IsPersisted(key)) {
            // A created or truncated file counts as changed even if nothing is written to it.
            std::lock_guard<std::mutex> guard(s_fdLock);
            s_writeFds[fd] = { key, created || (flags & O_TRUNC) != 0 };
        }
    }

    return fd;
}

ssize_t WebVfs::write(int fd, const void* buffer, size_t count) {
    ssize_t put = ::write(fd, buffer, count);

    if (put > 0) {
        MarkDirty(fd);
    }

    return put;
}

ssize_t WebVfs::pwrite(int fd, const void* buffer, size_t count, off_t offset) {
    ssize_t put = ::pwrite(fd, buffer, count, offset);

    if (put > 0) {
        MarkDirty(fd);
    }

    return put;
}

int WebVfs::ftruncate(int fd, off_t length) {
    int result = ::ftruncate(fd, length);

    if (result == 0) {
        MarkDirty(fd);
    }

    return result;
}

int WebVfs::unlink(const char* path) {
    std::string key = Key(path);
    bool persisted = IsPersisted(key);
    int result = ::unlink(path);

    // A file that only exists in the manifest (loaded from the user-data overlay) can be deleted too.
    if (result != 0 && persisted && FindFile(path) >= 0) {
        result = 0;
    }

    if (result == 0 && persisted) {
        PersistDelete(key);
    }

    return result;
}

int WebVfs::rename(const char* from, const char* to) {
    int result = ::rename(from, to);

    if (result == 0) {
        std::string fromKey = Key(from);
        std::string toKey = Key(to);
        struct stat st;

        // ponytail: renaming a directory does not move its persisted files; add a recursive walk if the client needs it.
        if (IsPersisted(toKey) && ::stat(to, &st) == 0 && S_ISREG(st.st_mode)) {
            PersistUpload(toKey);
        }

        if (IsPersisted(fromKey) && fromKey != toKey) {
            PersistDelete(fromKey);
        }
    }

    return result;
}

int WebVfs::close(int fd) {
    if (!IsRemote(fd)) {
        WriteFd tracked;
        bool isTracked = false;

        {
            std::lock_guard<std::mutex> guard(s_fdLock);
            auto it = s_writeFds.find(fd);

            if (it != s_writeFds.end()) {
                tracked = it->second;
                isTracked = true;
                s_writeFds.erase(it);
            }
        }

        int result = ::close(fd);

        if (result == 0 && isTracked && tracked.m_dirty) {
            PersistUpload(tracked.m_key);
        }

        return result;
    }

    std::lock_guard<std::mutex> guard(s_fdLock);
    return s_fds.erase(fd) ? 0 : (errno = EBADF, -1);
}

ssize_t WebVfs::read(int fd, void* buffer, size_t count) {
    if (!IsRemote(fd)) {
        return ::read(fd, buffer, count);
    }

    RemoteFd remote;

    if (!GetRemoteFd(fd, remote)) {
        errno = EBADF;
        return -1;
    }

    ssize_t got = ReadRemote(remote.m_file, buffer, count, remote.m_pos);

    if (got > 0) {
        std::lock_guard<std::mutex> guard(s_fdLock);
        s_fds[fd].m_pos += got;
    }

    return got;
}

ssize_t WebVfs::pread(int fd, void* buffer, size_t count, off_t offset) {
    if (!IsRemote(fd)) {
        return ::pread(fd, buffer, count, offset);
    }

    RemoteFd remote;

    if (!GetRemoteFd(fd, remote)) {
        errno = EBADF;
        return -1;
    }

    return ReadRemote(remote.m_file, buffer, count, offset);
}

off_t WebVfs::lseek(int fd, off_t offset, int whence) {
    if (!IsRemote(fd)) {
        return ::lseek(fd, offset, whence);
    }

    std::lock_guard<std::mutex> guard(s_fdLock);
    auto it = s_fds.find(fd);

    if (it == s_fds.end()) {
        errno = EBADF;
        return -1;
    }

    int64_t base = whence == SEEK_SET ? 0 : (whence == SEEK_CUR ? it->second.m_pos : s_manifest->m_files[it->second.m_file].m_size);

    if (base + offset < 0) {
        errno = EINVAL;
        return -1;
    }

    it->second.m_pos = base + offset;
    return it->second.m_pos;
}

int WebVfs::fstat(int fd, struct stat* st) {
    if (!IsRemote(fd)) {
        return ::fstat(fd, st);
    }

    RemoteFd remote;

    if (!GetRemoteFd(fd, remote)) {
        errno = EBADF;
        return -1;
    }

    FillRemoteStat(s_manifest->m_files[remote.m_file].m_size, false, st);
    return 0;
}

int WebVfs::stat(const char* path, struct stat* st) {
    if (::stat(path, st) == 0) {
        return 0;
    }

    int file = FindFile(path);

    if (file >= 0) {
        FillRemoteStat(s_manifest->m_files[file].m_size, false, st);
        return 0;
    }

    if (IsRemoteDir(path)) {
        FillRemoteStat(0, true, st);
        return 0;
    }

    errno = ENOENT;
    return -1;
}

int WebVfs::access(const char* path, int mode) {
    if (::access(path, mode) == 0) {
        return 0;
    }

    if ((FindFile(path) >= 0 || IsRemoteDir(path)) && !(mode & W_OK)) {
        return 0;
    }

    errno = ENOENT;
    return -1;
}

bool WebVfs::ListDirectory(const std::string& path, std::vector<std::pair<std::string, bool>>& entries) {
    std::map<std::string, bool> merged;
    bool found = false;

    if (DIR* handle = ::opendir(path.c_str())) {
        found = true;

        while (dirent* entry = ::readdir(handle)) {
            const char* name = entry->d_name;

            if (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]))) {
                continue;
            }

            bool isDir = entry->d_type == DT_DIR;

            if (entry->d_type != DT_DIR && entry->d_type != DT_REG) {
                struct stat st;
                isDir = ::stat((path + "/" + name).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
            }

            merged[name] = isDir;
        }

        ::closedir(handle);
    }

    if (Manifest* m = GetManifest()) {
        auto it = m->m_dirs.find(Key(path.c_str()));

        if (it != m->m_dirs.end()) {
            found = true;
            std::string dir = Key(path.c_str());

            for (const auto& entry : it->second) {
                if (entry.second || !IsDeleted((dir.empty() ? "" : dir + "/") + entry.first)) {
                    merged.insert(entry);
                }
            }
        }
    }

    entries.assign(merged.begin(), merged.end());
    return found;
}

#endif
