#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "Engine/Source/Storm/Source/SCmd.h"
#include "Engine/Source/Storm/H/stpl.h"
#include "Engine/Source/Storm/H/storm.h"

#ifndef ERROR_OPEN_FAILED
#define ERROR_OPEN_FAILED 110
#endif

static const char* const s_errorstr[3] = {
    "Invalid argument: %s",
    "The syntax of the command is incorrect.",
    "Unable to open response file: %s",
};

// The Windows version first looks the text up in the executable's string table; there is none here.
void GenerateError(void (STORMAPI* errorcallback)(_CMDERROR*), uint32_t errorcode, const char* itemstring) {
    uint32_t index;

    switch (errorcode) {
        case STORM_ERROR_BAD_ARGUMENT:
            index = 0;
            break;
        case STORM_ERROR_NOT_ENOUGH_ARGUMENTS:
            index = 1;
            break;
        case ERROR_OPEN_FAILED:
            index = 2;
            break;
        default:
            return;
    }

    char errorstr[256];

    if (strstr(s_errorstr[index], "%s")) {
        snprintf(errorstr, sizeof(errorstr), s_errorstr[index], itemstring);
    } else {
        SStrCopy(errorstr, s_errorstr[index], sizeof(errorstr));
    }

    if (errorstr[0]) {
        SStrPack(errorstr, "\n", sizeof(errorstr));
    }

    SErrSetLastError(errorcode);

    _CMDERROR data;
    data.errorcode = errorcode;
    data.itemstr = itemstring;
    data.errorstr = errorstr;
    errorcallback(&data);
}

int32_t ProcessFile(const char* filename, _PROCESSING* processing, CMDDEF** nextarg, int32_t (STORMAPI* extracallback)(const char*), void (STORMAPI* errorcallback)(_CMDERROR*)) {
    FILE* file = fopen(filename, "rb");

    if (!file) {
        if (errorcallback) {
            GenerateError(errorcallback, ERROR_OPEN_FAILED, filename);
        }

        return 0;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char* data = static_cast<char*>(SMemAlloc(size + 1, __FILE__, __LINE__, 0));
    size_t bytesread = fread(data, 1, size, file);
    fclose(file);
    data[bytesread] = 0;

    const char* curr = data;
    int32_t result = ProcessString(&curr, processing, nextarg, extracallback, errorcallback);
    SMemFree(data, __FILE__, __LINE__, 0);

    return result;
}

// A browser page has no command line; the program name stands in so that SCmdProcess has something to skip.
int32_t STORMAPI SCmdProcessCommandLine(int32_t (STORMAPI* extracallback)(const char*), void (STORMAPI* errorcallback)(_CMDERROR*)) {
    return SCmdProcess("Wow.exe", 1, extracallback, errorcallback);
}

#endif
