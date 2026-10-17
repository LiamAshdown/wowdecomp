#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include "Engine/Source/Storm/Source/SLog.h"
#include "Engine/Source/Storm/Source/SOption.h"
#include "Engine/Source/Storm/H/storm.h"

// _LOG::file is a FILE* here (null when no file is open). Files live in the virtual file system.

static int32_t s_logsysteminit;

static uintptr_t s_sequence;

static _LOG* s_loghead[4];

static CRITICAL_SECTION s_critsect[4];

static void FlushLog(_LOG* logptr) {
    if (logptr->bufferused) {
        fwrite(logptr->buffer, 1, logptr->bufferused, static_cast<FILE*>(logptr->file));
        fflush(static_cast<FILE*>(logptr->file));
        logptr->bufferused = 0;
        logptr->pendpoint = 0;
    }
}

static _LOG* LockLog(HSLOG log, HLOCKEDLOG* lockedhandle, int32_t createifnecessary) {
    if (!log) {
        *lockedhandle = reinterpret_cast<HLOCKEDLOG>(-1);
        return nullptr;
    }

    uintptr_t index = reinterpret_cast<uintptr_t>(log) & 3;
    EnterCriticalSection(&s_critsect[index]);
    *lockedhandle = reinterpret_cast<HLOCKEDLOG>(index);

    _LOG** prevptr = &s_loghead[index];

    for (_LOG* logptr = s_loghead[index]; logptr; logptr = logptr->next) {
        if (logptr->log == log) {
            return logptr;
        }

        prevptr = &logptr->next;
    }

    if (!createifnecessary) {
        LeaveCriticalSection(&s_critsect[index]);
        *lockedhandle = reinterpret_cast<HLOCKEDLOG>(-1);
        return nullptr;
    }

    _LOG* logptr = static_cast<_LOG*>(malloc(sizeof(_LOG)));
    *prevptr = logptr;

    if (!logptr) {
        LeaveCriticalSection(&s_critsect[index]);
        *lockedhandle = reinterpret_cast<HLOCKEDLOG>(-1);
        return nullptr;
    }

    logptr->log = log;
    logptr->next = nullptr;
    logptr->filename[0] = 0;
    logptr->file = nullptr;
    logptr->bufferused = 0;
    logptr->pendpoint = 0;

    return logptr;
}

static void OutputIndent(_LOG* logptr) {
    int32_t indent = logptr->indent;

    if (indent > 0) {
        if (indent >= 128) {
            indent = 128;
        }

        memset(&logptr->buffer[logptr->bufferused], ' ', indent);
        logptr->buffer[logptr->bufferused + indent] = 0;
        logptr->bufferused += indent;
    }
}

static void OutputReturn(_LOG* logptr) {
    strcpy(&logptr->buffer[logptr->bufferused], "\r\n");
    logptr->bufferused += 2;
}

static void OutputTime(_LOG* logptr, int32_t show) {
    static uint32_t lasttime;
    static char timestr[64];
    static uint32_t timestrlen;

    if (!logptr->timeStamp) {
        return;
    }

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    uint32_t time = static_cast<uint32_t>(now.tv_sec * 1000 + now.tv_nsec / 1000000);

    if (time != lasttime) {
        lasttime = time;

        time_t seconds = now.tv_sec;
        struct tm local;
        localtime_r(&seconds, &local);
        snprintf(timestr, sizeof(timestr), "%u/%u %02u:%02u:%02u.%03u  ", local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec, static_cast<uint32_t>(now.tv_nsec / 1000000));
        timestrlen = SStrLen(timestr);
    }

    if (show) {
        memcpy(&logptr->buffer[logptr->bufferused], timestr, timestrlen + 1);
    } else {
        memset(&logptr->buffer[logptr->bufferused], ' ', timestrlen);
        logptr->buffer[logptr->bufferused + timestrlen] = 0;
    }

    logptr->bufferused += timestrlen;
}

static void UnlockDeleteLog(_LOG* logptr, HLOCKEDLOG lockedhandle) {
    uintptr_t index = reinterpret_cast<uintptr_t>(lockedhandle);
    _LOG** prevptr = &s_loghead[index];

    for (_LOG* curr = s_loghead[index]; curr; curr = curr->next) {
        if (curr == logptr) {
            *prevptr = curr->next;
            free(curr);
            break;
        }

        prevptr = &curr->next;
    }

    LeaveCriticalSection(&s_critsect[index]);
}

static void UnlockLog(HLOCKEDLOG lockedhandle) {
    LeaveCriticalSection(&s_critsect[reinterpret_cast<uintptr_t>(lockedhandle)]);
}

void STORMAPI SLogClose(HSLOG log) {
    if (!s_logsysteminit) {
        return;
    }

    HLOCKEDLOG lockedhandle;
    _LOG* logptr = LockLog(log, &lockedhandle, 0);

    if (!logptr) {
        return;
    }

    if (logptr->file) {
        FlushLog(logptr);
        fclose(static_cast<FILE*>(logptr->file));
    }

    UnlockDeleteLog(logptr, lockedhandle);
}

void STORMAPI SLogFlush(HSLOG log) {
    HLOCKEDLOG lockedhandle;
    _LOG* logptr = LockLog(log, &lockedhandle, 0);

    if (logptr) {
        if (logptr->file) {
            FlushLog(logptr);
        }

        UnlockLog(lockedhandle);
    }
}

void STORMAPI SLogFlushAll() {
    for (int32_t i = 0; i < 4; i++) {
        EnterCriticalSection(&s_critsect[i]);

        for (_LOG* logptr = s_loghead[i]; logptr; logptr = logptr->next) {
            if (logptr->file) {
                FlushLog(logptr);
            }
        }

        LeaveCriticalSection(&s_critsect[i]);
    }
}

void STORMAPI SLogInitialize() {
    if (s_logsysteminit) {
        return;
    }

    for (int32_t i = 0; i < 4; i++) {
        InitializeCriticalSection(&s_critsect[i]);
    }

    s_logsysteminit = 1;
}

// Storm paths use backslashes; the virtual file system wants forward slashes.
static void NormalizePath(char* path) {
    for (char* c = path; *c; c++) {
        if (*c == '\\') {
            *c = '/';
        }
    }
}

static void CreateFileDirectory(char* path) {
    for (char* c = strchr(path + 1, '/'); c; c = strchr(c + 1, '/')) {
        *c = 0;
        mkdir(path, 0777);
        *c = '/';
    }
}

static int32_t OpenLogFile(const char* filename, void** file, uint32_t flags) {
    *file = nullptr;

    if (!filename || !*filename) {
        return 0;
    }

    char path[MAX_PATH];
    SStrCopy(path, filename, sizeof(path));
    NormalizePath(path);
    CreateFileDirectory(path);

    *file = fopen(path, (flags & SLOG_FLAG_APPEND) ? "ab" : "wb");

    return *file != nullptr;
}

int32_t STORMAPI SLogCreate(const char* filename, uint32_t flags, HSLOG* log) {
    if (!filename || !*filename || !log) {
        SErrSetLastError(STORM_ERROR_INVALID_PARAMETER);
        return 0;
    }

    *log = nullptr;

    if (flags & SLOG_FLAG_NOFILE) {
        filename = "";
        flags &= ~SLOG_FLAG_OPENFILE;
    }

    void* file = nullptr;

    if ((flags & SLOG_FLAG_OPENFILE) && !OpenLogFile(filename, &file, flags)) {
        return 0;
    }

    *log = reinterpret_cast<HSLOG>(++s_sequence);

    HLOCKEDLOG lockedhandle;
    _LOG* logptr = LockLog(*log, &lockedhandle, 1);

    if (!logptr) {
        *log = nullptr;
        return 0;
    }

    logptr->file = file;
    SStrCopy(logptr->filename, filename, sizeof(logptr->filename));
    logptr->flags = flags;
    logptr->timeStamp = 1;
    logptr->indent = 0;
    UnlockLog(lockedhandle);

    return 1;
}

void STORMAPI SLogDestroy() {
    SLogFlushAll();

    for (int32_t i = 0; i < 4; i++) {
        EnterCriticalSection(&s_critsect[i]);

        while (s_loghead[i]) {
            char filename[MAX_PATH];
            SStrCopy(filename, s_loghead[i]->filename, STORM_MAX_STR);
            SLogClose(s_loghead[i]->log);
            SErrReportNamedResourceLeak("HSLOG", filename);
        }

        LeaveCriticalSection(&s_critsect[i]);
    }

    s_logsysteminit = 0;

    for (int32_t i = 0; i < 4; i++) {
        DeleteCriticalSection(&s_critsect[i]);
    }
}

static int32_t PrepareLog(_LOG* logptr) {
    if (logptr->file) {
        return 1;
    }

    if (OpenLogFile(logptr->filename, &logptr->file, logptr->flags)) {
        return 1;
    }

    logptr->filename[0] = 0;
    return 0;
}

void STORMAPI SLogVWrite(HSLOG log, const char* format, va_list arglist) {
    HLOCKEDLOG lockedhandle;
    _LOG* logptr = LockLog(log, &lockedhandle, 0);

    if (!logptr) {
        return;
    }

    if (PrepareLog(logptr)) {
        OutputTime(logptr, 1);
        OutputIndent(logptr);
        SStrVPrintf(&logptr->buffer[logptr->bufferused], sizeof(logptr->buffer) - logptr->bufferused, format, arglist);
        logptr->bufferused += SStrLen(&logptr->buffer[logptr->bufferused]);
        OutputReturn(logptr);

        if (g_opt.echotooutputdebugstring) {
            fputs(&logptr->buffer[logptr->pendpoint], stderr);
        }

        logptr->pendpoint = logptr->bufferused;

        if (logptr->bufferused >= 0xC000) {
            FlushLog(logptr);
        }
    }

    UnlockLog(lockedhandle);
}

void SLogWrite(HSLOG log, const char* format, ...) {
    va_list arglist;
    va_start(arglist, format);
    SLogVWrite(log, format, arglist);
    va_end(arglist);
}

#endif
