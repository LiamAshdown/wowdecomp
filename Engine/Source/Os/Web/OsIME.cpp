#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cstdint>
#include <cstring>
#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include "Engine/Source/Event/Types.h"
#include "Engine/Source/Os/Input.h"
#include "Engine/Source/Os/OsIME.h"

// No IME on the web yet: committed text arrives as OS_INPUT_CHAR events instead.

// Input.cpp
int32_t OsWebTouchHidesCursor();

// The phone keyboard (docs/mobile-plan.md, T2). A focused game edit box focuses a hidden text field on the page, which
// opens the keyboard, and what is typed into the field goes to the game the way keys on the canvas do (Input.cpp).
// Styled by #wow-text and #wow-typebar in index.html.

// From the page (main thread): a character code, or 8, 13 and 27 for Backspace, Enter and Escape.
extern "C" EMSCRIPTEN_KEEPALIVE void OsWebText(int32_t code) {
    KEY key = code == 8 ? KEY_BACKSPACE : code == 13 ? KEY_ENTER : code == 27 ? KEY_ESCAPE : KEY_NONE;

    if (key != KEY_NONE) {
        OsQueuePut(OS_INPUT_KEY_DOWN, key, 1, 0, 0);
        OsQueuePut(OS_INPUT_KEY_UP, key, 1, 0, 0);
    } else if (code >= 0x20 && code != 0x7F) {
        OsQueuePut(OS_INPUT_CHAR, code, 1, 0, 0);
    }
}

EM_JS(void, WebTextSetup, (), {
    var field = document.createElement('input');
    field.id = 'wow-text';
    field.autocomplete = 'off';
    field.spellcheck = false;
    field.enterKeyHint = 'send';
    field.setAttribute('autocorrect', 'off');
    field.setAttribute('autocapitalize', 'off');
    field.sent = '';

    var bar = document.createElement('div');
    bar.id = 'wow-typebar';
    bar.textContent = 'Tap to type';
    document.body.append(field, bar);

    var canvas = document.getElementById('canvas');
    var vv = window.visualViewport;
    var vk = navigator.virtualKeyboard;
    // Chrome reports the keyboard's rect only while it overlays the page (the viewport meta asks for that too).
    if (vk) vk.overlaysContent = true;

    // Where the keyboard starts, in layout viewport pixels. Chrome gives its rect; Safari shrinks the visual viewport.
    // ponytail: Firefox does neither while the keyboard overlays the page, so there the field stays under it.
    function keyboardTop() {
        if (vk && vk.boundingRect.height > 0) return vk.boundingRect.y;
        return vv ? vv.offsetTop + vv.height : innerHeight;
    }

    function update() {
        var top = keyboardTop();
        var up = field.want && top < innerHeight - 80;
        field.classList.toggle('on', up);
        field.style.top = up ? (top - field.offsetHeight) + 'px' : '';
        // The keyboard didn't come up (iOS only opens it from a gesture) or was closed while the edit box kept focus.
        bar.classList.toggle('on', field.want && field.settled && !up);
    }

    if (vv) {
        vv.addEventListener('resize', update);
        vv.addEventListener('scroll', update);
    }
    if (vk) vk.addEventListener('geometrychange', update);

    function clear() {
        field.value = '';
        field.sent = '';
    }

    // Send the change against what was already sent: autocorrect and predictive text rewrite the word being typed.
    // ponytail: assumes the caret is at the end, as on the canvas; arrow keys go to the game and keep it there.
    field.addEventListener('input', function() {
        var sent = Array.from(field.sent);
        var now = Array.from(field.value);
        var same = 0;
        while (same < sent.length && same < now.length && sent[same] === now[same]) same++;
        for (var i = same; i < sent.length; i++) _OsWebText(8);
        for (var i = same; i < now.length; i++) _OsWebText(now[i].codePointAt(0));
        field.sent = field.value;
        bar.classList.remove('on');
    });

    // Keys without text (Escape, arrows, Tab, Shift, F-keys) from a Bluetooth keyboard go to the canvas handlers.
    function forward(e) {
        var copy = new KeyboardEvent(e.type, {
            key: e.key, code: e.code, location: e.location, repeat: e.repeat, bubbles: true, cancelable: true,
            ctrlKey: e.ctrlKey, shiftKey: e.shiftKey, altKey: e.altKey, metaKey: e.metaKey
        });
        if (!canvas.dispatchEvent(copy)) e.preventDefault();
    }

    field.addEventListener('keydown', function(e) {
        if (e.isComposing || e.keyCode === 229) return;
        if (e.key === 'Enter') {
            e.preventDefault();
            _OsWebText(13);
            clear();
        } else if (e.key === 'Backspace') {
            // With text in the field, the input event sends it.
            if (!field.value) {
                e.preventDefault();
                _OsWebText(8);
            }
        } else if (e.key.length > 1 && e.key !== 'Unidentified' && e.key !== 'Process') {
            forward(e);
        }
    });

    field.addEventListener('keyup', function(e) {
        if (e.key.length > 1 && e.key !== 'Enter' && e.key !== 'Backspace' && e.key !== 'Unidentified' && e.key !== 'Process') {
            forward(e);
        }
    });

    field.addEventListener('blur', function() {
        clear();
        update();
    });

    // A real tap, so iOS opens the keyboard.
    bar.addEventListener('click', function() {
        field.focus({ preventScroll: true });
        update();
    });

    window.wowTextFocus = function(enable) {
        field.want = enable;
        field.settled = false;
        clearTimeout(field.timer);
        clear();

        if (enable) {
            field.focus({ preventScroll: true });
            field.timer = setTimeout(function() {
                field.settled = true;
                update();
            }, 400);
        } else if (document.activeElement === field) {
            canvas.focus();
        }

        update();
    };

    // Android's back button (index.html).
    window.wowKey = function(code) {
        _OsWebText(code);
    };
});

EM_JS(void, WebTextFocus, (int enable), {
    if (window.wowTextFocus) window.wowTextFocus(!!enable);
});

EM_JS(void, WebTextSetType, (int secure), {
    var field = document.getElementById('wow-text');
    if (field) field.type = secure ? 'password' : 'text';
});

static void RunWebTextSetup(void*) {
    WebTextSetup();
}

static void RunWebTextFocus(void* enable) {
    WebTextFocus(enable != nullptr);
}

static void RunWebTextSetType(void* secure) {
    WebTextSetType(secure != nullptr);
}

static void RunOnMainThread(void (*func)(void*), int32_t arg) {
    emscripten_proxy_async(emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(), func, arg ? reinterpret_cast<void*>(1) : nullptr);
}

// Called by OsInputEnableSecureMode (Input.cpp). A password box doesn't call OsIMEEnable, so this also opens and closes
// the keyboard for it.
void OsWebTextSecure(int32_t secure) {
    RunOnMainThread(RunWebTextSetType, secure);
    OsIMEEnable(secure);
}

int32_t OsIMEGetLanguageMode() {
    return 0;
}

int32_t OsIMEGetCompositionString(char* buffer, uint32_t bufferSize) {
    memset(buffer, 0, bufferSize);
    return 0;
}

int32_t OsIMEGetCompositionResult(char* buffer, uint32_t bufferSize) {
    memset(buffer, 0, bufferSize);
    return 0;
}

int32_t OsIMEGetClauseInfo(uint32_t& clauseStart, uint32_t& clauseEnd, uint32_t& cursor) {
    return 0;
}

// The phone keyboard only opens after a touch; with a mouse, keys keep going to the canvas.
void OsIMEEnable(int32_t enable) {
    if (enable && !OsWebTouchHidesCursor()) {
        return;
    }

    RunOnMainThread(RunWebTextFocus, enable);
}

void OsIMESync() {
}

void OsIMESetLanguageMode(int32_t native) {
}

void OsIMEInitialize() {
    RunOnMainThread(RunWebTextSetup, 0);
}

void OsIMEDestroy() {
}

int32_t OsIMEGetCandidates(uint32_t listMask, uint32_t& pageStart, uint32_t& pageSize, uint32_t& count, uint32_t& selection, TSGrowableArray<OsIMECandidate>& candidates) {
    candidates.Clear();
    return 0;
}

#endif
