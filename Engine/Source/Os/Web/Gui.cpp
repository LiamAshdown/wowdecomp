#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cstdint>
#include <cstdio>
#include "Engine/Source/Os/Gui.h"

static void* s_GxDevWindow;

void OsGuiSetWindowTitle(void* window, const char* title) {
    // ponytail: log only, set document.title on the main thread when the canvas host exists.
    fprintf(stderr, "[OsGui] window title: %s\n", title ? title : "");
}

void* OsGuiGetWindow(int32_t type) {
    return type == 0 ? s_GxDevWindow : nullptr;
}

void OsGuiSetGxWindow(void* window) {
    s_GxDevWindow = window;
}

int32_t OsGuiMessageBox(void* parentWindow, int32_t style, const char* message, const char* title) {
    // ponytail: console only, no DOM overlay yet; always answers OK/Yes (0) so callers carry on.
    fprintf(stderr, "[OsGui] message box (%s): %s\n", title && *title ? title : "World of Warcraft", message ? message : "");
    return 0;
}

#endif
