#if defined(WOWDECOMP_PLATFORM_WEB)

#include "Engine/Source/Os/OsURLDownload.h"
#include <cstdlib>
#include <cstring>
#include <emscripten.h>

EM_JS_DEPS(OsURLDownload, "$stringToNewUTF8,$UTF8ToString");

// Synchronous XHR from the game pthread (allowed in workers). The response text (malloc'd), or null.
EM_JS(char*, OsWebFetchText, (const char* url), {
    var xhr = new XMLHttpRequest();
    xhr.open("GET", UTF8ToString(url), false);
    try {
        xhr.send();
    } catch (e) {
        return 0;
    }
    return xhr.status === 200 ? stringToNewUTF8(xhr.responseText) : 0;
});

// Only paths on the page's own server: the Blizzard URLs in the glue strings cannot be reached from the page (no
// CORS), so those fail as before. The server alert points here (WebAccount.cpp). The callback runs before this returns;
// callers only set flags in it.
bool OsURLDownload(const char* url, OsURLDownloadCallback callback, void* param) {
    if (!url || url[0] != '/' || url[1] == '/') {
        return false;
    }

    char* text = OsWebFetchText(url);

    if (!text) {
        callback(param, nullptr, 0, true, HTTPError::NotFound);
        return true;
    }

    uint32_t size = static_cast<uint32_t>(strlen(text));
    callback(param, text, size + 1, false, HTTPError::NoError);
    callback(param, nullptr, 0, true, HTTPError::NoError);
    free(text);
    return true;
}

#endif
