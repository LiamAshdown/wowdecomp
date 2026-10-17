#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cstdint>
#include <cstring>
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"

// The browser exposes no adapter or monitor enumeration, so report one fixed adapter and desktop mode.
static const uint32_t s_webDesktopWidth = 1920;
static const uint32_t s_webDesktopHeight = 1080;

int32_t CGxDevice::AdapterFormats(EGxApi api, TSGrowableArray<CGxFormat>& formats) {
    formats.SetCount(0);
    formats.m_chunk = 64;
    formats.Reserve(256, 0);

    static const CGxFormat::Format colorFormats[2] = { CGxFormat::Fmt_ArgbX888, CGxFormat::Fmt_Argb8888 };
    static const CGxFormat::Format depthFormats[2] = { CGxFormat::Fmt_Ds24X, CGxFormat::Fmt_Ds248 };

    for (uint32_t color = 0; color < 2; color++) {
        for (uint32_t depth = 0; depth < 2; depth++) {
            CGxFormat format;
            memset(&format, 0, sizeof(format));
            format.m_colorFormat = colorFormats[color];
            format.m_depthFormat = depthFormats[depth];
            format.m_multisampleCount = 1;

            if (!CGxDevice::FormatExists(formats, format)) {
                *formats.New() = format;
            }
        }
    }

    return formats.Count() != 0;
}

int32_t CGxDevice::DeviceAdapterID(uint16_t& vendorID, uint16_t& deviceID, uint32_t& driverVersionHi, uint32_t& driverVersionLo) {
    // Unknown vendor makes the caller fall back to DeviceAdapterInfer.
    vendorID = 0xFFFF;
    deviceID = 0xFFFF;
    driverVersionHi = 0;
    driverVersionLo = 0;

    return 0;
}

int32_t CGxDevice::DeviceAdapterInfer(uint16_t& deviceID) {
    // Hardware T&L with multitexture, no pixel shaders (the web renderer starts out fixed-function).
    deviceID = 1;

    return 1;
}

int32_t CGxDevice::AdapterDesktopMode(CGxMonitorMode& mode) {
    mode.m_size.x = s_webDesktopWidth;
    mode.m_size.y = s_webDesktopHeight;
    mode.m_refreshRate = 60;
    mode.m_bpp = 32;

    return 1;
}

int32_t CGxDevice::AdapterMonitorModes(TSGrowableArray<CGxMonitorMode>& modes) {
    static const uint32_t sizes[][2] = { { 800, 600 }, { 1024, 768 }, { 1280, 720 }, { 1280, 1024 }, { 1366, 768 }, { 1600, 900 }, { 1920, 1080 } };

    modes.SetCount(0);

    for (uint32_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        CGxMonitorMode* mode = modes.New();
        mode->m_size.x = sizes[i][0];
        mode->m_size.y = sizes[i][1];
        mode->m_bpp = 32;
        mode->m_refreshRate = 60;
    }

    return 1;
}

#endif
