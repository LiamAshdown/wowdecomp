#if defined(WOWDECOMP_PLATFORM_WEB)

// The touch HUD for the browser build on phones and tablets (docs/mobile-plan.md, T3). The controls are the game's own
// frames: the Lua below, run right after FrameXML loads and before any addon, moves the stock action buttons into an arc
// under the right thumb and adds a movement stick, a smart button, a pull-down drawer of the micro buttons and a folded
// chat, drawn with the stock button borders and dialog art plus Interface\TouchUI (Tools/Bridge/make-touch-art.py). The
// screen shows what the moment needs: exploring, the arc folds away and the minimap grows; in combat the arc opens.
// Because it runs as Blizzard code it can call the protected movement, targeting and casting functions.
//
// Fingers are routed in Os/Web/Input.cpp: the Lua reports the screen rects it owns (WebTouchSetZone), a finger landing in
// one drives the stick or presses that button, and any other finger is the mouse cursor. The HUD only exists while the
// webTouch CVar is 1, which defaults to 1 on a touch screen (index.html).

#include <cstdint>
#include <cstring>
#include <emscripten.h>
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/Event/Types.h"
#include "Engine/Source/Frame/CSimpleTop.h"
#include "Engine/Source/FrameScript/FrameScript.h"
#include "Engine/Source/lua-5.1/src/lua.h"
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/Ui/WorldFrame.h"

void OsWebTouchSetZone(int32_t id, int32_t kind, float left, float bottom, float right, float top);
void OsWebTouchClearZones();
void OsWebTouchGetStick(bool* active, float* x, float* y, float* originX, float* originY);
int32_t OsWebTouchNextEvent(int32_t* id, int32_t* down, int32_t* outside);
void OsWebTouchGetZoneDrag(int32_t id, float* dx, float* dy, float* ms);
void OsWebTouchMoveCursor(float x, float y);
void OsWebTouchClickAt(float x, float y);
void OsWebTouchSetDragProc(int32_t (*proc)());
void OsWebTouchSetTuning(double holdMs, double cameraSpeed);
void WebFaceInitialize();
void WebTouchPanelsInitialize();
void WebQuickChatInitialize();

namespace {

// A drag that starts on the world turns the camera and the character (right button); on a frame it picks up or moves
// it (left button).
int32_t TouchDragButton() {
    CSimpleFrame* focus = CSimpleTop::s_instance ? CSimpleTop::s_instance->m_mouseFocus : nullptr;
    CSimpleFrame* world = CGWorldFrame::s_currentWorldFrame;
    return world && (!focus || focus == world) ? MOUSE_BUTTON_RIGHT : MOUSE_BUTTON_LEFT;
}

// WebTouchSetZone(id, kind, left, bottom, right, top): kind "stick", "button" or "type" (a button that opens the phone
// keyboard), the rect in fractions of the screen with y up; a nil kind removes the zone.
int32_t Script_WebTouchSetZone(lua_State* L) {
    const char* kind = lua_isstring(L, 2) ? lua_tolstring(L, 2, nullptr) : nullptr;
    int32_t code = !kind ? 0 : !strcmp(kind, "stick") ? 1 : !strcmp(kind, "button") ? 2 : !strcmp(kind, "type") ? 3 : 0;

    OsWebTouchSetZone(static_cast<int32_t>(lua_tonumber(L, 1)), code, static_cast<float>(lua_tonumber(L, 3)),
        static_cast<float>(lua_tonumber(L, 4)), static_cast<float>(lua_tonumber(L, 5)), static_cast<float>(lua_tonumber(L, 6)));
    return 0;
}

// x, y, active, originX, originY = WebTouchGetStick(): the deflection (-1..1, y up) and where the thumb landed
// (fractions of the screen).
int32_t Script_WebTouchGetStick(lua_State* L) {
    bool active;
    float x, y, originX, originY;
    OsWebTouchGetStick(&active, &x, &y, &originX, &originY);

    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    lua_pushboolean(L, active);
    lua_pushnumber(L, originX);
    lua_pushnumber(L, originY);
    return 5;
}

// id, down, outside = WebTouchNextEvent(): the next button zone press or release, or nil. outside: the finger was lifted
// off the zone, not on it.
int32_t Script_WebTouchNextEvent(lua_State* L) {
    int32_t id;
    int32_t down;
    int32_t outside;

    if (!OsWebTouchNextEvent(&id, &down, &outside)) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushnumber(L, id);
    lua_pushboolean(L, down);
    lua_pushboolean(L, outside);
    return 3;
}

// dx, dy, ms = WebTouchZoneDrag(id): how far the finger on a button zone has moved since it pressed, in CSS px with y
// up, and how long it has been (or was) down, in ms.
int32_t Script_WebTouchZoneDrag(lua_State* L) {
    float dx, dy, ms;
    OsWebTouchGetZoneDrag(static_cast<int32_t>(lua_tonumber(L, 1)), &dx, &dy, &ms);

    lua_pushnumber(L, dx);
    lua_pushnumber(L, dy);
    lua_pushnumber(L, ms);
    return 3;
}

// WebTouchMoveCursor(x, y) and WebTouchClickAt(x, y): the cursor to, or a left click at, a point in fractions of the
// screen with y up.
int32_t Script_WebTouchMoveCursor(lua_State* L) {
    OsWebTouchMoveCursor(static_cast<float>(lua_tonumber(L, 1)), static_cast<float>(lua_tonumber(L, 2)));
    return 0;
}

int32_t Script_WebTouchClickAt(lua_State* L) {
    OsWebTouchClickAt(static_cast<float>(lua_tonumber(L, 1)), static_cast<float>(lua_tonumber(L, 2)));
    return 0;
}

// WebSpellIsGround(spellId): whether the spell is cast at a point on the ground (Spell.dbc targets flag 0x40, the
// destination location), which puts the game in spell targeting with the circle under the cursor.
int32_t Script_WebSpellIsGround(lua_State* L) {
    SpellRec spell;
    bool ground = lua_isnumber(L, 1) && g_spellDB.GetRecord(static_cast<int32_t>(lua_tonumber(L, 1)), &spell) && (spell.m_targets & 0x40);

    lua_pushboolean(L, ground);
    return 1;
}

// WebTouchSetTuning(hold, camera): the hold (s) that makes a tap a right click, and the camera drag speed (1 = the
// finger's own).
int32_t Script_WebTouchSetTuning(lua_State* L) {
    OsWebTouchSetTuning(lua_tonumber(L, 1) * 1000.0, lua_tonumber(L, 2));
    return 0;
}

// cssWidth, cssHeight, insetLeft, insetRight, insetTop, insetBottom = WebTouchScreen(): the canvas size and the
// safe-area insets (the notch), in CSS pixels.
int32_t Script_WebTouchScreen(lua_State* L) {
    for (int32_t i = 0; i < 6; i++) {
        lua_pushnumber(L, MAIN_THREAD_EM_ASM_DOUBLE({
            var rect = document.getElementById("canvas").getBoundingClientRect();
            if ($0 < 2) return $0 ? rect.height : rect.width;
            var safe = document.getElementById("safe");
            if (!safe) return 0;
            var style = getComputedStyle(safe);
            return parseFloat([style.paddingLeft, style.paddingRight, style.paddingTop, style.paddingBottom][$0 - 2]) || 0;
        }, i));
    }

    return 6;
}

const char kTouchUi[] = R"lua(
if GetCVar("webTouch") ~= "1" then return end

local ART = "Interface\\TouchUI\\"
local STICK_TRAVEL = 56   -- CSS px, as kStickRadius in Input.cpp
local HOLD = 0.45         -- s a button is held before it shows its tooltip instead of acting on release (Touch options)
local SWIPE = 30          -- CSS px a finger moves on a button before the release counts as a swipe
local FLICK = 0.3         -- s within which a swipe on an arc button is a flick (slower, it is a drag off to cancel)
local AIM_START = 18      -- CSS px a ground spell's button is dragged before it starts aiming
local AIM_REACH = 2.2     -- screen distance the targeting circle moves per CSS px of drag
local AIM_FROM_X, AIM_FROM_Y = 0.5, 0.32  -- about where the character's feet are on screen (fractions), behind the camera
local AIM_TOP = 0.62      -- higher up the screen is mostly past a spell's 30-40 yd range

-- Captured now, so nothing an addon puts in the globals later can stand in for them.
-- (The movement functions are captured in MOVES. Lua 5.1 allows this chunk at most 200 locals at a time, so parts that
-- are only built once sit in do ... end blocks.)
local JumpOrAscendStart, AscendStop, SitStandOrDescendStart, DescendStop = JumpOrAscendStart, AscendStop, SitStandOrDescendStart, DescendStop
local TargetNearestEnemy, InteractUnit, ToggleAutoRun, AttackTarget = TargetNearestEnemy, InteractUnit, ToggleAutoRun, AttackTarget
local CameraZoomIn, CameraZoomOut, SpellIsTargeting, SpellStopTargeting = CameraZoomIn, CameraZoomOut, SpellIsTargeting, SpellStopTargeting
local GetActionInfo, HasAction, GetSpellInfo, IsHarmfulSpell, GetTime = GetActionInfo, HasAction, GetSpellInfo, IsHarmfulSpell, GetTime
local UnitExists, UnitIsDead, UnitCanAttack, UnitIsUnit, UnitGUID, ClearTarget = UnitExists, UnitIsDead, UnitCanAttack, UnitIsUnit, UnitGUID, ClearTarget
local WebTouchSetZone, WebTouchGetStick, WebTouchNextEvent, WebTouchScreen = WebTouchSetZone, WebTouchGetStick, WebTouchNextEvent, WebTouchScreen
local WebTouchZoneDrag, WebTouchMoveCursor, WebTouchClickAt, WebSpellIsGround = WebTouchZoneDrag, WebTouchMoveCursor, WebTouchClickAt, WebSpellIsGround
local WebFaceTarget = WebFaceTarget   -- Ui/Web/WebFace.cpp
local WebTouchSetTuning, IsFlying, IsSwimming = WebTouchSetTuning, IsFlying, IsSwimming
local InCombatLockdown, UnitHasVehicleUI = InCombatLockdown, UnitHasVehicleUI
local sqrt, abs, cos, sin, rad, max, min, random = math.sqrt, math.abs, math.cos, math.sin, math.rad, math.max, math.min, math.random
local floor, format = math.floor, string.format

local hud = CreateFrame("Frame", nil, UIParent)
hud:SetAllPoints(UIParent)
hud:SetFrameStrata("MEDIUM")

-- Stock frames the touch layout has no room for live here: a hidden parent keeps them hidden whatever shows them.
local hidden = CreateFrame("Frame")
hidden:Hide()

-- Screen metrics, refreshed by Layout: UIParent's size in its own units, UI units per CSS pixel, the canvas in CSS px
-- and the safe-area insets in UI units.
local W, H, unit, cssW, cssH = 1, 1, 1, 1, 1
local insetL, insetR, insetT, insetB = 0, 0, 0, 0

-- The HUD's state: in combat, the arc opened or closed by hand ("open", "closed" or nil), the arc's page, the chat
-- opened up.
local combat, forced, arcPage, chatOpen = false, nil, 1, false

-- The Touch options (Interface Options > Touch, below), kept per browser in Config.wtf: button and stick size,
-- left-handed, hold time and camera speed.
local cfg = {}
local function ReadSettings()
    local function num(name, default, lo, hi)
        return max(lo, min(hi, tonumber(GetCVar(name)) or default))
    end
    cfg.buttons = num("webTouchButtons", 100, 70, 140) / 100
    cfg.stick = num("webTouchStick", 100, 70, 140) / 100
    cfg.left = GetCVar("webTouchLeft") == "1"
    cfg.hold = num("webTouchHold", 450, 250, 900) / 1000
    cfg.camera = num("webTouchCamera", 100, 40, 250) / 100
    HOLD = cfg.hold
    WebTouchSetTuning(cfg.hold, cfg.camera)
end
ReadSettings()

-- Centres frame at (x, y) UIParent units from the bottom left, size units across, whatever its parent's scale.
local function Place(frame, x, y, size)
    local parentScale = frame:GetParent():GetEffectiveScale() / UIParent:GetEffectiveScale()
    frame:SetScale(size / frame:GetWidth() / parentScale)
    local s = frame:GetEffectiveScale() / UIParent:GetEffectiveScale()
    frame:ClearAllPoints()
    frame:SetPoint("CENTER", UIParent, "BOTTOMLEFT", x / s, y / s)
end

-- A button in the stock action-button style: the icon, the quickslot border, the pushed and highlight art.
local function ThumbButton(icon, parent)
    local b = CreateFrame("Button", nil, parent or hud)
    b:SetWidth(36)
    b:SetHeight(36)
    b.icon = b:CreateTexture(nil, "BACKGROUND")
    b.icon:SetTexture(icon:find("\\") and icon or ART .. icon)
    b.icon:SetTexCoord(0.06, 0.94, 0.06, 0.94)
    b.icon:SetAllPoints()
    local border = b:CreateTexture(nil, "ARTWORK")
    border:SetTexture("Interface\\Buttons\\UI-Quickslot2")
    border:SetWidth(66)
    border:SetHeight(66)
    border:SetPoint("CENTER", 0, -1)
    b:SetPushedTexture("Interface\\Buttons\\UI-Quickslot-Depress")
    b:SetHighlightTexture("Interface\\Buttons\\ButtonHilight-Square", "ADD")
    return b
end

local function ArcPoint(px, py, r, deg)
    return px + r * cos(rad(deg)), py + r * sin(rad(deg))
end

-- The arc's pivot (as for the right hand) and size, set by Layout. Left-handed, everything mirrors to the other edge.
local arcX, arcY, f = 0, 0, 1
local function MX(x)
    return cfg.left and W - x or x
end
local function ArcAt(r, deg)
    local x, y = ArcPoint(arcX, arcY, r * f, deg)
    return MX(x), y
end
-- A distance in from the stick's edge of the screen: the left, or the right when left-handed.
local function EdgeX(d)
    return cfg.left and W - insetR - d or insetL + d
end

-- ---- Stick ----

-- The minimap's own bezel (UI-Minimap-Border, cropped to the ring without its zone-name bar) around the minimap's dark
-- disc, and a knob made like a minimap tracking button.
local stick = CreateFrame("Frame", nil, hud)
stick:SetWidth(150)
stick:SetHeight(150)
local knob
do
local well = stick:CreateTexture(nil, "BACKGROUND")
well:SetTexture("Interface\\Minimap\\UI-Minimap-Background")
well:SetVertexColor(1, 1, 1, 0.55)
well:SetWidth(128)
well:SetHeight(128)
well:SetPoint("CENTER")
local ring = stick:CreateTexture(nil, "BORDER")
ring:SetTexture("Interface\\Minimap\\UI-Minimap-Border")
ring:SetTexCoord(92 / 256, 244 / 256, 28 / 256, 180 / 256)
ring:SetAllPoints()
knob = CreateFrame("Frame", nil, stick)
knob:SetWidth(64)
knob:SetHeight(64)
knob:SetPoint("CENTER")
local knobFill = knob:CreateTexture(nil, "ARTWORK")
knobFill:SetTexture("Interface\\Minimap\\UI-Minimap-Background")
knobFill:SetWidth(50)
knobFill:SetHeight(50)
knobFill:SetPoint("CENTER")
local knobBorder = knob:CreateTexture(nil, "OVERLAY")
knobBorder:SetTexture("Interface\\Minimap\\MiniMap-TrackingBorder")
knobBorder:SetTexCoord(2 / 64, 38 / 64, 2 / 64, 38 / 64)
knobBorder:SetAllPoints()
end

local stickRestX, stickRestY = 0, 0
local moving = {}
local MOVES = {
    forward = { MoveForwardStart, MoveForwardStop },
    back = { MoveBackwardStart, MoveBackwardStop },
    left = { StrafeLeftStart, StrafeLeftStop },
    right = { StrafeRightStart, StrafeRightStop },
}

local function Move(dir, on)
    if (moving[dir] or false) ~= on then
        moving[dir] = on
        MOVES[dir][on and 1 or 2]()
    end
end

-- Eight directions out of the stick, relative to the camera (a camera drag also turns the character). At rest the stick
-- is faint: it is there to be found, not to be looked at.
local function UpdateStick()
    local x, y, active, ox, oy = WebTouchGetStick()
    local live = active and sqrt(x * x + y * y) > 0.25
    Move("forward", live and y > 0.38)
    Move("back", live and y < -0.38)
    Move("left", live and x < -0.38)
    Move("right", live and x > 0.38)

    if not active and not stick.moved then return end
    stick.moved = active
    local cx, cy = stickRestX, stickRestY
    if active then
        cx, cy = ox * W, oy * H
    end
    local s = stick:GetScale()
    stick:ClearAllPoints()
    stick:SetPoint("CENTER", UIParent, "BOTTOMLEFT", cx / s, cy / s)
    local travel = STICK_TRAVEL * unit / s
    knob:SetPoint("CENTER", stick, "CENTER", active and x * travel or 0, active and y * travel or 0)
    stick:SetAlpha(active and 1 or 0.35)
end

-- ---- Button zones ----
-- id -> { frame, alt, group, resolve, tap, press, release, tip, ground, swipe }. Zone 1 is the stick; 100 and 101 are
-- pinch steps (Input.cpp). A zone acts on release (tap), or on press and release (press, release); held still past HOLD
-- it shows a tooltip (tip) and does nothing on release; lifted off the button it does nothing; moved past SWIPE and let
-- go it swipes (swipe). A ground spell (ground) aims while it is dragged and goes down where the thumb lets go. Buttons
-- are matched in id order, so the plate behind the arc, added last, only gets what no button takes.

local zones = {}
local nextZone = 2

local function AddZone(zone)
    zones[nextZone] = zone
    nextZone = nextZone + 1
    return zone
end

-- The action slot behind a button (the stock code keeps it in .action), and the spell in it, if it is one.
local function Slot(b)
    return b.action or ActionButton_GetPagedID(b)
end

local function ActionSpell(b)
    local slot = Slot(b)
    if not slot then return end
    local kind, _, _, spellId = GetActionInfo(slot)
    if kind == "spell" then return spellId end
end

-- Before a tapped spell goes off: a harmful one with nothing to hit takes the nearest enemy, and turns to face it.
local function PrepareCast(b)
    local spellId = ActionSpell(b)
    local name = spellId and GetSpellInfo(spellId)
    if not name or not IsHarmfulSpell(name) then return end
    if not UnitExists("target") or UnitIsDead("target") or not UnitCanAttack("player", "target") then
        TargetNearestEnemy()
    end
    if WebFaceTarget and UnitExists("target") then
        WebFaceTarget()
    end
end

local function CastAction(b)
    PrepareCast(b)
    b:Click()
end

local function ActionTip(b)
    GameTooltip:SetOwner(b, "ANCHOR_LEFT")
    GameTooltip:SetAction(Slot(b))
    GameTooltip:Show()
end

local function GroundAction(b)
    local spellId = ActionSpell(b)
    return spellId and WebSpellIsGround(spellId)
end

local function LabelTip(text)
    return function(b)
        GameTooltip:SetOwner(b, "ANCHOR_LEFT")
        GameTooltip:SetText(text)
        GameTooltip:Show()
    end
end

-- Aim point for a dragged ground spell: out from the character as far and in the direction the thumb has moved.
local function AimPoint(dx, dy)
    local x = AIM_FROM_X + dx * AIM_REACH / cssW
    local y = AIM_FROM_Y + dy * AIM_REACH / cssH
    return max(0.08, min(0.92, x)), max(0.04, min(AIM_TOP, y))
end

-- While a spell is aimed the chat lets the cursor through to the world, which only moves the targeting circle while
-- the cursor is over it.
local function ChatTakesMouse(on)
    for i = 1, NUM_CHAT_WINDOWS do
        local chat = _G["ChatFrame" .. i]
        if chat then chat:EnableMouse(on) end
    end
end

local pressing = {}   -- zone id -> { b, t, tip, aiming, done, x, y }
local stopTargetingAt

local function ZoneDown(id)
    local zone = zones[id]
    local b = zone.resolve and zone.resolve() or zone.frame
    pressing[id] = { b = b, t = GetTime() }
    if b.SetButtonState then b:SetButtonState("PUSHED") end
    if zone.press then zone.press(b) end
end

local function ZoneHeld(id, p)
    local zone = zones[id]
    if p.done or p.tip then return end
    local dx, dy, ms = WebTouchZoneDrag(id)
    if p.aiming then
        p.x, p.y = AimPoint(dx, dy)
        WebTouchMoveCursor(p.x, p.y)
        return
    end
    local far = dx * dx + dy * dy > AIM_START * AIM_START
    if far and zone.ground and ms > FLICK * 1000 and zone.ground(p.b) then
        GameTooltip:Hide()
        zone.tap(p.b)
        p.aiming = SpellIsTargeting()
        p.done = not p.aiming
        if p.aiming then ChatTakesMouse(false) end
    elseif not far and zone.tip and ms > HOLD * 1000 then
        zone.tip(p.b)
        p.tip = true
    end
end

local function ZoneUp(id, outside)
    local zone = zones[id]
    local p = pressing[id]
    pressing[id] = nil
    if not p then return end
    if p.b.SetButtonState then p.b:SetButtonState("NORMAL") end
    if zone.release then zone.release(p.b) end
    local dx, dy, ms = WebTouchZoneDrag(id)
    if p.aiming then
        -- Let go away from the button: the spell goes down there. Back on the button: cancelled.
        if outside and SpellIsTargeting() then
            WebTouchClickAt(p.x, p.y)
            -- Out of range or not on the ground: targeting is still on. Drop it rather than leave the thumb stuck.
            stopTargetingAt = GetTime() + 0.3
        else
            SpellStopTargeting()
        end
        WebTouchMoveCursor(0.5, 1)
        ChatTakesMouse(true)
    elseif p.tip then
        GameTooltip:Hide()
    elseif zone.flick and not p.done and dx * dx + dy * dy > SWIPE * SWIPE and ms < FLICK * 1000 then
        zone.flick(dx, dy)
    elseif zone.swipe and not p.done and dx * dx + dy * dy > SWIPE * SWIPE then
        zone.swipe(dx, dy)
    elseif not p.done and not outside and zone.tap then
        zone.tap(p.b)
    end
end

-- ---- The arc ----
-- Seven places under the right thumb: the pivot (the main button) and two rings, all inside the plate (it spans 85 to 220
-- degrees, r 40 to 205). A flick across the arc, on any of its buttons or the plate, turns the page: page 1 is the main
-- bar's 1-7, page 2 its 8-12, pages 3 and 4 the second bar's 1-7 and 8-12, and pages with nothing on them are skipped. A
-- flick up opens the arc by hand, down closes it (out of combat). A page's buttons are shown by alpha alone (and their
-- zones), so the arc can open, close and turn in combat, when the stock buttons themselves can't be moved, shown or hidden.

local PLACES = { { 0, 0, 62 }, { 100, 108, 48 }, { 100, 140, 48 }, { 100, 172, 48 }, { 165, 129, 48 }, { 165, 154, 48 }, { 165, 179, 48 } }
local SMART_PLACE, JUMP_PLACE, MOUNT_PLACE, DESCEND_PLACE = { 165, 104 }, { 100, 204 }, { 100, 108 }, { 165, 196 }
local UpdateArc, ArcFlick
local arc = {}   -- { frame, alt, page, place }

local function AddArc(frame, alt, page, place)
    local entry = { frame = frame, alt = alt, page = page, place = place }
    arc[#arc + 1] = entry
    entry.zone = AddZone({ frame = frame, alt = alt, group = "arc", page = page, tap = CastAction, tip = ActionTip, ground = GroundAction,
        flick = function(dx, dy) ArcFlick(dx, dy) end,
        resolve = function()
            if alt and BonusActionBarFrame:IsShown() and alt:IsVisible() then return alt end
            return frame
        end })
end

for i = 1, 12 do
    local page, place = i <= 7 and 1 or 2, i <= 7 and i or i - 7
    AddArc(_G["ActionButton" .. i], _G["BonusActionButton" .. i], page, place)
end
for i = 1, 12 do
    local page, place = i <= 7 and 3 or 4, i <= 7 and i or i - 7
    AddArc(_G["MultiBarBottomLeftButton" .. i], nil, page, place)
end

local function PageHasActions(page)
    for _, entry in ipairs(arc) do
        if entry.page == page and HasAction(Slot(entry.frame) or 0) then return true end
    end
end

-- The arc is open in combat, with something attackable targeted, or when opened by hand.
local function ArcOpen()
    return combat or forced == "open"
        or (forced ~= "closed" and UnitExists("target") and UnitCanAttack("player", "target") and not UnitIsDead("target"))
end

-- The backing plate (ArcPlate: a 288-unit square in the corner, the pivot 80 units in), and its page pips, the stock
-- radio buttons.
local plate = CreateFrame("Frame", nil, hud)
plate:SetFrameStrata("LOW")
plate:SetWidth(288)
plate:SetHeight(288)
local plateTex = plate:CreateTexture(nil, "BACKGROUND")
plateTex:SetTexture(ART .. "ArcPlate")
plateTex:SetAllPoints()
local gryphon = plate:CreateTexture(nil, "ARTWORK")
gryphon:SetTexture("Interface\\MainMenuBar\\UI-MainMenuBar-EndCap-Dwarf")
gryphon:SetWidth(128)
gryphon:SetHeight(128)
gryphon:SetPoint("BOTTOMRIGHT", plate, "BOTTOMRIGHT", -172, -12)
local pips = {}
for i = 1, 4 do
    local pip = hud:CreateTexture(nil, "OVERLAY")
    pip:SetTexture("Interface\\Buttons\\UI-RadioButton")
    pip:SetWidth(12)
    pip:SetHeight(12)
    pips[i] = pip
end

-- ---- Thumb buttons: jump, the smart button, mount ----

local jump = ThumbButton("Interface\\Icons\\Spell_Magic_FeatherFall")
AddZone({ frame = jump, group = "always", press = JumpOrAscendStart, release = AscendStop })
jump:SetScript("OnMouseDown", JumpOrAscendStart)
jump:SetScript("OnMouseUp", AscendStop)

-- Descend: only while flying or swimming, where Jump climbs and nothing else goes down. The main menu's own scroll
-- arrow.
local descend = CreateFrame("Button", nil, hud)
descend:SetWidth(32)
descend:SetHeight(32)
descend:SetNormalTexture("Interface\\MainMenuBar\\UI-MainMenu-ScrollDownButton-Up")
descend:SetPushedTexture("Interface\\MainMenuBar\\UI-MainMenu-ScrollDownButton-Down")
descend:SetHighlightTexture("Interface\\MainMenuBar\\UI-MainMenu-ScrollDownButton-Highlight", "ADD")
descend:Hide()
AddZone({ frame = descend, group = "always", press = SitStandOrDescendStart, release = DescendStop })
descend:SetScript("OnMouseDown", SitStandOrDescendStart)
descend:SetScript("OnMouseUp", DescendStop)

-- The smart button does what the target calls for: no target, target the nearest enemy; an enemy, attack it; a body,
-- loot it (and once looted, move on to the next enemy); anyone else, talk to them. Swiped sideways it cycles enemies,
-- swiped up or down it opens or closes the arc by hand.
local smart = ThumbButton("Interface\\Icons\\Ability_Hunter_SniperShot")
smart:SetFrameStrata("HIGH")
local SMART = {
    target = { "Interface\\Icons\\Ability_Hunter_SniperShot", BINDING_NAME_TARGETNEARESTENEMY or "Target Nearest Enemy" },
    attack = { "Interface\\Icons\\Ability_SteelMelee", ATTACK or "Attack" },
    loot = { "Interface\\Icons\\INV_Misc_Bag_08", LOOT or "Loot" },
    talk = { "Interface\\Icons\\Ability_Warrior_BattleShout", BINDING_NAME_INTERACTTARGET or "Interact With Target" },
}
local looted

local function SmartMode()
    if not UnitExists("target") or UnitIsUnit("target", "player") then return "target" end
    if UnitIsDead("target") then return looted == UnitGUID("target") and "target" or "loot" end
    if UnitCanAttack("player", "target") then return "attack" end
    return "talk"
end

local function SmartTap()
    local mode = SmartMode()
    if mode == "target" then
        if UnitExists("target") and UnitIsDead("target") then ClearTarget() end
        TargetNearestEnemy()
    elseif mode == "attack" then
        if WebFaceTarget then WebFaceTarget() end
        AttackTarget()
    else
        if mode == "loot" then looted = UnitGUID("target") end
        InteractUnit("target")
    end
end

local function SmartSwipe(dx, dy)
    if abs(dx) > abs(dy) then
        TargetNearestEnemy()
    else
        ArcFlick(dx, dy)
    end
end

AddZone({ frame = smart, group = "always", tap = SmartTap, swipe = SmartSwipe,
    tip = function(b) LabelTip(SMART[SmartMode()][2])(b) end })
smart:SetScript("OnClick", SmartTap)

local function UpdateSmart()
    smart.icon:SetTexture(SMART[SmartMode()][1])
end

-- Mount: one of the player's mounts at random, or off it again. Only out of combat, and only with a mount to call.
local mount = ThumbButton("Interface\\Icons\\Ability_Mount_RidingHorse")
local UpdateMount
do
local IsMounted, Dismount, GetNumCompanions, GetCompanionInfo, CallCompanion = IsMounted, Dismount, GetNumCompanions, GetCompanionInfo, CallCompanion
local function MountTap()
    if IsMounted() then
        Dismount()
    elseif GetNumCompanions("MOUNT") > 0 then
        CallCompanion("MOUNT", random(GetNumCompanions("MOUNT")))
    end
end
AddZone({ frame = mount, group = "explore", tap = MountTap, tip = LabelTip(MOUNT or "Mount") })
mount:SetScript("OnClick", MountTap)

UpdateMount = function()
    local count = GetNumCompanions("MOUNT")
    if count > 0 then
        local _, _, _, icon = GetCompanionInfo("MOUNT", 1)
        mount.icon:SetTexture(icon)
    end
    mount.has = count > 0
end
end

-- ---- Stances and the pet: two thin rows over the bottom edge ----

for i = 1, NUM_SHAPESHIFT_SLOTS do
    local b = _G["ShapeshiftButton" .. i]
    AddZone({ frame = b, group = "always", tap = function() b:Click() end,
        tip = function() GameTooltip:SetOwner(b, "ANCHOR_LEFT"); GameTooltip:SetShapeshift(i); GameTooltip:Show() end })
end
for i = 1, NUM_PET_ACTION_SLOTS do
    local b = _G["PetActionButton" .. i]
    AddZone({ frame = b, group = "always", tap = function() b:Click() end,
        tip = function() GameTooltip:SetOwner(b, "ANCHOR_LEFT"); GameTooltip:SetPetAction(i); GameTooltip:Show() end })
end

-- ---- Chat: folded to a button; new lines float up beside it and fade ----

local chatButton = CreateFrame("Button", nil, hud)
chatButton:SetWidth(32)
chatButton:SetHeight(32)
chatButton:SetNormalTexture("Interface\\ChatFrame\\UI-ChatIcon-Chat-Up")
chatButton:SetPushedTexture("Interface\\ChatFrame\\UI-ChatIcon-Chat-Down")
chatButton:SetHighlightTexture("Interface\\Buttons\\UI-Common-MouseHilight", "ADD")
local CHAT_CHROME = { "ChatFrame1ButtonFrame", "ChatFrameMenuButton", "GeneralDockManager", "FriendsMicroButton", "ChatFrame1EditBox" }

local PlaceChat
local function SetChatOpen(open)
    chatOpen = open
    for _, name in ipairs(CHAT_CHROME) do
        if _G[name] then _G[name]:SetAlpha(open and 1 or 0) end
    end
    for i = 1, NUM_CHAT_WINDOWS do
        local tab = _G["ChatFrame" .. i .. "Tab"]
        if tab then tab:SetAlpha(open and 1 or 0) end
    end
    -- Folded, only the lines themselves: no background box. Hidden, not faded: the chat code fades in only what is shown.
    for _, name in ipairs(CHAT_FRAME_TEXTURES) do
        local texture = _G["ChatFrame1" .. name]
        if texture then
            if open then texture:Show() else texture:Hide() end
        end
    end
    ChatFrame1:SetTimeVisible(open and 120 or 6)
    PlaceChat()
    if open then
        ChatFrame_OpenChat("")
    else
        local editBox = ChatEdit_GetActiveWindow and ChatEdit_GetActiveWindow()
        if editBox then ChatEdit_OnEscapePressed(editBox) end
    end
    hud.zonesKey = nil
end

local function ChatTap() SetChatOpen(not chatOpen) end
AddZone({ frame = chatButton, group = "always", kind = "type", tap = ChatTap, tip = LabelTip(CHAT or "Chat") })
chatButton:SetScript("OnClick", ChatTap)

-- Folded, the chat sits over the chat button, right of the stick; opened, it takes the left of the screen and the
-- keyboard comes up. The chat code puts back its saved place and size on its own events, so this follows it.
PlaceChat = function()
    if InCombatLockdown() or W <= 1 then return end
    -- On the stick's side of the screen (the right, left-handed), clear of the arc.
    local point = cfg.left and "BOTTOMRIGHT" or "BOTTOMLEFT"
    ChatFrame1:ClearAllPoints()
    if chatOpen then
        ChatFrame1:SetPoint(point, UIParent, "BOTTOMLEFT", EdgeX(32), insetB + H * 0.3)
        ChatFrame1:SetWidth(min(380, W * 0.45))
        ChatFrame1:SetHeight(max(60, H - insetT - 120 - insetB - H * 0.3))
    else
        ChatFrame1:SetPoint(point, UIParent, "BOTTOMLEFT", EdgeX(200), insetB + 82)
        ChatFrame1:SetWidth(min(320, W * 0.38))
        ChatFrame1:SetHeight(64)
    end
end
hooksecurefunc("FCF_RestorePositionAndDimensions", function(frame) if frame == ChatFrame1 then PlaceChat() end end)

-- ---- Drawer: pulled down from the top, the micro buttons, bags, auto-run and sit ----

local drawer = CreateFrame("Frame", nil, hud)
drawer:SetFrameStrata("DIALOG")
drawer:SetBackdrop({
    bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
    edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
    tile = true, tileSize = 32, edgeSize = 32,
    insets = { left = 11, right = 12, top = 12, bottom = 11 },
})
drawer:SetWidth(410)
drawer:SetHeight(150)
drawer:EnableMouse(true)
drawer:Hide()
do
local header = drawer:CreateTexture(nil, "ARTWORK")
header:SetTexture("Interface\\DialogFrame\\UI-DialogBox-Header")
header:SetWidth(256)
header:SetHeight(64)
header:SetPoint("TOP", 0, 12)
local title = drawer:CreateFontString(nil, "ARTWORK", "GameFontNormal")
title:SetText(MAINMENU_BUTTON or "Game Menu")
title:SetPoint("TOP", header, "TOP", 0, -14)
end

local autorun = ThumbButton("Interface\\Icons\\Ability_Rogue_Sprint", drawer)
autorun:SetScript("OnClick", function() ToggleAutoRun(); drawer:Hide() end)
local sit = ThumbButton("Interface\\Icons\\Spell_Nature_Sleep", drawer)
sit:SetScript("OnMouseDown", SitStandOrDescendStart)
sit:SetScript("OnMouseUp", function() DescendStop(); drawer:Hide() end)
autorun:SetPoint("TOPRIGHT", drawer, "TOPRIGHT", -22, -30)
sit:SetPoint("TOP", autorun, "BOTTOM", 0, -10)
local gear = ThumbButton("Interface\\Icons\\INV_Misc_Gear_01", drawer)
gear:SetPoint("RIGHT", autorun, "LEFT", -10, 0)

-- The handle: the main menu's own scroll arrow, pointing down out of the top edge (up while the drawer is open).
local handle = CreateFrame("Button", nil, hud)
handle:SetWidth(32)
handle:SetHeight(32)
local ARROW = "Interface\\MainMenuBar\\UI-MainMenu-Scroll%sButton-%s"
local function HandleArt(open)
    local way = open and "Up" or "Down"
    handle:SetNormalTexture(ARROW:format(way, "Up"))
    handle:SetPushedTexture(ARROW:format(way, "Down"))
    handle:SetHighlightTexture(ARROW:format(way, "Highlight"), "ADD")
end
HandleArt(false)

local function SetDrawer(open)
    if open then drawer:Show() else drawer:Hide() end
    HandleArt(open)
end
local function DrawerTap() SetDrawer(not drawer:IsShown()) end
AddZone({ frame = handle, group = "always", tap = DrawerTap, swipe = function(dx, dy) SetDrawer(dy < 0) end,
    tip = LabelTip(MAINMENU_BUTTON or "Game Menu") })
handle:SetScript("OnClick", DrawerTap)
drawer:SetScript("OnHide", function() HandleArt(false) end)

local MICRO = { "CharacterMicroButton", "SpellbookMicroButton", "TalentMicroButton", "AchievementMicroButton",
    "QuestLogMicroButton", "SocialsMicroButton", "PVPMicroButton", "LFDMicroButton", "MainMenuMicroButton", "HelpMicroButton" }
local BAGS = { "MainMenuBarBackpackButton", "CharacterBag0Slot", "CharacterBag1Slot", "CharacterBag2Slot", "CharacterBag3Slot", "KeyRingButton" }

-- The micro buttons chain off CharacterMicroButton (and SocialsMicroButton off QuestLogMicroButton), as the stock bar
-- lays them out; leaving a vehicle puts them back on the main bar, so they come back here after it.
local function PlaceDrawerButtons()
    if UnitHasVehicleUI("player") then return end
    for _, name in ipairs(MICRO) do
        _G[name]:SetParent(drawer)
    end
    CharacterMicroButton:ClearAllPoints()
    CharacterMicroButton:SetPoint("TOPLEFT", drawer, "TOPLEFT", 20, -24)
    SocialsMicroButton:ClearAllPoints()
    SocialsMicroButton:SetPoint("BOTTOMLEFT", QuestLogMicroButton, "BOTTOMRIGHT", -3, 0)
    local last
    for _, name in ipairs(BAGS) do
        local bag = _G[name]
        if bag then
            bag:SetParent(drawer)
            bag:ClearAllPoints()
            if last then
                bag:SetPoint("LEFT", last, "RIGHT", 4, 0)
            else
                bag:SetPoint("BOTTOMLEFT", drawer, "BOTTOMLEFT", 22, 18)
            end
            last = bag
        end
    end
end

for _, name in ipairs(MICRO) do
    _G[name]:HookScript("OnClick", function() drawer:Hide() end)
end
for _, name in ipairs(BAGS) do
    if _G[name] then _G[name]:HookScript("OnClick", function() drawer:Hide() end) end
end
hooksecurefunc("VehicleMenuBar_MoveMicroButtons", PlaceDrawerButtons)

-- ---- Minimap: bigger while exploring, small in combat; a tap opens the world map ----

local function ToggleMap() ToggleFrame(WorldMapFrame) end
Minimap:SetScript("OnMouseUp", ToggleMap)

-- ---- The arc's state ----

local plateOn, shownPage, anim
local RING_DELAY = { 0, 0.05, 0.05, 0.05, 0.1, 0.1, 0.1 }   -- the buttons follow the plate out, ring by ring
local ANIM_TIME = 0.16

local function SetArcAlpha(entry, alpha)
    entry.frame:SetAlpha(alpha)
    if entry.alt then entry.alt:SetAlpha(alpha) end
end

-- The smart button: on the pivot while exploring, up on the outer ring with the arc open, k from 0 (folded) to 1.
local function PlaceSmart(k)
    if W <= 1 then return end
    local cx, cy = MX(arcX), arcY
    local ox, oy = ArcAt(SMART_PLACE[1], SMART_PLACE[2])
    Place(smart, cx + (ox - cx) * k, cy + (oy - cy) * k, (62 + (46 - 62) * k) * f)
end

local function PlacePlate(k)
    plate:SetAlpha(k)
    plate:ClearAllPoints()
    local slide = (1 - k) * 40
    if cfg.left then
        plate:SetPoint("BOTTOMLEFT", UIParent, "BOTTOMLEFT", insetL - slide, insetB - slide)
    else
        plate:SetPoint("BOTTOMRIGHT", UIParent, "BOTTOMRIGHT", -insetR + slide, insetB - slide)
    end
end

-- Opening, the plate slides in from the corner as it fades in, the buttons follow ring by ring and the smart button
-- moves up out of the way; closing runs it backwards; a page turn fades the new page in. Only alpha and the HUD's own
-- frames move, so it plays in combat too.
local function Animate()
    if not anim then return end
    local t = GetTime() - anim.t0
    local done = true
    for _, entry in ipairs(arc) do
        if entry.page == arcPage then
            local k = max(0, min(1, (t - (anim.open and RING_DELAY[entry.place] or 0)) / ANIM_TIME))
            if k < 1 then done = false end
            SetArcAlpha(entry, anim.open and k or 1 - k)
        end
    end
    if anim.plate then
        local k = max(0, min(1, t / ANIM_TIME))
        if k < 1 then done = false end
        local eased = 1 - (1 - k) * (1 - k)
        local shown = anim.open and eased or 1 - eased
        PlacePlate(shown)
        PlaceSmart(shown)
    end
    -- The smart button and the plate have moved: their zones are sent again where they ended up.
    if done then
        anim = nil
        hud.zonesKey = nil
    end
end

UpdateArc = function(force, animate)
    animate = animate ~= false
    local open = ArcOpen() and true or false
    if open and not PageHasActions(arcPage) then arcPage = 1 end
    UpdateSmart()
    MinimapCluster:SetScale(combat and 0.55 or 0.85)
    if not force and open == plateOn and arcPage == shownPage then return end
    local turned = open and open == plateOn and arcPage ~= shownPage
    shownPage = arcPage
    for _, entry in ipairs(arc) do
        local current = entry.page == arcPage
        if not animate or not current then
            SetArcAlpha(entry, open and current and 1 or 0)
        elseif open then
            SetArcAlpha(entry, 0)
        end
        -- Folded, the buttons are invisible but would still take a click meant for the world. The arc only folds out
        -- of combat (it opens as combat starts, before the lockdown), when their mouse can be switched.
        if not InCombatLockdown() then
            entry.frame:EnableMouse(open)
            if entry.alt then entry.alt:EnableMouse(open) end
        end
    end
    plateOn = open
    if animate then
        anim = { t0 = GetTime(), open = open, plate = not turned }
    else
        anim = nil
        PlacePlate(open and 1 or 0)
        PlaceSmart(open and 1 or 0)
    end
    for i, pip in ipairs(pips) do
        local has = open and PageHasActions(i)
        if has then pip:Show() else pip:Hide() end
        pip:SetTexCoord(i == arcPage and 0.25 or 0, i == arcPage and 0.5 or 0.25, 0, 1)
    end
    if not open and mount.has then mount:Show() else mount:Hide() end
    hud.zonesKey = nil
end

ArcFlick = function(dx, dy)
    if abs(dx) > abs(dy) then
        local step = dx < 0 and 1 or -1
        for _ = 1, 4 do
            arcPage = (arcPage - 1 + step) % 4 + 1
            if PageHasActions(arcPage) then break end
        end
        UpdateArc(true)
    elseif dy > 0 then
        forced = "open"
        UpdateArc()
    elseif not combat then
        forced = "closed"
        UpdateArc()
    end
end

-- The plate's own zone, between the buttons: a swipe of any speed is a flick; a tap does nothing (it only keeps the tap
-- off the world).
AddZone({ frame = plate, group = "plate", tap = function() end, swipe = function(dx, dy) ArcFlick(dx, dy) end })

-- ---- Layout ----

local function HideArt()
    for _, name in ipairs({ "MainMenuBarTexture0", "MainMenuBarTexture1", "MainMenuBarTexture2", "MainMenuBarTexture3",
            "MainMenuBarLeftEndCap", "MainMenuBarRightEndCap", "MainMenuBarPageNumber", "BonusActionBarTexture0",
            "BonusActionBarTexture1", "ShapeshiftBarLeft", "ShapeshiftBarMiddle", "ShapeshiftBarRight",
            "SlidingActionBarTexture0", "SlidingActionBarTexture1" }) do
        if _G[name] then _G[name]:SetAlpha(0) end
    end
    for _, name in ipairs({ "MainMenuExpBar", "MainMenuBarMaxLevelBar", "ReputationWatchBar", "ActionBarUpButton",
            "ActionBarDownButton", "MainMenuBarPerformanceBarFrame", "MultiBarBottomRight", "MultiBarLeft", "MultiBarRight",
            "MinimapZoomIn", "MinimapZoomOut", "MiniMapWorldMapButton" }) do
        if _G[name] then _G[name]:SetParent(hidden) end
    end
    -- The second bar is pages 3 and 4 of the arc, whatever the Interface Options say about showing it.
    MultiBarBottomLeft:SetParent(hud)
    MultiBarBottomLeft:Show()
    hooksecurefunc("MultiActionBar_Update", function()
        if not InCombatLockdown() then MultiBarBottomLeft:Show() end
    end)
    -- The bars' backgrounds would otherwise catch taps where nothing is drawn.
    MainMenuBar:EnableMouse(false)
    BonusActionBarFrame:EnableMouse(false)
    -- Key bindings mean nothing under a thumb.
    for i = 1, 12 do
        for _, bar in ipairs({ "ActionButton", "BonusActionButton", "MultiBarBottomLeftButton", "PetActionButton" }) do
            local hotKey = _G[bar .. i .. "HotKey"]
            if hotKey then hotKey:SetAlpha(0) end
        end
    end
    -- The panels' place under the player frame is taken; the screen is too short for both.
    UIParent:SetAttribute("TOP_OFFSET", -4)
    -- The tutorials teach the keyboard and mouse. Hidden here rather than switched off: that setting is the account's,
    -- and would follow it to desktop.
    -- Their "?" alert button too, which would open the hidden window.
    for _, name in ipairs({ "TutorialFrameParent", "TutorialFrame", "TutorialFrameAlertButton" }) do
        if _G[name] then _G[name]:SetParent(hidden) end
    end
    for i = 1, 10 do
        if _G["TutorialFrameAlertButton" .. i] then _G["TutorialFrameAlertButton" .. i]:SetParent(hidden) end
    end
end

local function ApplyScale()
    local _, h = WebTouchScreen()
    if not h or h <= 0 then return end
    -- One UI unit to a CSS pixel: 12 px text and 44 px buttons, as phones size things. Set on UIParent itself, not
    -- through the uiScale setting, which would follow this browser to a desktop session.
    local scale = 768 / max(384, min(768, h))
    if abs(UIParent:GetScale() - scale) > 0.01 then UIParent:SetScale(scale) end
end

-- Panels open at the top of the screen, over the player frame, scaled down until they fit its height.
local function FitPanels()
    if W <= 1 then return end
    for _, key in ipairs({ "left", "center", "right", "doublewide", "fullscreen" }) do
        local frame = GetUIPanel(key)
        if frame and frame ~= WorldMapFrame then
            frame:SetScale(min(1, (H - insetT - insetB - 8) / frame:GetHeight()))
        end
    end
end
hooksecurefunc("UpdateUIPanelPositions", FitPanels)

-- The bottom edge, between the chat button and the arc: the pet row, and the stances over it. The stock bars re-anchor
-- their first buttons and the cast bar on their own, so this follows them.
local rowRight = 0
local function PlaceRows()
    if W <= 1 then return end
    -- The cast bar where the eye already is: over the character.
    CastingBarFrame:ClearAllPoints()
    CastingBarFrame:SetPoint("CENTER", UIParent, "BOTTOMLEFT", W / 2, insetB + H * 0.47)
    if InCombatLockdown() then return end
    for i = 1, NUM_PET_ACTION_SLOTS do
        Place(_G["PetActionButton" .. i], MX(rowRight - (10 - i) * 30), insetB + 20, 28)
    end
    for i = 1, NUM_SHAPESHIFT_SLOTS do
        Place(_G["ShapeshiftButton" .. i], MX(rowRight - (10 - i) * 30), insetB + 54, 28)
    end
end
hooksecurefunc("ShapeshiftBar_Update", PlaceRows)
hooksecurefunc("UIParent_ManageFramePositions", PlaceRows)

local pending
local function Layout()
    if InCombatLockdown() then
        pending = true
        return
    end
    pending = nil

    ApplyScale()
    local l, r, t, b
    cssW, cssH, l, r, t, b = WebTouchScreen()
    W, H = UIParent:GetWidth(), UIParent:GetHeight()
    unit = H / max(1, cssH)
    insetL, insetR, insetT, insetB = l * unit, r * unit, t * unit, b * unit

    f = cfg.buttons
    arcX, arcY = W - (cfg.left and insetL or insetR) - 80 * f, insetB + 80 * f
    for _, entry in ipairs(arc) do
        local place = PLACES[entry.place]
        local x, y = ArcAt(place[1], place[2])
        Place(entry.frame, x, y, place[3] * f)
        if entry.alt then Place(entry.alt, x, y, place[3] * f) end
    end
    local x, y = ArcAt(JUMP_PLACE[1], JUMP_PLACE[2])
    Place(jump, x, y, 48 * f)
    x, y = ArcAt(MOUNT_PLACE[1], MOUNT_PLACE[2])
    Place(mount, x, y, 48 * f)
    x, y = ArcAt(DESCEND_PLACE[1], DESCEND_PLACE[2])
    Place(descend, x, y, 44 * f)
    -- The page pips on the plate's stone between the main button and the inner ring.
    for i, pip in ipairs(pips) do
        x, y = ArcAt(56, 165 - (i - 1) * 15)
        pip:ClearAllPoints()
        pip:SetPoint("CENTER", UIParent, "BOTTOMLEFT", x, y)
    end
    -- The plate and its gryphon, turned around for the left hand.
    plate:SetWidth(288 * f)
    plate:SetHeight(288 * f)
    plateTex:SetTexCoord(cfg.left and 1 or 0, cfg.left and 0 or 1, 0, 1)
    gryphon:SetWidth(128 * f)
    gryphon:SetHeight(128 * f)
    gryphon:SetTexCoord(cfg.left and 1 or 0, cfg.left and 0 or 1, 0, 1)
    gryphon:ClearAllPoints()
    if cfg.left then
        gryphon:SetPoint("BOTTOMLEFT", plate, "BOTTOMLEFT", 172 * f, -12 * f)
    else
        gryphon:SetPoint("BOTTOMRIGHT", plate, "BOTTOMRIGHT", -172 * f, -12 * f)
    end

    rowRight = arcX - 262 * f
    PlaceRows()
    -- The totem bar hangs off MainMenuBar's top left (UIParent_ManageFramePositions): moved for it, it sits where the
    -- stances would, which a shaman does not have. Its art is hidden and it no longer takes the mouse.
    local totemLeft = cfg.left and MX(rowRight) - 15 or rowRight - 9 * 30 - 15
    MainMenuBar:ClearAllPoints()
    MainMenuBar:SetPoint("BOTTOMLEFT", UIParent, "BOTTOMLEFT", totemLeft - 30, insetB + 5)

    Place(chatButton, EdgeX(200), insetB + 26, 40)
    PlaceChat()

    Place(handle, W / 2, H - insetT - 14, 56)
    drawer:ClearAllPoints()
    drawer:SetPoint("TOP", UIParent, "TOP", 0, -insetT - 30)
    drawer:SetScale(1.2)
    PlaceDrawerButtons()

    stick:SetScale(cfg.stick)
    stickRestX, stickRestY = EdgeX(100 * cfg.stick), insetB + 100 * cfg.stick
    stick.moved = true
    UpdateMount()
    UpdateArc(true, false)
end

-- ---- Zones ----

local function SendZone(id, frame, pad, kind)
    local l, b, r, t = frame:GetLeft(), frame:GetBottom(), frame:GetRight(), frame:GetTop()
    if not l then
        WebTouchSetZone(id, nil)
        return
    end
    local s = frame:GetEffectiveScale() / UIParent:GetEffectiveScale()
    WebTouchSetZone(id, kind or "button", (l * s - pad) / W, (b * s - pad) / H, (r * s + pad) / W, (t * s + pad) / H)
end

local function AnyBagOpen()
    for i = 1, NUM_CONTAINER_FRAMES do
        if _G["ContainerFrame" .. i]:IsShown() then return true end
    end
end

-- Which zones are live, one character each (zone 1, the stick, first). A panel over the stick's corner, an open chat,
-- or bags over the arc take their taps instead; a button that isn't there (an empty slot, another page, the bar in a
-- vehicle) has none.
local live = {}
local function ZonesKey()
    local map = WorldMapFrame:IsShown()
    local panel = GetUIPanel("left") or GetUIPanel("center") or GetUIPanel("doublewide") or GetUIPanel("fullscreen")
    local bags = AnyBagOpen()
    local open = plateOn and not map and not bags
    live[1] = not map and not panel and not chatOpen
    for id = 2, nextZone - 1 do
        local zone = zones[id]
        local on
        if zone.group == "arc" then
            on = open and zone.page == arcPage
        elseif zone.group == "plate" then
            on = open
        elseif zone.group == "explore" then
            on = not map and not plateOn and zone.frame:IsShown()
        else
            on = not map
        end
        live[id] = on and (zone.frame:IsVisible() or (zone.alt and zone.alt:IsVisible())) or false
    end
    local key = ""
    for id = 1, nextZone - 1 do
        key = key .. (live[id] and "1" or "0")
    end
    return key
end

local function SendZones()
    if live[1] then
        if cfg.left then
            WebTouchSetZone(1, "stick", 0.62, 0, 1, 0.46)
        else
            WebTouchSetZone(1, "stick", 0, 0, 0.38, 0.46)
        end
        stick:Show()
    else
        WebTouchSetZone(1, nil)
        stick:Hide()
    end
    for id = 2, nextZone - 1 do
        if live[id] then
            -- Opening the chat opens the keyboard, but closing it must not, so the chat button only types while closed.
            local kind = zones[id].kind
            if kind == "type" and chatOpen then kind = nil end
            SendZone(id, zones[id].frame, zones[id].group == "plate" and 0 or 4, kind)
        else
            WebTouchSetZone(id, nil)
        end
    end
end

-- ---- Driving it ----

local sinceZones = 0
hud:SetScript("OnUpdate", function(self, elapsed)
    for _ = 1, 64 do
        local id, down, outside = WebTouchNextEvent()
        if not id then break end
        if id == 100 then
            CameraZoomIn(1)
        elseif id == 101 then
            CameraZoomOut(1)
        elseif zones[id] then
            if down then ZoneDown(id) else ZoneUp(id, outside) end
        end
    end

    for id, p in pairs(pressing) do
        ZoneHeld(id, p)
    end
    Animate()
    if stopTargetingAt and GetTime() > stopTargetingAt then
        stopTargetingAt = nil
        if SpellIsTargeting() then SpellStopTargeting() end
    end

    UpdateStick()

    sinceZones = sinceZones + elapsed
    if sinceZones > 0.2 then
        sinceZones = 0
        local up = IsFlying() or IsSwimming()
        if up and not descend:IsShown() then descend:Show() elseif not up and descend:IsShown() then descend:Hide() end
        local key = ZonesKey()
        if key ~= self.zonesKey then
            self.zonesKey = key
            SendZones()
        end
    end
end)

-- ---- Touch options: Interface Options > Touch, in the stock options layout ----
-- In a block of its own: Lua 5.1 allows a function (this chunk) at most 200 locals at a time.

do
local options = CreateFrame("Frame", "LasthavensTouchOptions")
options.name = "Touch"
local heading = options:CreateFontString(nil, "ARTWORK", "GameFontNormalLarge")
heading:SetPoint("TOPLEFT", 16, -16)
heading:SetText("Touch Controls")
local blurb = options:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall")
blurb:SetPoint("TOPLEFT", heading, "BOTTOMLEFT", 0, -8)
blurb:SetPoint("RIGHT", -32, 0)
blurb:SetJustifyH("LEFT")
blurb:SetText("The stick, the action arc and the gestures, for phones and tablets. Kept for this browser only.")

local function Changed()
    ReadSettings()
    Layout()
end

local function OptionCheck(name, label, cvar, y, onChange)
    local check = CreateFrame("CheckButton", "LasthavensTouchOptions" .. name, options, "InterfaceOptionsCheckButtonTemplate")
    check:SetPoint("TOPLEFT", 16, y)
    _G[check:GetName() .. "Text"]:SetText(label)
    check:SetScript("OnShow", function(self) self:SetChecked(GetCVar(cvar) == "1") end)
    check:SetScript("OnClick", function(self)
        SetCVar(cvar, self:GetChecked() and "1" or "0")
        local apply = onChange or Changed
        apply()
    end)
end

local function OptionSlider(name, label, cvar, lo, hi, step, y, show)
    local slider = CreateFrame("Slider", "LasthavensTouchOptions" .. name, options, "OptionsSliderTemplate")
    slider:SetPoint("TOPLEFT", 24, y)
    slider:SetWidth(240)
    slider:SetMinMaxValues(lo, hi)
    slider:SetValueStep(step)
    _G[slider:GetName() .. "Low"]:SetText(show(lo))
    _G[slider:GetName() .. "High"]:SetText(show(hi))
    local text = _G[slider:GetName() .. "Text"]
    slider:SetScript("OnShow", function(self) self:SetValue(tonumber(GetCVar(cvar)) or lo) end)
    slider:SetScript("OnValueChanged", function(self, value)
        value = floor(value / step + 0.5) * step
        text:SetText(label .. ": " .. show(value))
        if tostring(value) ~= GetCVar(cvar) then
            SetCVar(cvar, value)
            Changed()
        end
    end)
end

local function Percent(v) return floor(v + 0.5) .. "%" end
OptionCheck("Enable", "Touch controls (reloads the interface)", "webTouch", -64, ReloadUI)
OptionCheck("Left", "Left-handed: the action arc on the left", "webTouchLeft", -92)
OptionSlider("Buttons", "Button size", "webTouchButtons", 80, 130, 5, -146, Percent)
OptionSlider("Stick", "Stick size", "webTouchStick", 80, 130, 5, -196, Percent)
OptionSlider("Hold", "Hold for a right click", "webTouchHold", 300, 800, 50, -246, function(v) return format("%.2f s", v / 1000) end)
OptionSlider("Camera", "Camera speed", "webTouchCamera", 50, 200, 10, -296, Percent)
InterfaceOptions_AddCategory(options)

gear:SetScript("OnClick", function()
    drawer:Hide()
    InterfaceOptionsFrame_OpenToCategory(options)
end)
end

-- The option windows and the game menu are sized for 768 units of height: scaled down to fit the phone's.
for _, name in ipairs({ "InterfaceOptionsFrame", "VideoOptionsFrame", "AudioOptionsFrame", "GameMenuFrame" }) do
    local frame = _G[name]
    if frame then
        frame:HookScript("OnShow", function(self)
            if W > 1 then self:SetScale(min(1, (H - insetT - insetB - 8) / self:GetHeight())) end
        end)
    end
end

for _, event in ipairs({ "PLAYER_ENTERING_WORLD", "DISPLAY_SIZE_CHANGED", "UI_SCALE_CHANGED", "PLAYER_REGEN_ENABLED",
        "PLAYER_REGEN_DISABLED", "PLAYER_TARGET_CHANGED", "UNIT_EXITED_VEHICLE", "COMPANION_LEARNED", "ACTIONBAR_SLOT_CHANGED",
        "UNIT_DYNAMIC_FLAGS", "UNIT_HEALTH" }) do
    hud:RegisterEvent(event)
end
hud:SetScript("OnEvent", function(self, event, unitId)
    if event == "PLAYER_ENTERING_WORLD" then
        combat = InCombatLockdown() and true or false
        Layout()
        SetChatOpen(false)
    elseif event == "PLAYER_REGEN_DISABLED" then
        combat = true
        UpdateArc()
    elseif event == "PLAYER_REGEN_ENABLED" then
        combat = false
        forced = nil
        if pending then Layout() else UpdateArc() end
    elseif event == "PLAYER_TARGET_CHANGED" or event == "ACTIONBAR_SLOT_CHANGED" then
        -- A new enemy opens an arc closed by hand again.
        if event == "PLAYER_TARGET_CHANGED" and forced == "closed" then forced = nil end
        UpdateArc()
    elseif event == "UNIT_DYNAMIC_FLAGS" or event == "UNIT_HEALTH" then
        if unitId == "target" then UpdateArc() end
    elseif event == "COMPANION_LEARNED" then
        UpdateMount()
        UpdateArc()
    else
        Layout()
    end
end)

HideArt()
)lua";

}

void WebTouchInitialize() {
    // A touch screen turns the HUD on by default (index.html sets window.wowTouch).
    static bool registered;
    if (!registered) {
        registered = true;
        bool touch = MAIN_THREAD_EM_ASM_INT({ return window.wowTouch ? 1 : 0; }) != 0;
        // The device decides on every load, over any saved value: Config.wtf can be shared between a phone and a desktop
        // (the local bridge keeps one for everyone), and a phone's setting must not turn the HUD on at the desk. The
        // options panel's switch lasts until the page reloads; ?touch=1 or ?touch=0 forces it.
        CVar::Register("webTouch", "Touch controls (browser build)", 0, touch ? "1" : "0", nullptr, 4, false, nullptr, false)
            ->Set(touch ? "1" : "0", true, false, false, false);
        // The Touch options, kept in this browser's Config.wtf (never the account's settings, which follow it to desktop).
        CVar::Register("webTouchButtons", "Touch: button size, percent", 0, "100", nullptr, 4, false, nullptr, false);
        CVar::Register("webTouchStick", "Touch: stick size, percent", 0, "100", nullptr, 4, false, nullptr, false);
        CVar::Register("webTouchLeft", "Touch: the arc on the left", 0, "0", nullptr, 4, false, nullptr, false);
        CVar::Register("webTouchHold", "Touch: hold for a right click, ms", 0, "450", nullptr, 4, false, nullptr, false);
        CVar::Register("webTouchCamera", "Touch: camera drag speed, percent", 0, "100", nullptr, 4, false, nullptr, false);
    }

    OsWebTouchClearZones();
    OsWebTouchSetDragProc(TouchDragButton);

    FrameScript_RegisterFunction("WebTouchSetZone", Script_WebTouchSetZone);
    FrameScript_RegisterFunction("WebTouchGetStick", Script_WebTouchGetStick);
    FrameScript_RegisterFunction("WebTouchNextEvent", Script_WebTouchNextEvent);
    FrameScript_RegisterFunction("WebTouchZoneDrag", Script_WebTouchZoneDrag);
    FrameScript_RegisterFunction("WebTouchMoveCursor", Script_WebTouchMoveCursor);
    FrameScript_RegisterFunction("WebTouchClickAt", Script_WebTouchClickAt);
    FrameScript_RegisterFunction("WebSpellIsGround", Script_WebSpellIsGround);
    FrameScript_RegisterFunction("WebTouchScreen", Script_WebTouchScreen);
    FrameScript_RegisterFunction("WebTouchSetTuning", Script_WebTouchSetTuning);
    WebFaceInitialize();

    FrameScript_Execute(kTouchUi, "WebTouch.lua", nullptr);
    // Loot and quest dialogs for touch (WebTouchPanels.cpp).
    WebTouchPanelsInitialize();
    // The quick chat menu (WebQuickChat.cpp), after the HUD it sits on.
    WebQuickChatInitialize();
}

#endif
