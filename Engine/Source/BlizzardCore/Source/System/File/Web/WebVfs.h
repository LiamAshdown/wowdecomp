#ifndef ENGINE_SOURCE_BLIZZARDCORE_SOURCE_SYSTEM_FILE_WEB_WEBVFS_H
#define ENGINE_SOURCE_BLIZZARDCORE_SOURCE_SYSTEM_FILE_WEB_WEBVFS_H

#if defined(WOWDECOMP_PLATFORM_WEB)

#include <string>
#include <utility>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>

// POSIX-shaped file layer for the web build. In a browser the read-only game files (Data\, Interface\, WTF\...) are
// listed in a manifest served by the bridge (Tools/Bridge) and read with synchronous HTTP Range requests through a
// bounded block cache. Everything else, and every write, goes to Emscripten's own file system. Without a manifest
// (e.g. under Node with NODERAWFS) every call is a plain passthrough.
namespace WebVfs {

int open(const char* path, int flags, int mode);
int close(int fd);
ssize_t read(int fd, void* buffer, size_t count);
ssize_t pread(int fd, void* buffer, size_t count, off_t offset);
off_t lseek(int fd, off_t offset, int whence);
int fstat(int fd, struct stat* st);
int stat(const char* path, struct stat* st);
int access(const char* path, int mode);
bool IsRemote(int fd);

// Writes go to the local (in-memory) file system. Files under WTF/, Cache/ and Screenshots/ that were written are also
// uploaded to the bridge (PUT /userdata/...) when they are closed, and unlink/rename are mirrored with DELETE, so they
// survive a page reload (the bridge lists them in the manifest on the next load).
ssize_t write(int fd, const void* buffer, size_t count);
ssize_t pwrite(int fd, const void* buffer, size_t count, off_t offset);
int ftruncate(int fd, off_t length);
int unlink(const char* path);
int rename(const char* from, const char* to);

// Entries of a directory (name, isDirectory), merging local and manifest entries. False if the directory exists in
// neither.
bool ListDirectory(const std::string& path, std::vector<std::pair<std::string, bool>>& entries);

}

#endif

#endif
