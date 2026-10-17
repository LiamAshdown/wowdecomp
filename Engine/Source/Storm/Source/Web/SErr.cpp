#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <vector>
#include "Engine/Source/Storm/Source/SOption.h"
#include "Engine/Source/Storm/H/storm.h"

// Win64 shows a message box, writes Errors/*.txt logs and a minidump. In a browser the console is the log:
// every error goes to stderr, and a non-recoverable one aborts the program (which prints a stack to the console).

typedef int32_t (STORMAPI* ERRORHANDLER)(uint32_t, const char*, const char*, int32_t, const char*);

static pthread_mutex_t s_critsect = PTHREAD_MUTEX_INITIALIZER;

static std::vector<ERRORHANDLER> s_handlers;

static int32_t s_displaying;

static uint32_t s_lasterror;

static const char* s_appFatFilename;

static int32_t s_appFatLinenumber;

static uint32_t s_appFatThreadId;

static char s_logtitle[128];

static int32_t (STORMAPI* s_logCallback)(char*, uint32_t);

static uint32_t s_noMiniDumps;

void SErrInitialize() {
    g_opt.serrleaksilentwarning = 1;
    SRegLoadValue("Internal", "No Minidumps", 0, &s_noMiniDumps);
}

uint32_t STORMAPI SErrGetLastError() {
    return s_lasterror;
}

int32_t STORMAPI SErrIsDisplayingError() {
    return s_displaying;
}

void STORMAPI SErrPrepareAppFatal(const char* filename, int32_t linenumber) {
    pthread_mutex_lock(&s_critsect);
    s_appFatFilename = filename;
    s_appFatLinenumber = linenumber;
    s_appFatThreadId = SGetCurrentThreadId();
    pthread_mutex_unlock(&s_critsect);
}

void STORMAPI SErrSetLastError(uint32_t errorcode) {
    s_lasterror = errorcode;
}

void STORMAPI SErrSetLogTitleString(const char* title) {
    pthread_mutex_lock(&s_critsect);
    SStrCopy(s_logtitle, title, sizeof(s_logtitle));
    pthread_mutex_unlock(&s_critsect);
}

void STORMAPI SErrSetLogCallback(int32_t (STORMAPI* callback)(char*, uint32_t)) {
    pthread_mutex_lock(&s_critsect);
    s_logCallback = callback;
    pthread_mutex_unlock(&s_critsect);
}

// No error log files are written, so there is never a last one.
int32_t STORMAPI SErrGetLogLastPath(char* buf, uint32_t size) {
    return 0;
}

int32_t STORMAPI SErrGetErrorStr(uint32_t errorcode, char* buffer, uint32_t bufferchars) {
    if (!buffer || !bufferchars) {
        SErrSetLastError(STORM_ERROR_INVALID_PARAMETER);
        return 0;
    }

    if (errorcode == 108) {
        SStrPrintf(buffer, bufferchars, "%s", "Invalid or corrupt archive.\n");
    } else {
        // The Windows text comes from FormatMessage and Storm's string table; neither exists here.
        SStrPrintf(buffer, bufferchars, "Error %u (0x%08x)\n", errorcode, errorcode);
    }

    return 1;
}

void STORMAPI SErrRegisterHandler(int32_t (STORMAPI* handler)(uint32_t, const char*, const char*, int32_t, const char*)) {
    pthread_mutex_lock(&s_critsect);
    s_handlers.push_back(handler);
    pthread_mutex_unlock(&s_critsect);
}

int32_t STORMAPI SErrDestroy() {
    pthread_mutex_lock(&s_critsect);
    s_handlers.clear();
    pthread_mutex_unlock(&s_critsect);

    return 1;
}

int32_t STORMAPI SErrDisplayError(uint32_t errorcode, const char* filename, int32_t linenumber, const char* description, int32_t recoverable, uint32_t exitcode, uint32_t unknown) {
    static int32_t lastlinenumber;
    static const char* lastfilename;

    if (g_opt.unk_0028) {
        if (filename == lastfilename && linenumber == lastlinenumber) {
            return 1;
        }

        lastfilename = filename;
        lastlinenumber = linenumber;
    }

    if (s_displaying) {
        return 0;
    }

    s_displaying = 1;

    char errorstr[256];
    errorstr[0] = 0;
    SErrGetErrorStr(errorcode, errorstr, sizeof(errorstr));

    fprintf(stderr, "%s", "Storm error: ");

    if (filename && *filename) {
        if (linenumber > 0) {
            fprintf(stderr, "%s(%d) : ", filename, linenumber);
        } else {
            fprintf(stderr, "%s : ", filename);
        }
    }

    fprintf(stderr, "error %u: %s %s\n", errorcode, errorstr, description ? description : "");

    // Handlers run unlocked so they may register further handlers or report errors themselves.
    pthread_mutex_lock(&s_critsect);
    std::vector<ERRORHANDLER> handlers = s_handlers;
    pthread_mutex_unlock(&s_critsect);

    for (size_t i = 0; i < handlers.size(); i++) {
        if (!handlers[i](errorcode, errorstr, filename, linenumber, description ? description : "")) {
            break;
        }
    }

    s_displaying = 0;

    if (g_opt.unk_0028 || recoverable) {
        return 1;
    }

    fflush(stderr);
    abort();
}

void STORMAPI SErrReportNamedResourceLeak(const char* handlename, const char* resourcename) {
    static int32_t checked;
    static uint32_t debugmode;

    if (!checked) {
        checked = 1;
        SRegLoadValue("Internal", "Debug Memory", 0, &debugmode);
    }

    if (!debugmode) {
        return;
    }

    char errormessage[256];

    if (resourcename && *resourcename) {
        SStrPrintf(errormessage, sizeof(errormessage), "%s (%s)", handlename, resourcename);
    } else {
        SStrPrintf(errormessage, sizeof(errormessage), "%s", handlename);
    }

    if (g_opt.serrleaksilentwarning) {
        fprintf(stderr, "Storm Error : handle never released -- %s\n", errormessage);
    } else {
        SErrDisplayError(STORM_ERROR_RESOURCES_NOT_RELEASED, errormessage, SLOG_HANDLE, nullptr, 1, 1, 0x11111111);
    }
}

void SErrDisplayAppFatalCustomV(uint32_t errorcode, const char* format, va_list args) {
    const char* filename = nullptr;
    int32_t linenumber = 0;

    pthread_mutex_lock(&s_critsect);

    if (s_appFatThreadId == SGetCurrentThreadId()) {
        filename = s_appFatFilename;
        linenumber = s_appFatLinenumber;
        s_appFatFilename = nullptr;
        s_appFatLinenumber = 0;
        s_appFatThreadId = 0;
    }

    pthread_mutex_unlock(&s_critsect);

    char buffer[2048];
    vsnprintf(buffer, sizeof(buffer) - 1, format, args);
    buffer[sizeof(buffer) - 1] = 0;
    SErrDisplayError(errorcode, filename, linenumber, buffer, 0, 1, 0x11111111);

    // SErrDisplayError aborts for a non-recoverable error; this covers the case where it returned early.
    fflush(stderr);
    abort();
}

void SErrDisplayAppFatalCustom(uint32_t errorcode, const char* format, ...) {
    va_list args;
    va_start(args, format);
    SErrDisplayAppFatalCustomV(errorcode, format, args);
}

void SErrDisplayAppFatal(const char* format, ...) {
    va_list args;
    va_start(args, format);
    SErrDisplayAppFatalCustomV(STORM_ERROR_FATAL_CONDITION, format, args);
}

int32_t SErrDisplayErrorFmt(uint32_t errorcode, const char* filename, int32_t linenumber, int32_t recoverable, uint32_t exitcode, const char* format, ...) {
    char buffer[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer) - 1, format, args);
    va_end(args);
    buffer[sizeof(buffer) - 1] = 0;

    return SErrDisplayError(errorcode, filename, linenumber, buffer, recoverable, exitcode, 0x11111111);
}

// There is no unhandled-exception filter in wasm; traps already reach the console.
void STORMAPI SErrCatchUnhandledExceptions() {
}

void SErrDisplayAppFatalMessage(const char* message, const char* filename, int32_t linenumber) {
    SErrPrepareAppFatal(filename, linenumber);
    SErrDisplayAppFatal("%s", message);
    abort();
}

#endif
