#if defined(WOWDECOMP_PLATFORM_WEB)

// Quick chat for the touch HUD (docs/mobile-plan.md): talking without the phone keyboard. A round button in the stock
// minimap-button style sits on the chat's top corner, away from the stick; it opens a stock dialog of red buttons: the
// channel (say, party, guild, raid, or the last whisper), canned phrases, the common emotes, and the recent whispers,
// where a name opens the chat's edit box whispering that player (and with it the phone keyboard, Os/Web/OsIME.cpp).
// Like WebTouch.cpp, the Lua below runs as Blizzard code right after FrameXML loads, and only while the webTouch CVar is
// 1. The phrases are one CVar, webQuickChat, ';' between them, kept in this browser's Config.wtf.
//
// The menu is a stock left panel: while a panel is up the HUD hands the stick's corner back to taps (WebTouch.cpp,
// ZonesKey), and Escape (the two-finger tap) closes it. It then moves to the stick's side, clear of the arc's plate.

#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/FrameScript/FrameScript.h"

namespace {

const char kQuickChat[] = R"lua(
if GetCVar("webTouch") ~= "1" then return end

local MENU_W, MENU_H = 400, 248
local PLATE = 296        -- UI units the arc's plate takes from its edge of the screen (288), and 8 clear
local TOP_CLEAR = 52     -- the menu's top below the drawer's handle at the top centre, its header included
local BOTTOM_CLEAR = 76  -- the menu's bottom above the pet and stance rows along the bottom edge
local EDGE = 8
local PAD, GAP, BUTTON_H, SECTION = 16, 4, 26, 10
local MAX_RECENT = 4

-- Captured now, so nothing an addon puts in the globals later can stand in for them.
local SendChatMessage, DoEmote, GetCVar, ChatFrame_SendTell = SendChatMessage, DoEmote, GetCVar, ChatFrame_SendTell
local GetNumPartyMembers, GetNumRaidMembers, IsInGuild = GetNumPartyMembers, GetNumRaidMembers, IsInGuild
local ShowUIPanel, HideUIPanel, WebTouchScreen = ShowUIPanel, HideUIPanel, WebTouchScreen   -- Ui/Web/WebTouch.cpp
local ChatFrame1, editBox = ChatFrame1, ChatFrame1EditBox
local min, max, gmatch, tinsert, tremove = math.min, math.max, string.gmatch, table.insert, table.remove

local EMOTES = {
    { "WAVE", "Wave" }, { "THANK", "Thank" }, { "CHEER", "Cheer" }, { "BOW", "Bow" },
    { "LAUGH", "Laugh" }, { "DANCE", "Dance" }, { "APPLAUD", "Applaud" }, { "ROAR", "Roar" },
}

-- Whispered with, either way, latest first. Only this session's: the stock tell targets aren't kept either.
local recent = {}

-- The player the whisper channel goes to: the edit box's own, when it is whispering, or the latest.
local function WhisperTarget()
    if editBox:GetAttribute("chatType") == "WHISPER" and editBox:GetAttribute("tellTarget") then
        return editBox:GetAttribute("tellTarget")
    end
    return recent[1]
end

local CHANNELS = {
    { type = "SAY", label = SAY or "Say", can = function() return true end },
    { type = "PARTY", label = PARTY or "Party", can = function() return GetNumPartyMembers() > 0 or GetNumRaidMembers() > 0 end },
    { type = "GUILD", label = GUILD or "Guild", can = function() return IsInGuild() end },
    { type = "RAID", label = RAID or "Raid", can = function() return GetNumRaidMembers() > 0 end },
    { type = "WHISPER", label = WHISPER or "Whisper", can = function() return WhisperTarget() ~= nil end },
}

-- The phrases, from the CVar: up to 8, without the escape character (a stray "|" gets a chat line refused).
local function Phrases()
    local list = {}
    for phrase in gmatch(GetCVar("webQuickChat") or "", "[^;]+") do
        phrase = phrase:gsub("|", ""):match("^%s*(.-)%s*$"):sub(1, 255)
        if phrase ~= "" and #list < 8 then tinsert(list, phrase) end
    end
    return list
end

-- ---- The menu: the stock dialog box with its header and close button ----

local menu = CreateFrame("Frame", "WebQuickChatFrame", UIParent)
menu:Hide()
menu:SetWidth(MENU_W)
menu:SetHeight(MENU_H)
menu:SetFrameStrata("DIALOG")
menu:SetToplevel(true)
menu:EnableMouse(true)
menu:SetBackdrop({
    bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
    edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
    tile = true, tileSize = 32, edgeSize = 32,
    insets = { left = 11, right = 12, top = 12, bottom = 11 },
})
UIPanelWindows["WebQuickChatFrame"] = { area = "left", pushable = 0, whileDead = 1 }

local header = menu:CreateTexture(nil, "ARTWORK")
header:SetTexture("Interface\\DialogFrame\\UI-DialogBox-Header")
header:SetWidth(256)
header:SetHeight(64)
header:SetPoint("TOP", 0, 12)
local title = menu:CreateFontString(nil, "ARTWORK", "GameFontNormal")
title:SetPoint("TOP", header, "TOP", 0, -14)
title:SetText("Quick Chat")

CreateFrame("Button", "WebQuickChatFrameClose", menu, "UIPanelCloseButton"):SetPoint("TOPRIGHT", -3, -3)

local nextButton = 0
local function MenuButton(x, y, width, onClick)
    nextButton = nextButton + 1
    local b = CreateFrame("Button", "WebQuickChatFrameButton" .. nextButton, menu, "UIPanelButtonTemplate")
    b:SetWidth(width)
    b:SetHeight(BUTTON_H)
    b:SetPoint("TOPLEFT", PAD + x, y)
    b:SetScript("OnClick", onClick)
    b:GetFontString():SetWidth(width - 8)   -- a long name is cut, not spilled over its neighbours
    return b
end

-- A row of n buttons across the menu; calls make(i, x, width) for each.
local function Row(n, make)
    local width = (MENU_W - 2 * PAD - (n - 1) * GAP) / n
    for i = 1, n do make(i, (i - 1) * (width + GAP), width) end
end

local function Close()
    HideUIPanel(menu)
    if menu:IsShown() then menu:Hide() end
end

-- ---- Channel ----

local chatType = "SAY"
local channelButtons = {}
local Refresh

local function Usable(t)
    for _, c in ipairs(CHANNELS) do
        if c.type == t then return c.can() end
    end
end

-- Picking a channel sets the stock edit box's own, so typed chat follows it too.
local function SetChannel(t)
    local target = WhisperTarget()
    chatType = t
    editBox:SetAttribute("chatType", t)
    if ChatTypeInfo[t] and ChatTypeInfo[t].sticky == 1 then editBox:SetAttribute("stickyType", t) end
    if t == "WHISPER" then editBox:SetAttribute("tellTarget", target) end
    if editBox:IsShown() then ChatEdit_UpdateHeader(editBox) end
    Refresh()
end

local y = -38   -- below the header and the close button
Row(#CHANNELS, function(i, x, width)
    channelButtons[i] = MenuButton(x, y, width, function() SetChannel(CHANNELS[i].type) end)
end)
y = y - BUTTON_H - SECTION

-- ---- Phrases and emotes, 4 to a row ----

local function Say(text)
    local t = Usable(chatType) and chatType or "SAY"
    SendChatMessage(text, t, nil, t == "WHISPER" and WhisperTarget() or nil)
    Close()
end

local phraseButtons = {}
for row = 0, 1 do
    Row(4, function(i, x, width)
        local b
        b = MenuButton(x, y, width, function() Say(b:GetText()) end)
        phraseButtons[row * 4 + i] = b
    end)
    y = y - BUTTON_H - GAP
end
y = y - SECTION + GAP

for row = 0, 1 do
    Row(4, function(i, x, width)
        local emote = EMOTES[row * 4 + i]
        MenuButton(x, y, width, function()
            DoEmote(emote[1])
            Close()
        end):SetText(emote[2])
    end)
    y = y - BUTTON_H - GAP
end
y = y - SECTION + GAP

-- ---- Recent whispers: a name opens the edit box whispering them ----

-- Folded, the HUD's chat hides its edit box (alpha 0): it is shown for the whisper, and hidden again after.
local unfaded
editBox:HookScript("OnHide", function(self)
    if unfaded then
        unfaded = false
        self:SetAlpha(0)
    end
end)

local function WhisperTo(name)
    Close()
    if editBox:GetAlpha() < 0.5 then
        editBox:SetAlpha(1)
        unfaded = true
    end
    ChatFrame_SendTell(name, ChatFrame1)
end

local whisperButtons = {}
local whisperLabel = menu:CreateFontString(nil, "ARTWORK", "GameFontDisable")
whisperLabel:SetPoint("TOPLEFT", PAD + 6, y - 7)
whisperLabel:SetText("No recent whispers")
Row(MAX_RECENT, function(i, x, width)
    local b
    b = MenuButton(x, y, width, function() WhisperTo(b.name) end)
    whisperButtons[i] = b
end)

-- ---- Filling it in ----

Refresh = function()
    if not Usable(chatType) then chatType = "SAY" end
    for i, c in ipairs(CHANNELS) do
        local b = channelButtons[i]
        b:SetText(c.type == "WHISPER" and WhisperTarget() or c.label)
        if c.can() then b:Enable() else b:Disable() end
        if c.type == chatType then b:LockHighlight() else b:UnlockHighlight() end
    end
    local phrases = Phrases()
    for i, b in ipairs(phraseButtons) do
        if phrases[i] then
            b:SetText(phrases[i])
            b:Show()
        else
            b:Hide()
        end
    end
    for i, b in ipairs(whisperButtons) do
        b.name = recent[i]
        if recent[i] then
            b:SetText(recent[i])
            b:Show()
        else
            b:Hide()
        end
    end
    if recent[1] then whisperLabel:Hide() else whisperLabel:Show() end
end

-- The edit box's channel, when it is one of the menu's.
local function ReadChannel()
    local t = editBox:GetAttribute("chatType")
    for _, c in ipairs(CHANNELS) do
        if c.type == t then chatType = t end
    end
end

-- On the stick's side of the screen, below the drawer's handle and above the bottom rows, clear of the arc's plate;
-- scaled down until it fits. Follows the panel manager, which puts every left panel at the top left.
local function PlaceMenu()
    local W, H = UIParent:GetWidth(), UIParent:GetHeight()
    local insetL, insetR, insetT, insetB = 0, 0, 0, 0
    if WebTouchScreen then
        local _, cssH, l, r, t, b = WebTouchScreen()
        local unit = H / max(1, cssH or 1)
        insetL, insetR, insetT, insetB = (l or 0) * unit, (r or 0) * unit, (t or 0) * unit, (b or 0) * unit
    end
    local s = min(1, (W - insetL - insetR - PLATE - EDGE) / MENU_W, (H - insetT - insetB - TOP_CLEAR - BOTTOM_CLEAR) / MENU_H)
    menu:SetScale(s)
    menu:ClearAllPoints()
    if GetCVar("webTouchLeft") == "1" then
        menu:SetPoint("TOPRIGHT", UIParent, "TOPRIGHT", -(insetR + EDGE) / s, -(insetT + TOP_CLEAR) / s)
    else
        menu:SetPoint("TOPLEFT", UIParent, "TOPLEFT", (insetL + EDGE) / s, -(insetT + TOP_CLEAR) / s)
    end
end
hooksecurefunc("UpdateUIPanelPositions", function()
    if menu:IsShown() then PlaceMenu() end
end)
menu:SetScript("OnShow", PlaceMenu)

local function Open()
    ReadChannel()
    Refresh()
    ShowUIPanel(menu)
    -- Refused as a panel: shown anyway, though the stick keeps its corner.
    if not menu:IsShown() then menu:Show() end
end

menu:RegisterEvent("CHAT_MSG_WHISPER")
menu:RegisterEvent("CHAT_MSG_WHISPER_INFORM")
menu:RegisterEvent("PARTY_MEMBERS_CHANGED")
menu:RegisterEvent("PLAYER_GUILD_UPDATE")
menu:SetScript("OnEvent", function(self, event, _, name)
    if (event == "CHAT_MSG_WHISPER" or event == "CHAT_MSG_WHISPER_INFORM") and name and name ~= "" then
        for i = #recent, 1, -1 do
            if recent[i] == name then tremove(recent, i) end
        end
        tinsert(recent, 1, name)
        recent[MAX_RECENT + 1] = nil
    end
    if self:IsShown() then Refresh() end
end)

-- ---- The button: a chat bubble in the stock minimap-button ring, on the chat's top corner away from the stick ----

local toggle = CreateFrame("Button", nil, UIParent)
toggle:SetWidth(32)
toggle:SetHeight(32)
toggle:SetScale(1.25)   -- 40 units, about a fingertip
toggle:SetFrameStrata("HIGH")
local background = toggle:CreateTexture(nil, "BACKGROUND")
background:SetTexture("Interface\\Minimap\\UI-Minimap-Background")
background:SetWidth(20)
background:SetHeight(20)
background:SetPoint("TOPLEFT", 7, -5)
local icon = toggle:CreateTexture(nil, "ARTWORK")
icon:SetTexture("Interface\\GossipFrame\\GossipGossipIcon")
icon:SetWidth(16)
icon:SetHeight(16)
icon:SetPoint("TOPLEFT", 9, -7)
local ring = toggle:CreateTexture(nil, "OVERLAY")
ring:SetTexture("Interface\\Minimap\\MiniMap-TrackingBorder")
ring:SetWidth(53)
ring:SetHeight(53)
ring:SetPoint("TOPLEFT")
toggle:SetHighlightTexture("Interface\\Minimap\\UI-Minimap-ZoomButton-Highlight")
toggle:SetScript("OnClick", function()
    if menu:IsShown() then Close() else Open() end
end)

-- The HUD moves the chat (and, left-handed, mirrors it): the button follows it there, on its far corner.
local function PlaceToggle()
    toggle:ClearAllPoints()
    if GetCVar("webTouchLeft") == "1" then
        toggle:SetPoint("LEFT", ChatFrame1, "TOPLEFT")
    else
        toggle:SetPoint("RIGHT", ChatFrame1, "TOPRIGHT")
    end
end
hooksecurefunc(ChatFrame1, "SetPoint", PlaceToggle)
PlaceToggle()
)lua";

}

void WebQuickChatInitialize() {
    static bool registered;
    if (!registered) {
        registered = true;
        // Kept in this browser's Config.wtf, like the Touch options: /run SetCVar("webQuickChat", "inc;ty;...") edits them.
        CVar::Register("webQuickChat", "Touch: quick chat phrases, ; between them", 0, "inc;ty;lfg;omw;brb;gg;need heals;oom",
            nullptr, 4, false, nullptr, false);
    }

    FrameScript_Execute(kQuickChat, "WebQuickChat.lua", nullptr);
}

#endif
