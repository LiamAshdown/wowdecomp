#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cstdint>
#include <windows.h>
#include "Engine/Source/BlizzardCore/Source/System/File/File.h"
#include "Engine/Source/BlizzardCore/Source/System/String/Path.h"
#include "Engine/Source/BlizzardCore/Source/System/String/String.h"
#include "Engine/Source/Os/OsFile.h"

// Win32 constants used by the Os file API (OsFile.h already defines the generic/create/normal/directory ones).
#ifndef OS_FILE_ATTRIBUTE_READONLY
#define OS_FILE_ATTRIBUTE_READONLY 0x1
#endif
#ifndef OS_FILE_ATTRIBUTE_HIDDEN
#define OS_FILE_ATTRIBUTE_HIDDEN 0x2
#endif
#ifndef OS_FILE_SHARE_READ
#define OS_FILE_SHARE_READ 0x1
#endif
#ifndef OS_FILE_SHARE_WRITE
#define OS_FILE_SHARE_WRITE 0x2
#endif
#ifndef OS_CREATE_NEW
#define OS_CREATE_NEW 1
#endif
#ifndef OS_OPEN_ALWAYS
#define OS_OPEN_ALWAYS 4
#endif
#ifndef OS_TRUNCATE_EXISTING
#define OS_TRUNCATE_EXISTING 5
#endif

// Stands in for SetLastError(ERROR_INVALID_PARAMETER).
static void WebSetLastError() {
    Blizzard::File::SetLastError(8);
}

bool OsCloseFile(HOSFILE file) {
    return Blizzard::File::Close(reinterpret_cast<Blizzard::File::StreamRecord*>(file));
}

int32_t OsFileExists(const char* fileName) {
    return Blizzard::File::IsFile(fileName);
}

int32_t OsDirectoryExists(const char* pathName) {
    return Blizzard::File::IsDirectory(pathName);
}

int32_t OsReadFile(HOSFILE file, void* buffer, uint32_t bytesToRead, uint32_t* bytesRead) {
    if (buffer && bytesRead) {
        *bytesRead = bytesToRead;
        return Blizzard::File::Read(reinterpret_cast<Blizzard::File::StreamRecord*>(file), buffer, reinterpret_cast<int32_t*>(bytesRead));
    }

    WebSetLastError();
    return 0;
}

int32_t OsWriteFile(HOSFILE file, const void* buffer, uint32_t bytesToWrite, uint32_t* bytesWritten) {
    if (buffer && bytesWritten) {
        *bytesWritten = bytesToWrite;
        return Blizzard::File::Write(reinterpret_cast<Blizzard::File::StreamRecord*>(file), buffer, reinterpret_cast<int32_t*>(bytesWritten));
    }

    WebSetLastError();
    return 0;
}

uint64_t OsGetFileSize(HOSFILE file) {
    Blizzard::File::FileInfo* info = Blizzard::File::GetFileInfo(reinterpret_cast<Blizzard::File::StreamRecord*>(file));
    return info ? info->m_size : 0xFFFFFFFF;
}

uint32_t OsGetFileAttributes(const char* fileName) {
    if (!fileName) {
        WebSetLastError();
        return 0;
    }

    Blizzard::File::FileInfo info;
    if (!Blizzard::File::GetFileInfo(fileName, &info)) {
        return 0xFFFFFFFF;
    }

    return info.m_attributes;
}

int32_t OsSetFileAttributes(const char* fileName, uint32_t attributes) {
    if (!fileName) {
        WebSetLastError();
        return 0;
    }

    return Blizzard::File::SetAttributes(fileName, attributes);
}

int32_t OsMoveFile(const char* existingFileName, const char* newFileName) {
    if (existingFileName && newFileName) {
        return Blizzard::File::Move(existingFileName, newFileName);
    }

    WebSetLastError();
    return 0;
}

int32_t OsCopyFile(const char* existingFileName, const char* newFileName, int32_t failIfExists) {
    if (existingFileName && newFileName) {
        return Blizzard::File::Copy(existingFileName, newFileName, failIfExists == 0);
    }

    WebSetLastError();
    return 0;
}

int32_t OsDeleteFile(const char* fileName) {
    if (!fileName) {
        WebSetLastError();
        return 0;
    }

    return Blizzard::File::Delete(fileName);
}

int32_t OsCreateDirectory(const char* pathName, int32_t recursive) {
    if (!pathName) {
        WebSetLastError();
        return 0;
    }

    return Blizzard::File::CreateDirectory(pathName, recursive != 0);
}

int32_t OsRemoveDirectory(const char* pathName) {
    if (!pathName) {
        WebSetLastError();
        return 0;
    }

    return Blizzard::File::RemoveDirectory(pathName);
}

int32_t OsRemoveDirectoryRecurse(const char* pathName, uint32_t flags) {
    if (!pathName) {
        WebSetLastError();
        return 0;
    }

    return Blizzard::File::RemoveDirectoryAndContents(pathName, (flags & 0x1) != 0);
}

bool OsSetCurrentDirectory(const char* path) {
    if (path) {
        return Blizzard::File::SetWorkingDirectory(path) != 0;
    }

    WebSetLastError();

    return false;
}

int32_t OsGetCurrentDirectory(uint32_t bufferSize, char* buffer) {
    if (!buffer) {
        WebSetLastError();
        return 0;
    }

    return Blizzard::File::GetWorkingDirectory(buffer, bufferSize);
}

bool OsFileList_Internal::Callback(const Blizzard::File::ProcessDirParms& parms) {
    OS_FILE_DATA data;
    Blizzard::String::Copy(data.fileName, parms.m_filename, sizeof(data.fileName));

    char path[260];
    Blizzard::String::JoinPath(path, sizeof(path), parms.m_pathRoot, parms.m_filename);

    OsFileList_Internal* list = static_cast<OsFileList_Internal*>(parms.m_cookie);

    if (!Blizzard::File::SimpleGlob(parms.m_filename, list->m_pattern)) {
        return true;
    }

    Blizzard::File::FileInfo info;
    if (!Blizzard::File::GetFileInfo(path, &info)) {
        return true;
    }

    data.size = static_cast<uint32_t>(info.m_size);
    data.flags = 0;

    if (info.m_attributes & 0x40) {
        data.flags = OS_FILE_ATTRIBUTE_DIRECTORY;
    }

    if (info.m_attributes & 0x1) {
        data.flags |= OS_FILE_ATTRIBUTE_READONLY;
    }

    if (info.m_attributes & 0x2) {
        data.flags |= OS_FILE_ATTRIBUTE_HIDDEN;
    }

    if (!list->m_includeHidden && (info.m_attributes & 0x2)) {
        return true;
    }

    return !list->m_callback(data, list->m_param);
}

uint32_t GetFileMode(uint32_t desiredAccess, uint32_t shareMode, uint32_t createDisposition) {
    uint32_t mode = 0;

    if (desiredAccess & OS_GENERIC_READ) {
        mode = 0x1;
    }

    if (desiredAccess & OS_GENERIC_WRITE) {
        mode |= 0x2;
    }

    if (shareMode & OS_FILE_SHARE_READ) {
        mode |= 0x4;
    }

    if (shareMode & OS_FILE_SHARE_WRITE) {
        mode |= 0x8;
    }

    switch (createDisposition) {
        case OS_CREATE_NEW:
            mode |= 0xC00;
            break;

        case OS_CREATE_ALWAYS:
            mode |= 0x400;
            break;

        case OS_OPEN_EXISTING:
            mode |= 0x1000;
            break;

        case OS_OPEN_ALWAYS:
            mode |= 0x200;
            break;

        case OS_TRUNCATE_EXISTING:
            mode |= 0x100;
            break;
    }

    return mode;
}

char* OsFileGetDownloadFolder() {
    // STATIC: 0x00B38A88
    static char s_downloadfolder[260];

    if (!s_downloadfolder[0]) {
        Blizzard::File::GetWorkingDirectory(s_downloadfolder, sizeof(s_downloadfolder));
    }

    return s_downloadfolder;
}

HOSFILE OsCreateFile(const char* fileName, uint32_t desiredAccess, uint32_t shareMode, uint32_t createDisposition, uint32_t flagsAndAttributes, uint32_t extendedFileType) {
    if (!fileName || !desiredAccess || createDisposition < OS_CREATE_NEW || createDisposition > OS_TRUNCATE_EXISTING) {
        return OS_INVALID_FILE;
    }

    Blizzard::File::StreamRecord* file;
    if (!Blizzard::File::Open(fileName, GetFileMode(desiredAccess, shareMode, createDisposition), file)) {
        return OS_INVALID_FILE;
    }

    if (!(flagsAndAttributes & OS_FILE_ATTRIBUTE_NORMAL)) {
        OsSetFileAttributes(fileName, flagsAndAttributes);
    }

    return reinterpret_cast<HOSFILE>(file);
}

int32_t OsFileList(const char* dir, const char* pattern, OsFileListCallback callback, void* param, int32_t includeHidden) {
    char relative[260];
    Blizzard::String::Copy(relative, dir, sizeof(relative));

    char* separator = Blizzard::String::FindPathSeparatorReverse(relative, 0x7FFFFFFF);
    if (separator) {
        *separator = '\0';
    }

    char path[260];
    if (!Blizzard::File::MakeAbsolutePath(relative, path, sizeof(path), false)) {
        return 0;
    }

    OsFileList_Internal list;
    list.m_callback = callback;
    list.m_param = param;
    list.m_includeHidden = includeHidden;
    list.m_pattern = pattern;

    return Blizzard::File::ProcessDirFast(path, &list, OsFileList_Internal::Callback, false);
}

#endif
