#if defined(WOWDECOMP_PLATFORM_WEB)

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <windows.h>
#include "Engine/Source/Event/Types.h"
#include "Engine/Source/Os/Input.h"
#include "Engine/Source/Os/Joystick.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Storm/H/storm.h"

// There is no window message pump on the web: DOM handlers on the main browser thread feed the queue below and the
// game thread drains it through OsInputGet. The queue and the state beside it are the only things both threads touch,
// so they share one lock.
static pthread_mutex_t s_queueLock = PTHREAD_MUTEX_INITIALIZER;
static int32_t s_queueHead;
static int32_t s_queueTail;
// Win64 keeps 16 entries and leans on the OS message queue; DOM events arrive in bursts, so keep more.
static const int32_t kQueueSize = 512;
static OSEVENT s_queue[kQueueSize];

static bool s_keyDown[KEY_LAST + 1];
static uint32_t s_osButtonState;
static int32_t s_mouseX;
static int32_t s_mouseY;
static std::atomic<OS_MOUSE_MODE> s_osMouseMode{OS_MOUSE_MODE_NORMAL};
// Whether this stretch of relative mode already asked for pointer lock (OnMouseMove).
static std::atomic<bool> s_pointerLockRequested{false};
static float s_mouseSpeed = 1.0f;
static int32_t s_WindowResizeLock;
static void (*s_eventPollProc)();
static RECT s_defaultWindowRect = { 0, 0, 1280, 720 };

// Mouse moves arrive far faster than the game polls, and the queue drops its oldest entry when full, so moves
// are folded into one pending absolute move and one pending relative delta that get queued when the game asks for the
// next event or when another event has to be ordered after them.
static bool s_pendingMove;
static int32_t s_pendingX;
static int32_t s_pendingY;
static bool s_pendingRelative;
static int32_t s_pendingDX;
static int32_t s_pendingDY;

// Only touched by the DOM handlers on the main thread.
static uint32_t s_domButtons;
static bool s_registered;

static void PutLocked(OSINPUT id, int32_t param0, int32_t param1, int32_t param2) {
    switch (id) {
        case OS_INPUT_KEY_DOWN:
        case OS_INPUT_KEY_UP:
            if (static_cast<uint32_t>(param0) <= KEY_LAST) {
                s_keyDown[param0] = id == OS_INPUT_KEY_DOWN;
            }

            break;

        case OS_INPUT_MOUSE_DOWN:
            s_osButtonState |= param0;
            s_mouseX = param1;
            s_mouseY = param2;
            break;

        case OS_INPUT_MOUSE_UP:
            s_osButtonState &= ~param0;
            s_mouseX = param1;
            s_mouseY = param2;
            break;

        case OS_INPUT_MOUSE_MOVE:
            s_mouseX = param1;
            s_mouseY = param2;
            break;

        default:
            break;
    }

    int32_t next = s_queueTail == kQueueSize - 1 ? 0 : s_queueTail + 1;

    if (next == s_queueHead) {
        s_queueHead = s_queueHead == kQueueSize - 1 ? 0 : s_queueHead + 1;
    }

    OSEVENT& event = s_queue[s_queueTail];
    event.id = id;
    event.param[0] = param0;
    event.param[1] = param1;
    event.param[2] = param2;
    event.param[3] = OsGetAsyncTimeMs();

    s_queueTail = next;
}

static void FlushMovesLocked() {
    if (s_pendingRelative) {
        s_pendingRelative = false;
        PutLocked(OS_INPUT_MOUSE_MOVE_RELATIVE, 0, s_pendingDX, s_pendingDY);
        s_pendingDX = 0;
        s_pendingDY = 0;
    }

    if (s_pendingMove) {
        s_pendingMove = false;
        PutLocked(OS_INPUT_MOUSE_MOVE, 0, s_pendingX, s_pendingY);
    }
}

void OsQueuePut(OSINPUT id, int32_t param0, int32_t param1, int32_t param2, int32_t param3) {
    pthread_mutex_lock(&s_queueLock);

    FlushMovesLocked();
    PutLocked(id, param0, param1, param2);

    pthread_mutex_unlock(&s_queueLock);
}

int32_t OsQueueGet(OSINPUT* id, int32_t* param0, int32_t* param1, int32_t* param2, int32_t* param3) {
    pthread_mutex_lock(&s_queueLock);

    if (s_queueHead == s_queueTail) {
        FlushMovesLocked();
    }

    if (s_queueHead == s_queueTail) {
        pthread_mutex_unlock(&s_queueLock);
        return 0;
    }

    OSEVENT& event = s_queue[s_queueHead];
    *id = event.id;
    *param0 = event.param[0];
    *param1 = event.param[1];
    *param2 = event.param[2];
    *param3 = event.param[3];

    s_queueHead = s_queueHead == kQueueSize - 1 ? 0 : s_queueHead + 1;

    pthread_mutex_unlock(&s_queueLock);

    return 1;
}

// Canvas position inside the page in CSS pixels; the drawing buffer is the canvas CSS size times devicePixelRatio.
EM_JS(double, WebCanvasOrigin, (int axis), {
    var rect = document.getElementById('canvas').getBoundingClientRect();
    return axis ? rect.top : rect.left;
});

static void ClientToCanvas(int32_t clientX, int32_t clientY, int32_t* x, int32_t* y) {
    double scale = emscripten_get_device_pixel_ratio();
    *x = static_cast<int32_t>(std::lround((clientX - WebCanvasOrigin(0)) * scale));
    *y = static_cast<int32_t>(std::lround((clientY - WebCanvasOrigin(1)) * scale));
}

// The drawing buffer size in pixels. Win64 only reports a size event when the client area really changed.
static bool UpdateCanvasSize(bool notify) {
    double cssWidth = 0.0;
    double cssHeight = 0.0;
    if (emscripten_get_element_css_size("#canvas", &cssWidth, &cssHeight) != EMSCRIPTEN_RESULT_SUCCESS) {
        return false;
    }

    double scale = emscripten_get_device_pixel_ratio();
    int32_t width = static_cast<int32_t>(std::lround(cssWidth * scale));
    int32_t height = static_cast<int32_t>(std::lround(cssHeight * scale));
    if (width <= 0 || height <= 0) {
        return false;
    }

    pthread_mutex_lock(&s_queueLock);

    bool changed = width != s_defaultWindowRect.right || height != s_defaultWindowRect.bottom;
    if (changed) {
        s_defaultWindowRect.left = 0;
        s_defaultWindowRect.top = 0;
        s_defaultWindowRect.right = width;
        s_defaultWindowRect.bottom = height;

        if (notify) {
            FlushMovesLocked();
            PutLocked(OS_INPUT_SIZE, width, height, 0);
        }
    }

    pthread_mutex_unlock(&s_queueLock);

    return changed;
}

// Called by the Gx device whenever it resizes the drawing buffer; the canvas can change size without a window resize.
// notify sends the OS_INPUT_SIZE event the UI re-lays out on.
void OsWebSetWindowSize(int32_t width, int32_t height, bool notify) {
    pthread_mutex_lock(&s_queueLock);
    s_defaultWindowRect.left = 0;
    s_defaultWindowRect.top = 0;
    s_defaultWindowRect.right = width;
    s_defaultWindowRect.bottom = height;

    if (notify) {
        FlushMovesLocked();
        PutLocked(OS_INPUT_SIZE, width, height, 0);
    }

    pthread_mutex_unlock(&s_queueLock);
}

struct DomKey {
    const char* code;
    KEY key;
};

// DOM KeyboardEvent.code in place of the Windows virtual keys ConvertKeyCode switches on. Letters and digits come from
// keyCode instead (see ConvertDomKey), which follows the keyboard layout the way MapVirtualKey does.
static const DomKey s_domKeys[] = {
    { "ShiftLeft", KEY_LSHIFT },
    { "ShiftRight", KEY_RSHIFT },
    { "ControlLeft", KEY_LCONTROL },
    { "ControlRight", KEY_RCONTROL },
    { "AltLeft", KEY_LALT },
    { "AltRight", KEY_RALT },
    { "Escape", KEY_ESCAPE },
    { "Enter", KEY_ENTER },
    { "NumpadEnter", KEY_ENTER },
    { "Backspace", KEY_BACKSPACE },
    { "Tab", KEY_TAB },
    { "ArrowLeft", KEY_LEFT },
    { "ArrowUp", KEY_UP },
    { "ArrowRight", KEY_RIGHT },
    { "ArrowDown", KEY_DOWN },
    { "Insert", KEY_INSERT },
    { "Delete", KEY_DELETE },
    { "Home", KEY_HOME },
    { "End", KEY_END },
    { "PageUp", KEY_PAGEUP },
    { "PageDown", KEY_PAGEDOWN },
    { "CapsLock", KEY_CAPSLOCK },
    { "NumLock", KEY_NUMLOCK },
    { "ScrollLock", KEY_SCROLLLOCK },
    { "Pause", KEY_PAUSE },
    { "PrintScreen", KEY_PRINTSCREEN },
    { "Space", KEY_SPACE },
    { "Numpad0", KEY_NUMPAD0 },
    { "Numpad1", KEY_NUMPAD1 },
    { "Numpad2", KEY_NUMPAD2 },
    { "Numpad3", KEY_NUMPAD3 },
    { "Numpad4", KEY_NUMPAD4 },
    { "Numpad5", KEY_NUMPAD5 },
    { "Numpad6", KEY_NUMPAD6 },
    { "Numpad7", KEY_NUMPAD7 },
    { "Numpad8", KEY_NUMPAD8 },
    { "Numpad9", KEY_NUMPAD9 },
    { "NumpadAdd", KEY_NUMPAD_PLUS },
    { "NumpadSubtract", KEY_NUMPAD_MINUS },
    { "NumpadMultiply", KEY_NUMPAD_MULTIPLY },
    { "NumpadDivide", KEY_NUMPAD_DIVIDE },
    { "NumpadDecimal", KEY_NUMPAD_DECIMAL },
    // The OEM keys, as MapVirtualKey reports them on a US layout.
    { "Backquote", KEY_TILDE },
    { "Minus", KEY_MINUS },
    { "Equal", KEY_PLUS },
    { "BracketLeft", KEY_BRACKET_OPEN },
    { "BracketRight", KEY_BRACKET_CLOSE },
    { "Backslash", KEY_BACKSLASH },
    { "Semicolon", KEY_SEMICOLON },
    { "Quote", KEY_APOSTROPHE },
    { "Comma", KEY_COMMA },
    { "Period", KEY_PERIOD },
    { "Slash", KEY_SLASH },
};

static int32_t ConvertDomKey(const EmscriptenKeyboardEvent* event, KEY* key) {
    // keyCode matches the Windows virtual key for letters and digits (and F1-F12, below).
    if ((event->keyCode >= 'A' && event->keyCode <= 'Z') || (event->keyCode >= '0' && event->keyCode <= '9')) {
        *key = static_cast<KEY>(event->keyCode);
        return 1;
    }

    const char* code = event->code;

    if (code[0] == 'F' && code[1] >= '1' && code[1] <= '9') {
        int32_t number = code[1] - '0';
        if (code[2] >= '0' && code[2] <= '9' && !code[3]) {
            number = number * 10 + (code[2] - '0');
        } else if (code[2]) {
            number = 0;
        }

        if (number >= 1 && number <= 12) {
            *key = static_cast<KEY>(KEY_F1 + number - 1);
            return 1;
        }

        return 0;
    }

    // A digit row key whose keyCode is not a digit (non-US layouts).
    if (!strncmp(code, "Digit", 5) && code[5] >= '0' && code[5] <= '9' && !code[6]) {
        *key = static_cast<KEY>(KEY_0 + (code[5] - '0'));
        return 1;
    }

    for (const DomKey& entry : s_domKeys) {
        if (!strcmp(entry.code, code)) {
            *key = entry.key;
            return 1;
        }
    }

    return 0;
}

static MOUSEBUTTON ConvertDomButton(unsigned short button) {
    switch (button) {
        case 0:
            return MOUSE_BUTTON_LEFT;

        case 1:
            return MOUSE_BUTTON_MIDDLE;

        case 2:
            return MOUSE_BUTTON_RIGHT;

        case 3:
            return MOUSE_BUTTON_XBUTTON1;

        case 4:
            return MOUSE_BUTTON_XBUTTON2;

        default:
            return MOUSE_BUTTON_NONE;
    }
}

// F11 asks the GL shim to log the next frame's draws (see GlShimSetDump).
std::atomic<int> g_webGxDumpRequest;

static bool OnKey(int eventType, const EmscriptenKeyboardEvent* event, void* userData) {
    KEY key;
    if (!ConvertDomKey(event, &key)) {
        return false;
    }

    if (key == KEY_F11 && eventType == EMSCRIPTEN_EVENT_KEYDOWN) {
        g_webGxDumpRequest = 1;
    }

    // Alt+Enter toggles fullscreen, as it switches between windowed and fullscreen in the native client. The page does
    // the switch (index.html); the key press is the user activation the browser asks for.
    if (key == KEY_ENTER && event->altKey) {
        if (eventType == EMSCRIPTEN_EVENT_KEYDOWN && !event->repeat) {
            MAIN_THREAD_ASYNC_EM_ASM({
                if (window.wowToggleFullscreen) window.wowToggleFullscreen();
            });
        }

        return true;
    }

    OsQueuePut(eventType == EMSCRIPTEN_EVENT_KEYDOWN ? OS_INPUT_KEY_DOWN : OS_INPUT_KEY_UP, key, 1, 0, 0);

    // The page toggles its log on F10, and ctrl+C/V/X must reach the browser for the clipboard events.
    if (key == KEY_F10 || event->metaKey) {
        return false;
    }

    if (event->ctrlKey && !event->altKey && (key == KEY_C || key == KEY_V || key == KEY_X)) {
        return false;
    }

    // Consuming keydown suppresses the browser's keypress, so the WM_CHAR equivalent comes from KeyboardEvent.key:
    // one UTF-8 encoded character means a printable key.
    if (eventType == EMSCRIPTEN_EVENT_KEYDOWN && !(event->ctrlKey && !event->altKey)) {
        const unsigned char* k = reinterpret_cast<const unsigned char*>(event->key);
        int32_t code = -1;

        if (k[0] && k[0] < 0x80 && !k[1]) {
            code = k[0];
        } else if ((k[0] & 0xE0) == 0xC0 && k[1] && !k[2]) {
            code = ((k[0] & 0x1F) << 6) | (k[1] & 0x3F);
        } else if ((k[0] & 0xF0) == 0xE0 && k[1] && k[2] && !k[3]) {
            code = ((k[0] & 0x0F) << 12) | ((k[1] & 0x3F) << 6) | (k[2] & 0x3F);
        }

        if (code >= 0x20 && code != 0x7F) {
            OsQueuePut(OS_INPUT_CHAR, code, 1, 0, 0);
        }
    }

    return true;
}

// WM_CHAR: printable characters only, as typed (the code page lookup tables on Win64 convert to Unicode, which the
// browser already gives us).
static bool OnKeyPress(int eventType, const EmscriptenKeyboardEvent* event, void* userData) {
    if (event->charCode < 0x20 || event->charCode == 0x7F || event->metaKey || (event->ctrlKey && !event->altKey)) {
        return false;
    }

    OsQueuePut(OS_INPUT_CHAR, static_cast<int32_t>(event->charCode), 1, 0, 0);

    return true;
}

static bool OnMouseDown(int eventType, const EmscriptenMouseEvent* event, void* userData) {
    MOUSEBUTTON button = ConvertDomButton(event->button);
    if (button == MOUSE_BUTTON_NONE) {
        return false;
    }

    // Default handling would drop the focus the key events need, so take it explicitly.
    MAIN_THREAD_ASYNC_EM_ASM({ document.getElementById('canvas').focus(); });

    s_domButtons |= button;

    int32_t x;
    int32_t y;
    if (s_osMouseMode == OS_MOUSE_MODE_RELATIVE) {
        OsInputGetMousePosition(&x, &y);
    } else {
        ClientToCanvas(event->clientX, event->clientY, &x, &y);

        // Windows always delivers a WM_MOUSEMOVE to the click position first; the UI picks the clicked frame from it.
        pthread_mutex_lock(&s_queueLock);

        if (x != s_mouseX || y != s_mouseY) {
            s_pendingMove = true;
            s_pendingX = x;
            s_pendingY = y;
            s_mouseX = x;
            s_mouseY = y;
        }

        pthread_mutex_unlock(&s_queueLock);
    }

    OsQueuePut(OS_INPUT_MOUSE_DOWN, button, x, y, 0);

    return true;
}

// Registered on the window: Win64 captures the mouse while a button is held, so releases outside the canvas still count.
static bool OnMouseUp(int eventType, const EmscriptenMouseEvent* event, void* userData) {
    MOUSEBUTTON button = ConvertDomButton(event->button);
    if (button == MOUSE_BUTTON_NONE || !(s_domButtons & button)) {
        return false;
    }

    s_domButtons &= ~button;

    int32_t x;
    int32_t y;
    if (s_osMouseMode == OS_MOUSE_MODE_RELATIVE) {
        OsInputGetMousePosition(&x, &y);
    } else {
        ClientToCanvas(event->clientX, event->clientY, &x, &y);
    }

    OsQueuePut(OS_INPUT_MOUSE_UP, button, x, y, 0);

    return true;
}

static bool OnMouseMove(int eventType, const EmscriptenMouseEvent* event, void* userData) {
    if (s_osMouseMode == OS_MOUSE_MODE_RELATIVE) {
        double scale = emscripten_get_device_pixel_ratio();
        int32_t dx = static_cast<int32_t>(std::lround(event->movementX * scale));
        int32_t dy = static_cast<int32_t>(std::lround(event->movementY * scale));

        if (dx || dy) {
            pthread_mutex_lock(&s_queueLock);
            s_pendingRelative = true;
            s_pendingDX += dx;
            s_pendingDY += dy;
            pthread_mutex_unlock(&s_queueLock);
        }

        // movementX/Y work without pointer lock, and every lock makes the browser show "press Esc to show your cursor"
        // (the game enters relative mode on any press in the world, clicks included). So the lock is only taken when
        // the hidden cursor nears the edge of the window, where the movement would otherwise stop.
        if (!s_pointerLockRequested) {
            double width = 0.0;
            double height = 0.0;
            emscripten_get_element_css_size("#canvas", &width, &height);
            const double margin = 32.0;

            if (event->clientX < margin || event->clientY < margin || event->clientX > width - margin || event->clientY > height - margin) {
                s_pointerLockRequested = true;
                MAIN_THREAD_ASYNC_EM_ASM({
                    try {
                        var lock = document.getElementById('canvas').requestPointerLock();
                        if (lock && lock.catch) {
                            lock.catch(function () {});
                        }
                    } catch (e) {
                    }
                });
            }
        }

        return false;
    }

    int32_t x;
    int32_t y;
    ClientToCanvas(event->clientX, event->clientY, &x, &y);

    pthread_mutex_lock(&s_queueLock);

    // Without a button held this is just the cursor passing over the rest of the page.
    if (s_domButtons || (x >= 0 && y >= 0 && x < s_defaultWindowRect.right && y < s_defaultWindowRect.bottom)) {
        s_pendingMove = true;
        s_pendingX = x;
        s_pendingY = y;
        s_mouseX = x;
        s_mouseY = y;
    }

    pthread_mutex_unlock(&s_queueLock);

    return false;
}

static bool OnWheel(int eventType, const EmscriptenWheelEvent* event, void* userData) {
    // WM_MOUSEWHEEL reports multiples of 120, positive away from the user; DOM deltaY is positive towards the user.
    double unit = event->deltaMode == DOM_DELTA_PIXEL ? 1.2 : (event->deltaMode == DOM_DELTA_LINE ? 40.0 : 120.0);
    double delta = -event->deltaY * unit;
    if (delta > -1.0 && delta < 1.0) {
        delta = event->deltaY < 0.0 ? 1.0 : (event->deltaY > 0.0 ? -1.0 : 0.0);
    }

    if (delta == 0.0) {
        return false;
    }

    delta = delta > 32767.0 ? 32767.0 : (delta < -32768.0 ? -32768.0 : delta);

    int32_t x;
    int32_t y;
    if (s_osMouseMode == OS_MOUSE_MODE_RELATIVE) {
        OsInputGetMousePosition(&x, &y);
    } else {
        ClientToCanvas(event->mouse.clientX, event->mouse.clientY, &x, &y);
    }

    OsQueuePut(OS_INPUT_MOUSE_WHEEL, static_cast<int32_t>(std::lround(delta)), x, y, 0);

    return true;
}

static bool OnFocus(int eventType, const EmscriptenFocusEvent* event, void* userData) {
    bool active = eventType == EMSCRIPTEN_EVENT_FOCUS;

    s_domButtons = 0;

    pthread_mutex_lock(&s_queueLock);

    FlushMovesLocked();

    if (!active) {
        // Key releases that happen while unfocused never arrive.
        for (uint32_t key = 0; key <= KEY_LAST; key++) {
            if (s_keyDown[key]) {
                PutLocked(OS_INPUT_KEY_UP, key, 1, 0);
            }
        }
    }

    s_osButtonState = 0;
    PutLocked(OS_INPUT_FOCUS, active, 0, 0);

    pthread_mutex_unlock(&s_queueLock);

    return false;
}

// Only the rect used for mouse coordinates is updated here. The Gx device sends OS_INPUT_SIZE once it has resized the
// drawing buffer and the resolution CVars the UI takes its aspect ratio from (CGxDeviceOpenGlWeb.cpp).
static bool OnResize(int eventType, const EmscriptenUiEvent* event, void* userData) {
    UpdateCanvasSize(false);

    return false;
}

#define WEB_REGISTER(call) \
    do { \
        if ((call) != EMSCRIPTEN_RESULT_SUCCESS) { \
            fprintf(stderr, "[OsInput] %s failed\n", #call); \
        } \
    } while (0)

// Registered through the proxied html5 API, so this can run on the game thread while the handlers run on the main one.
static void RegisterDomHandlers() {
    const pthread_t mainThread = EM_CALLBACK_THREAD_CONTEXT_MAIN_RUNTIME_THREAD;

    UpdateCanvasSize(false);

    WEB_REGISTER(emscripten_set_keydown_callback_on_thread("#canvas", nullptr, true, OnKey, mainThread));
    WEB_REGISTER(emscripten_set_keyup_callback_on_thread("#canvas", nullptr, true, OnKey, mainThread));
    WEB_REGISTER(emscripten_set_keypress_callback_on_thread("#canvas", nullptr, true, OnKeyPress, mainThread));
    WEB_REGISTER(emscripten_set_mousedown_callback_on_thread("#canvas", nullptr, true, OnMouseDown, mainThread));
    WEB_REGISTER(emscripten_set_mouseup_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, true, OnMouseUp, mainThread));
    WEB_REGISTER(emscripten_set_mousemove_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, true, OnMouseMove, mainThread));
    WEB_REGISTER(emscripten_set_wheel_callback_on_thread("#canvas", nullptr, true, OnWheel, mainThread));
    WEB_REGISTER(emscripten_set_focus_callback_on_thread("#canvas", nullptr, true, OnFocus, mainThread));
    WEB_REGISTER(emscripten_set_blur_callback_on_thread("#canvas", nullptr, true, OnFocus, mainThread));
    WEB_REGISTER(emscripten_set_resize_callback_on_thread(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, true, OnResize, mainThread));

    // Key events only go to the focused element.
    MAIN_THREAD_ASYNC_EM_ASM({ document.getElementById('canvas').focus(); });
}

void OsInputSetWindowResizeLock(int32_t resizeLock) {
    s_WindowResizeLock = resizeLock;
}

float OsInputGetMouseSpeed() {
    return s_mouseSpeed;
}

int32_t OsInputIsKeyDown(KEY key) {
    return static_cast<uint32_t>(key) <= KEY_LAST && s_keyDown[key];
}

int32_t OsInputIsMouseButtonDown(MOUSEBUTTON button) {
    return (s_osButtonState & button) != 0;
}

void OsInputInitialize() {
    if (!s_registered) {
        s_registered = true;
        RegisterDomHandlers();
    }
}

void OsInputDestroy() {
}

void OsInputSetEventPollProc(void (*pollProc)()) {
    s_eventPollProc = pollProc;
}

int32_t OsInputGet(OSINPUT* id, int32_t* param0, int32_t* param1, int32_t* param2, int32_t* param3) {
    *id = static_cast<OSINPUT>(-1);

    OsPollJoysticks(1);

    return OsQueueGet(id, param0, param1, param2, param3);
}

void OsInputSetMouseMode(OS_MOUSE_MODE mode) {
    if (mode >= OS_MOUSE_MODES) {
        SErrSetLastError(STORM_ERROR_INVALID_PARAMETER);
        return;
    }

    if (mode == s_osMouseMode.exchange(mode)) {
        return;
    }

    // Relative mode runs on movementX/Y; OnMouseMove takes pointer lock only if a drag reaches the window edge (the
    // click that started it still counts as the user activation the browser asks for).
    s_pointerLockRequested = false;

    if (mode != OS_MOUSE_MODE_RELATIVE) {
        MAIN_THREAD_ASYNC_EM_ASM({
            if (document.pointerLockElement) document.exitPointerLock();
        });
    }
}

void OsInputSetMouseSpeed(float speed) {
    s_mouseSpeed = speed < 0.1f ? 0.1f : (speed >= 2.0f ? 2.0f : speed);
}

void OsInputGetMousePosition(int32_t* x, int32_t* y) {
    if (x) {
        *x = s_mouseX;
    }

    if (y) {
        *y = s_mouseY;
    }
}

// The browser cannot move the cursor; this only moves the position the game reads back.
void OsInputSetMousePosition(int32_t x, int32_t y) {
    s_mouseX = x;
    s_mouseY = y;
}

// Not part of the Os input API: the Gx device calls this where the D3D device calls ShowCursor.
void OsWebSetCursorVisible(int32_t visible) {
    MAIN_THREAD_ASYNC_EM_ASM({ document.getElementById('canvas').style.cursor = $0 ? "" : "none"; }, visible);
}

int32_t OsGetDefaultWindowRect(tagRECT* rect) {
    if (!rect) {
        SErrSetLastError(STORM_ERROR_INVALID_PARAMETER);
        return 0;
    }

    pthread_mutex_lock(&s_queueLock);
    *rect = s_defaultWindowRect;
    pthread_mutex_unlock(&s_queueLock);

    return 1;
}

intptr_t OsWindowProc(void* window, uint32_t message, uintptr_t wparam, intptr_t lparam) {
    return 0;
}

void OsInputEnableSecureMode(int32_t enable) {
}

#endif
