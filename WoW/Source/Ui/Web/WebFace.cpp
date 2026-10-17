#if defined(WOWDECOMP_PLATFORM_WEB)

// WebFaceTarget() for the touch HUD (docs/mobile-plan.md): steering with a thumb is too coarse to keep the target in
// front, so the HUD turns the player to face the target before it casts. The turn is the one the mouse and the arrow
// keys make (CGUnit_C::OnSetRawFacingLocal, as CGInputControl::CameraTurnPlayer and CGUnit_C::TrackingSetTurn do),
// which queues MSG_MOVE_SET_FACING; the movement tick is then run at once so the packet leaves before the cast does.

#include <cmath>
#include <cstdint>
#include "Engine/Source/FrameScript/FrameScript.h"
#include "Engine/Source/lua-5.1/src/lua.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Tempest/CMath.h"
#include "WoW/Source/Object/Movement.h"
#include "WoW/Source/Object/ObjectClient/Movement_C.h"
#include "WoW/Source/Object/ObjectClient/Player_C.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/UIUtil/InputControl.h"
#include "WoW/Source/Ui/GameUI.h"

namespace {

// Facings closer than this aren't sent anyway (CGUnit_C::IsFacingUnchangedSinceSent).
const float kFacingTolerance = 0.1f;

// WebFaceTarget(): true if the player turned to face their target, false if there was nothing to do.
int32_t Script_WebFaceTarget(lua_State* L) {
    CGPlayer_C* player = ClntObjMgrGetActivePlayerObj();
    WOWGUID targetGuid = CGGameUI::m_lockedTarget;
    CGObject_C* target = player && targetGuid != player->m_obj->m_guid ? ClntObjMgrObjectPtr(targetGuid, TYPEMASK_OBJECT, __FILE__, __LINE__) : nullptr;

    // The active mover isn't the player while they drive a vehicle or mind control a unit. CanTurn refuses the dead,
    // the stunned, a taxi or any other spline, and a vehicle seat that can't turn.
    if (!target || player->m_obj->m_guid != CGUnit_C::s_activeMover || !CGInputControl::CanTurn(player)) {
        lua_pushboolean(L, false);
        return 1;
    }

    C3Vector from = player->GetPosition();
    C3Vector to = target->GetPosition();
    float facing = CalculateFacingTo(&from, &to);
    float delta = CMath::normalizeangle0to2pi_(facing - player->GetFacing());

    if (delta < kFacingTolerance || delta > 6.2831855f - kFacingTolerance) {
        lua_pushboolean(L, false);
        return 1;
    }

    uint32_t now = OsGetAsyncTimeMs();
    player->OnSetRawFacingLocal(now, player->m_movement->GetRawFacing(facing));

    // Catch movement up to now, which sends the queued facing; if the last tick was this same millisecond there was no
    // time to catch up and the queue is run directly.
    MovementIdleMoveUnits(nullptr, nullptr);
    player->m_movement->UpdatePlayerMovement(now);

    lua_pushboolean(L, true);
    return 1;
}

} // namespace

void WebFaceInitialize() {
    FrameScript_RegisterFunction("WebFaceTarget", Script_WebFaceTarget);
}

#endif
