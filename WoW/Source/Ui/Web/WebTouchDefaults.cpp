#if defined(WOWDECOMP_PLATFORM_WEB)

// Stock gameplay settings that suit a phone (docs/mobile-plan.md), as the defaults while the webTouch CVar is 1:
// - autoInteract (Click-to-Move): a hold is a right click in touch mode, so a hold walks to the ground, an NPC or an
//   enemy and interacts with or attacks it.
// - cameraTerrainTilt (Follow Terrain): the camera pitches with the slopes, one less thing to drag.
// - nameplateShowEnemies: enemy nameplates are bigger tap targets than the units under them.
// These are account and character settings, which follow the account to the desktop, so it is their default that
// changes, not a saved value: the account data keeps only what differs from the default (CVar::IterateForArchive). A
// setting still at its default moves to the touch one; one the player set stays theirs, and setting it back to the
// stock value is saved as a change. cameraSmoothStyle needs nothing (its default, 4, follows only while moving), nor do
// autoDismount, autoUnshift, autoSelfCast and autoClearAFK (already on). autoDismountFlying stays off: a spell tapped in
// the air would drop the player.

#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/Storm/H/storm.h"

namespace {

const struct {
    const char* name;
    const char* value;
} kTouchDefaults[] = {
    { "autoInteract", "1" },
    { "cameraTerrainTilt", "1" },
    { "nameplateShowEnemies", "1" },
};

}

// Run after WebTouchInitialize registers webTouch, before the account data loads over the values.
void WebTouchDefaultsInitialize() {
    CVar* touch = CVar::LookupRegistered("webTouch");

    if (!touch || touch->m_intValue != 1) {
        return;
    }

    for (const auto& setting : kTouchDefaults) {
        CVar* var = CVar::LookupRegistered(setting.name);

        if (!var || !var->m_stringValue.m_str || !var->m_defaultValue.m_str) {
            continue;
        }

        bool unchanged = !SStrCmpI(var->m_stringValue.m_str, var->m_defaultValue.m_str, STORM_MAX_STR);
        var->m_defaultValue.Copy(setting.value);

        if (unchanged) {
            var->Default();
        }
    }
}

#endif
