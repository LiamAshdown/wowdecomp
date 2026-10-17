#if defined(WOWDECOMP_PLATFORM_WEB)

// Looting and questing for the touch HUD (docs/mobile-plan.md): one tap each. Like WebTouch.cpp, the Lua below runs as
// Blizzard code right after FrameXML loads, and only while the webTouch CVar is 1. It changes the stock frames, not
// their look:
// - Loot: whatever can be taken at once is, when the loot opens; the account's autoLootDefault is left alone, so it does
//   not follow the player to the desktop. The stock LootFrame stays for the rest (bind-on-pickup confirmations, items
//   rolled for or handed out by the master looter, full bags), scaled up and placed just left of the action arc.
// - NPC dialogs: GossipFrame's and QuestFrame's own red buttons (Accept, Decline, Continue, Complete Quest, Goodbye,
//   Cancel) are made thumb-sized where they are, and gossip options and quest titles are taller rows.
// The quest tracker needs nothing: the stock WatchFrame opens a quest in the quest log on a left click, which is what a
// tap is (Os/Web/Input.cpp).

#include "Engine/Source/FrameScript/FrameScript.h"

namespace {

const char kTouchPanels[] = R"lua(
if GetCVar("webTouch") ~= "1" then return end

local ARC_LEFT = 277   -- UI units from the right edge to just left of the open arc: pivot 80 in, outer ring 165 out, half a button, 8 clear
local ROWS_TOP = 74    -- the pet and stance rows along the bottom edge (WebTouch.cpp) end below this
local LOOT_ART = 186   -- the loot panel's art is the left 186 of its 256 units (its HitRectInsets cut off the other 70)
local LOOT_SCALE = 1.15  -- a loot row 47 units high, about a fingertip
local BUTTON_W, BUTTON_H, BUTTON_Y = 148, 40, 66   -- the dialogs' red buttons, on the bar along the bottom of the art
local ROW = 36         -- a gossip option or quest title, at least

-- Captured now, so nothing an addon puts in the globals later can stand in for them.
local GetNumLootItems, GetLootSlotInfo, LootSlotIsCoin, LootSlot = GetNumLootItems, GetLootSlotInfo, LootSlotIsCoin, LootSlot
local GetLootMethod, GetLootThreshold, GetNumPartyMembers, GetNumRaidMembers = GetLootMethod, GetLootThreshold, GetNumPartyMembers, GetNumRaidMembers
local InCombatLockdown, WebTouchScreen = InCombatLockdown, WebTouchScreen   -- Ui/Web/WebTouch.cpp
local min, max = math.min, math.max

-- ---- Loot ----

-- The loot window is the HUD's own to place, not a panel at the top left: the panel manager would put it back there.
-- Escape (the two-finger tap) still closes it.
UIPanelWindows["LootFrame"] = nil
tinsert(UISpecialFrames, "LootFrame")

-- Scaled up and just left of the arc, above the rows on the bottom edge (and clear of the notch).
local function PlaceLoot()
    if InCombatLockdown() and LootFrame:IsProtected() then return end
    local W, H = UIParent:GetWidth(), UIParent:GetHeight()
    local insetR, insetT, insetB = 0, 0, 0
    if WebTouchScreen then
        local _, cssH, _, r, t, b = WebTouchScreen()
        local unit = H / max(1, cssH or 1)
        insetR, insetT, insetB = (r or 0) * unit, (t or 0) * unit, (b or 0) * unit
    end
    local s = min(LOOT_SCALE, (H - insetT - insetB - ROWS_TOP - 4) / LootFrame:GetHeight())
    LootFrame:SetScale(s)
    LootFrame:ClearAllPoints()
    LootFrame:SetPoint("BOTTOMLEFT", UIParent, "BOTTOMLEFT", (W - insetR - ARC_LEFT - LOOT_ART * s) / s, (insetB + ROWS_TOP) / s)
end
hooksecurefunc("LootFrame_Show", PlaceLoot)

-- Everything that can be taken, a frame after the loot opens (once LootFrame has shown it: taking the coins alone can
-- close the loot at once). Not what the client already takes itself (autoLoot 1: autoLootDefault or the modifier), nor
-- what LootSlot would only complain about or open the master looter's list for: locked items, and in a group under
-- group, need-before-greed or master loot, items at or over the threshold. The client closes the loot once it is empty.
local looter = CreateFrame("Frame")
looter:Hide()
looter:SetScript("OnUpdate", function(self)
    self:Hide()
    local method = GetLootMethod()
    local shared = (GetNumPartyMembers() > 0 or GetNumRaidMembers() > 0) and method ~= "freeforall" and method ~= "roundrobin"
    local threshold = GetLootThreshold()
    for slot = 1, GetNumLootItems() do
        local _, _, _, quality, locked = GetLootSlotInfo(slot)
        if LootSlotIsCoin(slot) or (not locked and not (shared and (quality or 0) >= threshold)) then
            LootSlot(slot)
        end
    end
end)
looter:RegisterEvent("LOOT_OPENED")
looter:RegisterEvent("LOOT_CLOSED")
looter:SetScript("OnEvent", function(self, event, autoLoot)
    if event == "LOOT_OPENED" and autoLoot ~= 1 then self:Show() else self:Hide() end
end)

-- ---- NPC dialogs ----

-- The stock red buttons, thumb-sized in their stock places: the left one (go on) and the right one (go back).
local LEFT = { "QuestFrameAcceptButton", "QuestFrameCompleteButton", "QuestFrameCompleteQuestButton" }
local RIGHT = { "QuestFrameDeclineButton", "QuestFrameGoodbyeButton", "QuestFrameCancelButton", "QuestFrameGreetingGoodbyeButton",
    "GossipFrameGreetingGoodbyeButton" }

local function ThumbSize(name, left)
    local b = _G[name]
    if not b then return end
    local frame = name:find("^Gossip") and GossipFrame or QuestFrame
    b:SetWidth(BUTTON_W)
    b:SetHeight(BUTTON_H)
    b:SetNormalFontObject(GameFontNormalLarge)
    b:SetHighlightFontObject(GameFontHighlightLarge)
    b:SetDisabledFontObject(GameFontDisableLarge)
    b:ClearAllPoints()
    if left then
        b:SetPoint("BOTTOMLEFT", frame, "BOTTOMLEFT", 22, BUTTON_Y)
    else
        b:SetPoint("BOTTOMRIGHT", frame, "BOTTOMRIGHT", -39, BUTTON_Y)
    end
end
for _, name in ipairs(LEFT) do ThumbSize(name, true) end
for _, name in ipairs(RIGHT) do ThumbSize(name, false) end

-- Gossip options and quest titles: taller rows, the icon and text centred in them. The stock code sizes each row to
-- its text when it fills it in (GossipResize, QuestFrameGreetingPanel's OnShow); this follows it.
hooksecurefunc("GossipResize", function(b)
    b:SetHeight(max(b:GetHeight(), ROW))
end)
for i = 1, NUMGOSSIPBUTTONS do
    local icon = _G["GossipTitleButton" .. i .. "GossipIcon"]
    icon:ClearAllPoints()
    icon:SetPoint("LEFT", 3, 0)
end

for i = 1, MAX_NUM_QUESTS do
    local b = _G["QuestTitleButton" .. i]
    local icon = _G["QuestTitleButton" .. i .. "QuestIcon"]
    icon:ClearAllPoints()
    icon:SetPoint("LEFT", 4, 0)
    local text = b:GetFontString()
    text:ClearAllPoints()
    text:SetPoint("LEFT", 20, 0)
end
QuestFrameGreetingPanel:HookScript("OnShow", function()
    for i = 1, MAX_NUM_QUESTS do
        local b = _G["QuestTitleButton" .. i]
        b:SetHeight(max(b:GetHeight(), ROW))
    end
end)
)lua";

}

void WebTouchPanelsInitialize() {
    FrameScript_Execute(kTouchPanels, "WebTouchPanels.lua", nullptr);
}

#endif
