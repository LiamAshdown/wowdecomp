#if defined(WOWDECOMP_PLATFORM_WEB)

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <pthread.h>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
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
// While the last input was a touch, the game's cursor is not drawn (CGxDeviceOpenGlWeb.cpp): no finger has a cursor
// under it. A real mouse moving brings it back.
static std::atomic<bool> s_touchInput{false};
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

// A touch drag is queued with this in place of a button (OnTouch). Which button it presses is decided on the game thread
// when the press comes out of the queue, because only the game knows whether the finger went down on the world (a right
// drag, which turns the camera and the character) or on a frame (a left drag, which picks up or moves it).
static const int32_t kTouchDragButton = 0x40000000;
static int32_t (*s_touchDragProc)();
static int32_t s_touchDragButton = MOUSE_BUTTON_LEFT;   // game thread only

// Set by WebTouch.cpp: returns the MOUSEBUTTON a drag starting at the cursor presses.
void OsWebTouchSetDragProc(int32_t (*proc)()) {
    s_touchDragProc = proc;
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

    // The moves before the press have been dispatched by now, so the game's mouse focus is the frame under the finger.
    if ((*id == OS_INPUT_MOUSE_DOWN || *id == OS_INPUT_MOUSE_UP) && *param0 == kTouchDragButton) {
        if (*id == OS_INPUT_MOUSE_DOWN) {
            s_touchDragButton = s_touchDragProc ? s_touchDragProc() : MOUSE_BUTTON_LEFT;
        }

        s_osButtonState &= ~kTouchDragButton;
        s_osButtonState = *id == OS_INPUT_MOUSE_DOWN ? s_osButtonState | s_touchDragButton : s_osButtonState & ~s_touchDragButton;
        *param0 = s_touchDragButton;
    }

    pthread_mutex_unlock(&s_queueLock);

    return 1;
}

// Drawing-buffer pixels per CSS pixel: devicePixelRatio times the page's render scale (index.html, below 1 on phones).
// The Gx device sizes the canvas with it (CGxDeviceOpenGlWeb.cpp), so mouse coordinates use the same factor.
double OsWebPixelRatio() {
    return MAIN_THREAD_EM_ASM_DOUBLE({ return devicePixelRatio * (window.wowRenderScale || 1); });
}

// Canvas position inside the page in CSS pixels; the drawing buffer is the canvas CSS size times OsWebPixelRatio.
EM_JS(double, WebCanvasOrigin, (int axis), {
    var rect = document.getElementById('canvas').getBoundingClientRect();
    return axis ? rect.top : rect.left;
});

static void ClientToCanvas(int32_t clientX, int32_t clientY, int32_t* x, int32_t* y) {
    double scale = OsWebPixelRatio();
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

    double scale = OsWebPixelRatio();
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
    s_touchInput = false;

    if (s_osMouseMode == OS_MOUSE_MODE_RELATIVE) {
        double scale = OsWebPixelRatio();
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

// ---- Touch (docs/mobile-plan.md, T3) ----
// The touch HUD (WebTouch.cpp) owns screen regions, its zones: a finger that lands in one belongs to it, and drives the
// stick or presses a button there. Any other finger drives the game's one cursor: a tap is a left click, a hold is a right
// click, a drag is a left or right drag (decided in OsQueueGet), and two fingers pinch the camera zoom.
// Zone rects are fractions of the canvas with y up, so they need no knowledge of the UI's coordinates. The zones, stick
// and button events are guarded by s_queueLock; the fingers are only touched on the main thread.

// A type zone is a button that also opens the phone keyboard: iOS only does that from inside the tap itself, and the game
// would focus its edit box a frame later, so the field (OsIME.cpp) is focused here, while the finger lifts.
enum { TOUCH_ZONE_NONE, TOUCH_ZONE_STICK, TOUCH_ZONE_BUTTON, TOUCH_ZONE_TYPE };

struct TouchZone {
    int32_t kind;
    float left;
    float bottom;
    float right;
    float top;
};

static const int32_t kMaxTouchZones = 64;
static TouchZone s_touchZones[kMaxTouchZones];

struct TouchStick {
    bool active;
    float x;
    float y;
    float originX;
    float originY;
};

static TouchStick s_touchStick;

// Button zone presses (+id) and releases (-id, or -(id + kTouchOutside) when the finger left the zone first) for
// OsWebTouchNextEvent. A quick tap is both in one frame, so they queue.
static const int32_t kTouchOutside = 1000;
static const int32_t kTouchEventsSize = 64;
static int32_t s_touchEvents[kTouchEventsSize];
static int32_t s_touchEventsHead;
static int32_t s_touchEventsTail;

// How far the finger holding each button zone has moved since it pressed (CSS px, y up), and when it pressed and let go
// (ms, 0 while held): thumb aiming and flicks in WebTouch.lua, timed here because the game's frames can lag.
struct ZoneDrag {
    float dx;
    float dy;
    double start;
    double end;
};

static ZoneDrag s_zoneDrag[kMaxTouchZones];

enum TouchRole { TOUCH_NONE, TOUCH_STICK, TOUCH_BUTTON, TOUCH_CURSOR, TOUCH_PINCH, TOUCH_IGNORED };

struct Finger {
    int32_t id;
    TouchRole role;
    int32_t zone;
    double startX;
    double startY;
    double x;
    double y;
    double startTime;
    bool dragging;
};

static Finger s_fingers[10];


int32_t OsWebTouchHidesCursor() {
    return s_touchInput;
}
static double s_pinchDistance;
static double s_pinchWheel;
static double s_pinchStart;
static double s_pinchTravel;

static const double kTouchSlop = 10.0;      // CSS px a finger moves before a tap becomes a drag
// Set from the Touch options (WebTouch.cpp): a tap held this long is a right click, and a camera drag turns this much
// faster than the finger moves. Main thread only.
static double s_touchHoldMs = 450.0;
static double s_touchCameraSpeed = 1.0;
static const double kStickRadius = 56.0;    // CSS px for full deflection; WebTouch.lua moves the knob as far
static const double kPinchStep = 30.0;      // CSS px of pinch per camera zoom step
static const double kTwoFingerTapMs = 300.0; // two fingers down and up this quickly, barely moving, is Escape

// Pinch steps go to WebTouch.lua as these "zones" (above kMaxTouchZones): the camera zooms whatever frame the fingers are
// over, where the wheel would scroll a chat frame or a list instead.
static const int32_t kTouchZoomIn = 100;
static const int32_t kTouchZoomOut = 101;

void OsWebTouchSetZone(int32_t id, int32_t kind, float left, float bottom, float right, float top) {
    if (id < 0 || id >= kMaxTouchZones) {
        return;
    }

    pthread_mutex_lock(&s_queueLock);
    s_touchZones[id] = { kind, left, bottom, right, top };
    pthread_mutex_unlock(&s_queueLock);
}

// The game UI is going away (a reload, or back to the glue screens): its zones must not swallow touches any more.
void OsWebTouchClearZones() {
    pthread_mutex_lock(&s_queueLock);
    memset(s_touchZones, 0, sizeof(s_touchZones));
    s_touchStick = {};
    s_touchEventsHead = s_touchEventsTail;
    pthread_mutex_unlock(&s_queueLock);
}

struct TouchTuning {
    double holdMs;
    double cameraSpeed;
};

static void ApplyTouchTuning(void* arg) {
    TouchTuning* tuning = static_cast<TouchTuning*>(arg);
    s_touchHoldMs = tuning->holdMs;
    s_touchCameraSpeed = tuning->cameraSpeed;
    delete tuning;
}

// From the game thread; the values are read by the touch handlers on the main thread, so they are handed over there.
void OsWebTouchSetTuning(double holdMs, double cameraSpeed) {
    emscripten_proxy_async(emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(), ApplyTouchTuning,
        new TouchTuning{ holdMs, cameraSpeed });
}

void OsWebTouchGetStick(bool* active, float* x, float* y, float* originX, float* originY) {
    pthread_mutex_lock(&s_queueLock);
    *active = s_touchStick.active;
    *x = s_touchStick.x;
    *y = s_touchStick.y;
    *originX = s_touchStick.originX;
    *originY = s_touchStick.originY;
    pthread_mutex_unlock(&s_queueLock);
}

int32_t OsWebTouchNextEvent(int32_t* id, int32_t* down, int32_t* outside) {
    pthread_mutex_lock(&s_queueLock);

    int32_t result = s_touchEventsHead != s_touchEventsTail;
    if (result) {
        int32_t event = s_touchEvents[s_touchEventsHead];
        s_touchEventsHead = (s_touchEventsHead + 1) % kTouchEventsSize;
        *id = event < 0 ? -event : event;
        *down = event > 0;
        *outside = *id >= kTouchOutside;
        *id = *outside ? *id - kTouchOutside : *id;
    }

    pthread_mutex_unlock(&s_queueLock);

    return result;
}

void OsWebTouchGetZoneDrag(int32_t id, float* dx, float* dy, float* ms) {
    pthread_mutex_lock(&s_queueLock);
    bool valid = id >= 0 && id < kMaxTouchZones;
    const ZoneDrag& drag = s_zoneDrag[valid ? id : 0];
    *dx = valid ? drag.dx : 0.0f;
    *dy = valid ? drag.dy : 0.0f;
    *ms = valid && drag.start ? static_cast<float>((drag.end ? drag.end : emscripten_get_now()) - drag.start) : 0.0f;
    pthread_mutex_unlock(&s_queueLock);
}

static void CanvasPoint(float x, float y, int32_t* px, int32_t* py) {
    pthread_mutex_lock(&s_queueLock);
    *px = static_cast<int32_t>(std::lround(x * s_defaultWindowRect.right));
    *py = static_cast<int32_t>(std::lround((1.0f - y) * s_defaultWindowRect.bottom));
    pthread_mutex_unlock(&s_queueLock);
}

// Moves the game's cursor to a point given in fractions of the canvas (y up), through the queue like any mouse move:
// thumb aiming steers a ground spell's targeting circle with it. Called on the game thread.
void OsWebTouchMoveCursor(float x, float y) {
    int32_t px, py;
    CanvasPoint(x, y, &px, &py);
    OsQueuePut(OS_INPUT_MOUSE_MOVE, 0, px, py, 0);
}

// A left click there, as a tap would make: it puts an aimed ground spell down.
void OsWebTouchClickAt(float x, float y) {
    int32_t px, py;
    CanvasPoint(x, y, &px, &py);
    OsQueuePut(OS_INPUT_MOUSE_MOVE, 0, px, py, 0);
    OsQueuePut(OS_INPUT_MOUSE_DOWN, MOUSE_BUTTON_LEFT, px, py, 0);
    OsQueuePut(OS_INPUT_MOUSE_UP, MOUSE_BUTTON_LEFT, px, py, 0);
}

static void PutTouchEvent(int32_t zone, bool down, bool outside = false) {
    pthread_mutex_lock(&s_queueLock);

    int32_t next = (s_touchEventsTail + 1) % kTouchEventsSize;
    if (next != s_touchEventsHead) {
        int32_t code = outside ? zone + kTouchOutside : zone;
        s_touchEvents[s_touchEventsTail] = down ? code : -code;
        s_touchEventsTail = next;
    }

    pthread_mutex_unlock(&s_queueLock);
}

// The zone under a point (fractions, y up); button zones win over the stick's.
static int32_t TouchZoneAt(float x, float y, int32_t* zone) {
    pthread_mutex_lock(&s_queueLock);

    int32_t kind = TOUCH_ZONE_NONE;
    for (int32_t want : { TOUCH_ZONE_BUTTON, TOUCH_ZONE_STICK }) {
        for (int32_t id = 1; id < kMaxTouchZones && kind == TOUCH_ZONE_NONE; id++) {
            const TouchZone& z = s_touchZones[id];
            bool match = z.kind == want || (want == TOUCH_ZONE_BUTTON && z.kind == TOUCH_ZONE_TYPE);
            if (match && x >= z.left && x < z.right && y >= z.bottom && y < z.top) {
                kind = want;
                *zone = id;
            }
        }
    }

    pthread_mutex_unlock(&s_queueLock);

    return kind;
}

static Finger* FindFinger(int32_t id) {
    for (Finger& finger : s_fingers) {
        if (finger.role != TOUCH_NONE && finger.id == id) {
            return &finger;
        }
    }

    return nullptr;
}

static Finger* FindRole(TouchRole role, const Finger* other = nullptr) {
    for (Finger& finger : s_fingers) {
        if (finger.role == role && &finger != other) {
            return &finger;
        }
    }

    return nullptr;
}

// Moves the cursor to a point in the canvas (CSS px), as the absolute move a mouse makes before it clicks.
static void TouchMoveCursor(double x, double y) {
    double ratio = OsWebPixelRatio();

    pthread_mutex_lock(&s_queueLock);
    s_pendingMove = true;
    s_pendingX = s_mouseX = static_cast<int32_t>(std::lround(x * ratio));
    s_pendingY = s_mouseY = static_cast<int32_t>(std::lround(y * ratio));
    pthread_mutex_unlock(&s_queueLock);
}

static void TouchStart(int32_t id, double x, double y, double width, double height) {
    Finger* finger = FindRole(TOUCH_NONE);
    if (!finger) {
        return;
    }

    *finger = { id, TOUCH_IGNORED, 0, x, y, x, y, emscripten_get_now(), false };
    s_touchInput = true;

    float fx = static_cast<float>(x / width);
    float fy = static_cast<float>(1.0 - y / height);
    int32_t zone = 0;
    int32_t kind = TouchZoneAt(fx, fy, &zone);
    Finger* cursor = FindRole(TOUCH_CURSOR, finger);

    if (kind == TOUCH_ZONE_BUTTON) {
        finger->role = TOUCH_BUTTON;
        finger->zone = zone;
        pthread_mutex_lock(&s_queueLock);
        s_zoneDrag[zone] = { 0.0f, 0.0f, finger->startTime, 0.0 };
        pthread_mutex_unlock(&s_queueLock);
        PutTouchEvent(zone, true);
    } else if (kind == TOUCH_ZONE_STICK && !FindRole(TOUCH_STICK, finger)) {
        // The stick centres itself where the thumb lands.
        finger->role = TOUCH_STICK;
        pthread_mutex_lock(&s_queueLock);
        s_touchStick = { true, 0.0f, 0.0f, fx, fy };
        pthread_mutex_unlock(&s_queueLock);
    } else if (kind == TOUCH_ZONE_STICK) {
        finger->role = TOUCH_IGNORED;
    } else if (!cursor && !FindRole(TOUCH_PINCH, finger)) {
        finger->role = TOUCH_CURSOR;
        if (s_osMouseMode != OS_MOUSE_MODE_RELATIVE) {
            TouchMoveCursor(x, y);
        }
    } else if (cursor) {
        // A second free finger: the two pinch, which ends whatever the first was doing.
        if (cursor->dragging) {
            OsQueuePut(OS_INPUT_MOUSE_UP, kTouchDragButton, s_mouseX, s_mouseY, 0);
        }

        cursor->role = TOUCH_PINCH;
        finger->role = TOUCH_PINCH;
        s_pinchDistance = std::hypot(x - cursor->x, y - cursor->y);
        s_pinchWheel = 0.0;
        s_pinchStart = cursor->startTime;
        s_pinchTravel = std::hypot(cursor->x - cursor->startX, cursor->y - cursor->startY);
    }
}

static void TouchMove(Finger* finger, double x, double y, double width, double height) {
    double dx = x - finger->x;
    double dy = y - finger->y;
    finger->x = x;
    finger->y = y;

    switch (finger->role) {
        case TOUCH_BUTTON:
            pthread_mutex_lock(&s_queueLock);
            s_zoneDrag[finger->zone].dx = static_cast<float>(x - finger->startX);
            s_zoneDrag[finger->zone].dy = static_cast<float>(finger->startY - y);
            pthread_mutex_unlock(&s_queueLock);
            break;

        case TOUCH_STICK: {
            pthread_mutex_lock(&s_queueLock);
            double sx = (x - s_touchStick.originX * width) / kStickRadius;
            double sy = ((1.0 - s_touchStick.originY) * height - y) / kStickRadius;
            double length = std::hypot(sx, sy);
            if (length > 1.0) {
                sx /= length;
                sy /= length;
            }
            s_touchStick.x = static_cast<float>(sx);
            s_touchStick.y = static_cast<float>(sy);
            pthread_mutex_unlock(&s_queueLock);
            break;
        }

        case TOUCH_CURSOR:
            if (!finger->dragging && std::hypot(x - finger->startX, y - finger->startY) > kTouchSlop) {
                finger->dragging = true;
                OsQueuePut(OS_INPUT_MOUSE_DOWN, kTouchDragButton, s_mouseX, s_mouseY, 0);
            }

            if (finger->dragging) {
                // A right drag on the world puts the game in relative mode (mouselook), which wants deltas.
                if (s_osMouseMode == OS_MOUSE_MODE_RELATIVE) {
                    double ratio = OsWebPixelRatio();
                    pthread_mutex_lock(&s_queueLock);
                    s_pendingRelative = true;
                    s_pendingDX += static_cast<int32_t>(std::lround(dx * ratio * s_touchCameraSpeed));
                    s_pendingDY += static_cast<int32_t>(std::lround(dy * ratio * s_touchCameraSpeed));
                    pthread_mutex_unlock(&s_queueLock);
                } else {
                    TouchMoveCursor(x, y);
                }
            }

            break;

        case TOUCH_PINCH: {
            Finger* other = FindRole(TOUCH_PINCH, finger);
            if (!other) {
                break;
            }

            double distance = std::hypot(x - other->x, y - other->y);
            s_pinchTravel += std::hypot(dx, dy);
            s_pinchWheel += distance - s_pinchDistance;
            s_pinchDistance = distance;

            // Spreading the fingers zooms in.
            while (s_pinchWheel >= kPinchStep || s_pinchWheel <= -kPinchStep) {
                bool in = s_pinchWheel > 0.0;
                s_pinchWheel -= in ? kPinchStep : -kPinchStep;
                PutTouchEvent(in ? kTouchZoomIn : kTouchZoomOut, true);
            }

            break;
        }

        default:
            break;
    }
}

static void TouchEnd(Finger* finger, bool cancel, double width, double height) {
    switch (finger->role) {
        case TOUCH_STICK:
            pthread_mutex_lock(&s_queueLock);
            s_touchStick = {};
            pthread_mutex_unlock(&s_queueLock);
            break;

        case TOUCH_BUTTON: {
            // Lifted off the button: WebTouch.lua does not cast (a slipped thumb, or a drag away to cancel).
            pthread_mutex_lock(&s_queueLock);
            const TouchZone& z = s_touchZones[finger->zone];
            float fx = static_cast<float>(finger->x / width);
            float fy = static_cast<float>(1.0 - finger->y / height);
            bool outside = cancel || fx < z.left || fx >= z.right || fy < z.bottom || fy >= z.top;
            bool type = z.kind == TOUCH_ZONE_TYPE;
            s_zoneDrag[finger->zone].end = emscripten_get_now();
            pthread_mutex_unlock(&s_queueLock);
            if (type && !outside) {
                EM_ASM({
                    var field = document.getElementById('wow-text');
                    if (field) field.focus({ preventScroll: true });
                });
            }
            PutTouchEvent(finger->zone, false, outside);
            break;
        }

        case TOUCH_CURSOR:
            if (finger->dragging) {
                OsQueuePut(OS_INPUT_MOUSE_UP, kTouchDragButton, s_mouseX, s_mouseY, 0);
            } else if (!cancel) {
                MOUSEBUTTON button = emscripten_get_now() - finger->startTime >= s_touchHoldMs ? MOUSE_BUTTON_RIGHT : MOUSE_BUTTON_LEFT;
                OsQueuePut(OS_INPUT_MOUSE_DOWN, button, s_mouseX, s_mouseY, 0);
                OsQueuePut(OS_INPUT_MOUSE_UP, button, s_mouseX, s_mouseY, 0);
            }

            // Off the frame it was over, to the top edge, so its tooltip and highlight go away with the finger.
            if (s_osMouseMode != OS_MOUSE_MODE_RELATIVE) {
                TouchMoveCursor(width / 2.0, 0.0);
            }

            break;

        case TOUCH_PINCH:
            // The finger left behind waits to be lifted.
            if (Finger* other = FindRole(TOUCH_PINCH, finger)) {
                other->role = TOUCH_IGNORED;
            }

            // A quick two-finger tap is Escape, which phones have no key for: it closes menus and cancels casts.
            if (!cancel && emscripten_get_now() - s_pinchStart < kTwoFingerTapMs && s_pinchTravel < kTouchSlop * 2.0) {
                OsQueuePut(OS_INPUT_KEY_DOWN, KEY_ESCAPE, 1, 0, 0);
                OsQueuePut(OS_INPUT_KEY_UP, KEY_ESCAPE, 1, 0, 0);
            }

            break;

        default:
            break;
    }

    finger->role = TOUCH_NONE;
}

static bool OnTouch(int eventType, const EmscriptenTouchEvent* event, void* userData) {
    if (eventType == EMSCRIPTEN_EVENT_TOUCHSTART) {
        // Key events (a Bluetooth keyboard) only go to the focused element.
        MAIN_THREAD_ASYNC_EM_ASM({ document.getElementById('canvas').focus(); });
    }

    double width = 0.0;
    double height = 0.0;
    emscripten_get_element_css_size("#canvas", &width, &height);
    if (width <= 0.0 || height <= 0.0) {
        return true;
    }

    double originX = WebCanvasOrigin(0);
    double originY = WebCanvasOrigin(1);

    for (int32_t i = 0; i < event->numTouches; i++) {
        const EmscriptenTouchPoint& touch = event->touches[i];
        if (!touch.isChanged) {
            continue;
        }

        double x = touch.clientX - originX;
        double y = touch.clientY - originY;

        if (eventType == EMSCRIPTEN_EVENT_TOUCHSTART) {
            TouchStart(touch.identifier, x, y, width, height);
        } else if (Finger* finger = FindFinger(touch.identifier)) {
            if (eventType == EMSCRIPTEN_EVENT_TOUCHMOVE) {
                TouchMove(finger, x, y, width, height);
            } else {
                TouchEnd(finger, eventType == EMSCRIPTEN_EVENT_TOUCHCANCEL, width, height);
            }
        }
    }

    // Consumed: no scrolling, no zoom, and no mouse events made up from the touch.
    return true;
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
    WEB_REGISTER(emscripten_set_touchstart_callback_on_thread("#canvas", nullptr, true, OnTouch, mainThread));
    WEB_REGISTER(emscripten_set_touchmove_callback_on_thread("#canvas", nullptr, true, OnTouch, mainThread));
    WEB_REGISTER(emscripten_set_touchend_callback_on_thread("#canvas", nullptr, true, OnTouch, mainThread));
    WEB_REGISTER(emscripten_set_touchcancel_callback_on_thread("#canvas", nullptr, true, OnTouch, mainThread));

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

// Password boxes: the phone keyboard's text field turns into a password field (OsIME.cpp).
void OsWebTextSecure(int32_t secure);

void OsInputEnableSecureMode(int32_t enable) {
    OsWebTextSecure(enable);
}

#endif
