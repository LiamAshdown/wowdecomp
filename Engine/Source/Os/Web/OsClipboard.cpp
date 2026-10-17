#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cstdint>
#include <cstring>
#include <string>
#include <pthread.h>
#include "Engine/Source/Os/OsClipboard.h"
#include "Engine/Source/Storm/H/storm.h"

// In-memory clipboard (UTF-8). The DOM layer can fill it from paste events later.
static pthread_mutex_t s_clipboardLock = PTHREAD_MUTEX_INITIALIZER;
static std::string s_clipboard;

char* IOsClipboardGetString(void* window) {
    pthread_mutex_lock(&s_clipboardLock);

    char* string = nullptr;

    if (!s_clipboard.empty()) {
        string = static_cast<char*>(SMemAlloc(s_clipboard.size() + 1, __FILE__, __LINE__, 0));
        memcpy(string, s_clipboard.c_str(), s_clipboard.size() + 1);
    }

    pthread_mutex_unlock(&s_clipboardLock);

    return string;
}

void OsClipboardFreeString(char* string) {
    if (string) {
        SMemFree(string, __FILE__, __LINE__, 0);
    }
}

int32_t IOsClipboardPutString(const char* string, void* window) {
    pthread_mutex_lock(&s_clipboardLock);
    s_clipboard = string ? string : "";
    pthread_mutex_unlock(&s_clipboardLock);

    return 1;
}

#endif
