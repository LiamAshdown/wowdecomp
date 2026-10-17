#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cstdint>
#include "Engine/Source/Os/Joystick.h"

// No gamepad support on the web yet (Gamepad API could back this later).

void OsJoystickClose(uint32_t index) {
}

void OsJoystickConfigureMouseMode(uint32_t index, int32_t stick, int32_t leftButton, int32_t rightButton, float speed) {
}

uint32_t OsJoystickCount() {
    return 0;
}

char* OsJoystickGetName(uint32_t index, char* buffer, uint32_t size) {
    if (buffer && size) {
        buffer[0] = '\0';
    }

    return buffer;
}

void OsJoystickMapHat(uint32_t index, int32_t hat, int32_t up, int32_t right, int32_t down, int32_t left) {
}

void OsJoystickMapSlider(uint32_t index, int32_t axis) {
}

void OsJoystickMapStick(uint32_t index, int32_t stick, int32_t xAxis, int32_t yAxis) {
}

uint32_t OsJoystickOpen(uint32_t index) {
    return 0xFFFFFFFF;
}

void OsJoystickSetMouseMode(uint32_t index, int32_t enable) {
}

void OsPollJoysticks(int32_t active) {
}

#endif
