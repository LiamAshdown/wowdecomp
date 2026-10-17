#if defined(WOWDECOMP_PLATFORM_WEB)

// Web replacements for the Win32/WGL parts of CGxDeviceOpenGl.cpp: the "window" is the page's #canvas, transferred to
// the game pthread as an OffscreenCanvas, and the GL context is a WebGL2 context driven through GlShim. Frames are
// presented when the frame callback returns to the browser, so there is no swap call.

#include <atomic>
#include <cmath>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <cstdio>
#include <cstring>
#include "Engine/Source/Base/Coordinate.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/Gx/CGxDeviceOpenGL/CGxDeviceOpenGl.h"
#include "Engine/Source/Os/Input.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"

double OsWebPixelRatio();
int32_t OsWebTouchHidesCursor();
void OsWebSetCursorVisible(int32_t visible);
void OsWebSetWindowSize(int32_t width, int32_t height, bool notify);
extern std::atomic<int> g_webGxDumpRequest;

static EMSCRIPTEN_WEBGL_CONTEXT_HANDLE s_context;
static int32_t s_canvasWindow;
static int32_t s_frames;

// The drawing buffer is the canvas' CSS size in device pixels, times the page's render scale.
static void CanvasSize(int32_t& width, int32_t& height) {
    double cssWidth = 0.0;
    double cssHeight = 0.0;
    emscripten_get_element_css_size("#canvas", &cssWidth, &cssHeight);
    double ratio = OsWebPixelRatio();
    width = static_cast<int32_t>(std::lround(cssWidth * ratio));
    height = static_cast<int32_t>(std::lround(cssHeight * ratio));
    width = width > 0 ? width : 1280;
    height = height > 0 ? height : 720;
}

// The UI and glue scenes take their aspect ratio from gxResolution (Screen::GetAspectRatio, 4:3 unless widescreen is
// set), and native WoW keeps its window at that resolution. The canvas follows the page instead, so the resolution
// CVars follow the canvas. Not saved to Config.wtf.
static void SyncResolutionCVars(int32_t width, int32_t height) {
    char value[32];
    snprintf(value, sizeof(value), "%dx%d", width, height);

    if (CVar* widescreen = CVar::Lookup("widescreen")) {
        widescreen->Set("1", true, false, false, false);
    }

    CVar* resolution = CVar::Lookup("gxResolution");

    if (resolution) {
        // gxResolution is latched: a saved resolution waits there and CVar::Update (UpdateGxCVars) would apply it over
        // the canvas size, laying the UI out for the wrong shape.
        resolution->m_latchedValue.Copy(nullptr);
    }

    if (resolution && (!resolution->m_stringValue.m_str || strcmp(resolution->m_stringValue.m_str, value))) {
        // InternalSet skips the callback, which only accepts monitor modes and would queue a gxRestart.
        resolution->InternalSet(value, true, false, false, false);
    }
}

static void ApplyCanvasSize(CGxDeviceOpenGl* device, int32_t width, int32_t height, bool notify) {
    emscripten_set_canvas_element_size("#canvas", width, height);
    OsWebSetWindowSize(width, height, notify);
    SyncResolutionCVars(width, height);
    device->m_format.m_size.x = width;
    device->m_format.m_size.y = height;

    CRect rect;
    rect.minX = 0.0f;
    rect.minY = 0.0f;
    rect.maxX = static_cast<float>(width);
    rect.maxY = static_cast<float>(height);
    device->DeviceWM(GxWM_Size, reinterpret_cast<uintptr_t>(&rect), 0);
}

static int32_t AttachContext(CGxDeviceOpenGl* device, const CGxFormat& format) {
    if (!s_context) {
        EmscriptenWebGLContextAttributes attributes;
        emscripten_webgl_init_context_attributes(&attributes);
        attributes.majorVersion = 2;
        attributes.minorVersion = 0;
        attributes.alpha = EM_FALSE;
        attributes.depth = EM_TRUE;
        attributes.stencil = EM_TRUE;
        attributes.antialias = EM_FALSE;
        attributes.premultipliedAlpha = EM_FALSE;
        attributes.preserveDrawingBuffer = EM_FALSE;
        attributes.powerPreference = EM_WEBGL_POWER_PREFERENCE_HIGH_PERFORMANCE;

        s_context = emscripten_webgl_create_context("#canvas", &attributes);

        if (s_context <= 0) {
            CGxDevice::Log("WebGL2 context creation failed (%d)", static_cast<int32_t>(s_context));
            s_context = 0;
            return 0;
        }

        // The page's ?stats line (index.html) counts presented frames.
        MAIN_THREAD_ASYNC_EM_ASM({ window.wowFrameCount = function() { return HEAP32[$0 >> 2]; }; }, &s_frames);
    }

    emscripten_webgl_make_context_current(s_context);
    GlShimInitialize();

    device->m_hwnd = reinterpret_cast<HWND>(&s_canvasWindow);
    device->m_context = 1;
    device->m_windowFocus = 1;
    device->m_renderTargetMode = 1;

    int32_t width;
    int32_t height;
    CanvasSize(width, height);
    ApplyCanvasSize(device, width, height, false);

    device->ISetCaps(format);
    device->ICursorCreate(format);
    device->IStateSetContextDefaults();
    device->IRsSync(1);
    device->IShaderConstantsClear();
    device->m_glVendor = glGetString(GL_VENDOR);
    device->m_glRenderer = glGetString(GL_RENDERER);
    device->m_glVersion = glGetString(GL_VERSION);

    // The game draws its own (software) cursor.
    OsWebSetCursorVisible(0);

    return 1;
}

int32_t CGxDeviceOpenGl::DeviceCreate(GxWindowProc windowProc, const CGxFormat& format) {
    this->m_ownhwnd = 1;
    return CGxDevice::DeviceCreate(windowProc, format);
}

int32_t CGxDeviceOpenGl::DeviceCreate(void* window, const CGxFormat& format) {
    this->m_ownhwnd = 0;
    return AttachContext(this, format) && CGxDevice::DeviceCreate(window, format);
}

int32_t CGxDeviceOpenGl::DeviceSetFormat(const CGxFormat& format) {
    CGxDevice::Log("CGxDeviceOpenGl::DeviceSetFormat():");
    this->Log(format);

    if (!AttachContext(this, format)) {
        return 0;
    }

    // The drawing buffer is the canvas, whatever resolution was requested.
    CGxFormat canvasFormat = format;
    canvasFormat.m_window = 1;
    canvasFormat.m_size = this->m_format.m_size;

    if (!CGxDevice::DeviceSetFormat(canvasFormat)) {
        return 0;
    }

    this->DeviceSetGamma(this->m_gammaRamp);
    return 1;
}

// The WebGL context lives as long as the page.
void CGxDeviceOpenGl::DeviceDestroy() {
    CGxDevice::DeviceDestroy();
}

// No hardware gamma ramps in a browser.
void CGxDeviceOpenGl::DeviceSetGamma(float gamma) {
    CGxDevice::DeviceSetGamma(gamma);
}

void CGxDeviceOpenGl::DeviceSetGamma(const CGxGammaRamp& ramp) {
    CGxDevice::DeviceSetGamma(ramp);
}

int32_t CGxDeviceOpenGl::DeviceWM(EGxWM wm, uintptr_t param1, uintptr_t param2) {
    switch (wm) {
        case GxWM_Size:
            if (param2 == 1 || param2 == 2) {
                this->m_windowVisible = 0;
            } else {
                this->m_windowVisible = 1;
                this->DeviceSetDefWindow(*reinterpret_cast<const CRect*>(param1));
                this->m_viewportDirty = 1;
            }

            break;

        case GxWM_DisplayChange:
            this->DeviceSetDefWindow(*reinterpret_cast<const CRect*>(param1));
            this->m_viewportDirty = 1;
            break;

        case GxWM_SetFocus:
            this->m_windowFocus = 1;
            break;

        case GxWM_Destroy:
        case GxWM_KillFocus:
            this->m_windowFocus = 0;
            break;

        default:
            break;
    }

    return 0;
}

void CGxDeviceOpenGl::CapsWindowSizeInScreenCoords(CRect& rect) {
    rect = this->DeviceCurWindow();
}

// Render-to-texture always uses FBOs on web (m_renderTargetMode = 1); pbuffers do not exist.
void CGxDeviceOpenGl::IDeviceSetRenderTargetCopy(EGxBuffer buffer, CGxTex* texture, uint32_t plane) {
    this->IDeviceSetRenderTargetFBO(buffer, texture, plane);
}

void CGxDeviceOpenGl::ScenePresent() {
    int32_t screenShot = this->m_scrShotClick;
    CGxDevice::ScenePresent();

    if (screenShot) {
        this->DeviceScreenShot();
    }

    if (!OsWebTouchHidesCursor()) {
        this->ICursorDraw();
    }

    // ponytail: poll the page size twice a second instead of a resize callback; the input layer also reports
    // OS_INPUT_SIZE for the UI.
    if (++s_frames % 30 == 0) {
        int32_t width;
        int32_t height;
        CanvasSize(width, height);
        const CRect& window = this->DeviceDefWindow();

        if (width != static_cast<int32_t>(window.maxX) || height != static_cast<int32_t>(window.maxY)) {
            ApplyCanvasSize(this, width, height, true);
        }
    }

    this->IShaderConstantsClear();
    bool dump = g_webGxDumpRequest.exchange(0) != 0;

    // The values the UI and world aspect come from, to compare with the page when the picture looks stretched.
    if (dump) {
        double cssWidth = 0.0;
        double cssHeight = 0.0;
        emscripten_get_element_css_size("#canvas", &cssWidth, &cssHeight);
        CVar* resolution = CVar::Lookup("gxResolution");
        CVar* widescreen = CVar::Lookup("widescreen");
        const CRect& window = this->DeviceDefWindow();
        printf("[Gx] css=%.0fx%.0f ratio=%.2f window=%.0fx%.0f format=%dx%d gxResolution=%s widescreen=%d coordAspect=%.3f\n",
            cssWidth, cssHeight, OsWebPixelRatio(), window.maxX, window.maxY, this->m_format.m_size.x,
            this->m_format.m_size.y, resolution ? resolution->m_stringValue.m_str : "?", widescreen ? widescreen->m_intValue : -1,
            CoordinateGetAspectRatio());
    }

    GlShimSetDump(dump);
}

#endif
