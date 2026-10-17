#if defined(WOWDECOMP_PLATFORM_WEB)

// The AddOn Library for the browser build: a "Get AddOns" button at character select opens a list of the realm's
// curated addons (the bridge's --addons folder, Tools/Bridge/addons.mjs), each with Install or Remove. The picks are
// this browser's WTF/AddOnLibrary.txt in the bridge's user-data overlay; the bridge lists the picked addons' files under
// Interface/AddOns/ in the manifest, so after a reload the client finds them like any other addon (and the stock
// AddOns list enables and disables them).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <emscripten.h>
#include "Engine/Source/FrameScript/FrameScript.h"
#include "Engine/Source/lua-5.1/src/lua.h"

EM_JS_DEPS(WebAddOns, "$stringToNewUTF8,$UTF8ToString");

// The catalog (GET /addons/library.txt), malloc'd, or null when the bridge cannot be reached. Sync XHR is allowed on the
// game thread (a worker).
EM_JS(char*, WebAddOnsFetchCatalog, (), {
    var xhr = new XMLHttpRequest();
    xhr.open("GET", "/addons/library.txt", false);
    try {
        xhr.send();
    } catch (e) {
        return 0;
    }
    return xhr.status === 200 ? stringToNewUTF8(xhr.responseText) : 0;
});

// Replaces the install list in the user-data overlay; returns the HTTP status, or -1. The header is what keeps other
// sites from writing there (see WebVfsPut).
EM_JS(int, WebAddOnsPutList, (const char* text), {
    var xhr = new XMLHttpRequest();
    xhr.open("PUT", "/userdata/WTF/AddOnLibrary.txt", false);
    xhr.setRequestHeader("X-Wow-Client", "1");
    try {
        xhr.send(UTF8ToString(text));
    } catch (e) {
        return -1;
    }
    return xhr.status;
});

namespace {

const char* const kFields[] = { "name", "installed", "bytes", "title", "version", "author", "notes" };
const int kFieldCount = sizeof(kFields) / sizeof(kFields[0]);

// rows = WebAddOnLibrary(): an array of { name, installed, bytes, title, version, author, notes }, or nil when the
// bridge cannot be reached.
int32_t Script_WebAddOnLibrary(lua_State* L) {
    char* reply = WebAddOnsFetchCatalog();

    if (!reply) {
        lua_pushnil(L);
        return 1;
    }

    std::string text(reply);
    free(reply);
    lua_createtable(L, 0, 0);
    int row = 0;

    for (size_t pos = 0; pos < text.size();) {
        size_t end = text.find('\n', pos);
        end = end == std::string::npos ? text.size() : end;
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;

        if (line.empty()) {
            continue;
        }

        lua_createtable(L, 0, kFieldCount);

        for (int field = 0, start = 0; field < kFieldCount; field++) {
            size_t tab = field == kFieldCount - 1 ? std::string::npos : line.find('\t', start);
            std::string value = line.substr(start, tab == std::string::npos ? std::string::npos : tab - start);
            start = tab == std::string::npos ? static_cast<int>(line.size()) : static_cast<int>(tab) + 1;

            if (field == 1) {
                lua_pushboolean(L, value == "1");
            } else if (field == 2) {
                lua_pushnumber(L, strtod(value.c_str(), nullptr));
            } else {
                lua_pushstring(L, value.c_str());
            }

            lua_setfield(L, -2, kFields[field]);
        }

        lua_rawseti(L, -2, ++row);
    }

    return 1;
}

// ok = WebSetAddOnLibrary(names): replaces the install list with the names in the array. Takes effect on reload.
int32_t Script_WebSetAddOnLibrary(lua_State* L) {
    std::string text;

    for (int i = 1; lua_type(L, 1) == LUA_TTABLE; i++) {
        lua_rawgeti(L, 1, i);
        const char* name = lua_isstring(L, -1) ? lua_tolstring(L, -1, nullptr) : nullptr;

        if (name) {
            text += name;
            text += '\n';
        }

        lua_pop(L, 1);

        if (!name) {
            break;
        }
    }

    int status = WebAddOnsPutList(text.c_str());

    if (status != 204) {
        printf("[WebAddOns] saving the install list failed (HTTP %d)\n", status);
    }

    lua_pushboolean(L, status == 204);
    return 1;
}

// Reloads the page, the browser build's restart (the client only looks for addons when it starts).
int32_t Script_WebReload(lua_State* L) {
    MAIN_THREAD_ASYNC_EM_ASM({ location.reload(); });
    return 0;
}

// CharacterSelect* and AddonList come from the stock GlueXML.
const char kLibraryUi[] = R"lua(
if not CharacterSelectAddonsButton or WebAddOnLibraryButton then return end

local ok, err = pcall(function()

local ROWS, ROW_HEIGHT, TOP = 9, 40, -108

local dialog = CreateFrame("Frame", "WebAddOnLibrary", GlueParent)
dialog:SetAllPoints(GlueParent)
dialog:SetFrameStrata("DIALOG")
dialog:EnableMouse(true)
dialog:EnableKeyboard(true)
dialog:EnableMouseWheel(true)
dialog:Hide()

local shade = dialog:CreateTexture(nil, "BACKGROUND")
shade:SetAllPoints(dialog)
shade:SetTexture(0, 0, 0, 0.75)

local box = CreateFrame("Frame", "WebAddOnLibraryBox", dialog)
box:SetWidth(600)
box:SetHeight(-TOP + ROWS * ROW_HEIGHT + 100)
box:SetPoint("CENTER")
box:SetBackdrop({
    bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background", edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
    tile = true, tileSize = 32, edgeSize = 32, insets = { left = 11, right = 12, top = 12, bottom = 11 },
})

local title = box:CreateFontString(nil, "ARTWORK", "GlueFontNormalLarge")
title:SetPoint("TOP", 0, -22)
title:SetText("AddOn Library")

local hint = box:CreateFontString(nil, "ARTWORK", "GlueFontHighlightSmall")
hint:SetPoint("TOP", title, "BOTTOM", 0, -8)
hint:SetText("AddOns picked here are installed for this browser. Reload to apply changes.")

local empty = box:CreateFontString(nil, "ARTWORK", "GlueFontHighlight")
empty:SetPoint("CENTER", 0, 20)

local page = box:CreateFontString(nil, "ARTWORK", "GlueFontHighlightSmall")
page:SetPoint("BOTTOMLEFT", 32, 74)

local status = box:CreateFontString(nil, "ARTWORK", "GlueFontHighlightSmall")
status:SetPoint("BOTTOMRIGHT", -32, 74)

-- addons: the whole catalog; shown: the ones matching the search box.
local addons, shown, offset, changed, emptyText = {}, {}, 0, false, ""
local rows = {}
local Update

local function Size(bytes)
    if bytes >= 1048576 then return string.format("%.1f MB", bytes / 1048576) end
    return string.format("%d KB", math.max(1, math.floor(bytes / 1024 + 0.5)))
end

-- Saves right away, so nothing is lost if the player closes the tab instead of reloading.
local function Toggle(addon)
    addon.installed = not addon.installed
    local names = {}
    for _, a in ipairs(addons) do if a.installed then names[#names + 1] = a.name end end
    if WebSetAddOnLibrary(names) then
        changed = true
        status:SetTextColor(0.3, 1, 0.3)
        status:SetText("Saved. Reload to apply.")
    else
        addon.installed = not addon.installed
        status:SetTextColor(1, 0.3, 0.3)
        status:SetText("Could not save. Is the bridge running?")
    end
    Update()
end

for i = 1, ROWS do
    local row = CreateFrame("Frame", nil, box)
    row:SetHeight(ROW_HEIGHT)
    row:SetPoint("TOPLEFT", 30, TOP - (i - 1) * ROW_HEIGHT)
    row:SetPoint("TOPRIGHT", -30, TOP - (i - 1) * ROW_HEIGHT)

    row.title = row:CreateFontString(nil, "ARTWORK", "GlueFontNormal")
    row.title:SetPoint("TOPLEFT", 4, -4)
    row.title:SetWidth(390)
    row.title:SetHeight(16)
    row.title:SetJustifyH("LEFT")

    row.notes = row:CreateFontString(nil, "ARTWORK", "GlueFontHighlightSmall")
    row.notes:SetPoint("TOPLEFT", row.title, "BOTTOMLEFT", 0, -2)
    row.notes:SetWidth(390)
    row.notes:SetHeight(14)
    row.notes:SetJustifyH("LEFT")

    row.button = CreateFrame("Button", "WebAddOnLibraryRow" .. i .. "Button", row, "GlueButtonSmallTemplate")
    row.button:SetWidth(104)
    row.button:SetHeight(30)
    row.button:SetPoint("RIGHT", -2, 0)
    row.button:SetScript("OnClick", function() PlaySound("gsTitleOptionOK") Toggle(row.addon) end)

    row.size = row:CreateFontString(nil, "ARTWORK", "GlueFontHighlightSmall")
    row.size:SetPoint("RIGHT", row.button, "LEFT", -8, 0)
    row.size:SetTextColor(0.6, 0.6, 0.6)
    rows[i] = row
end

local reload = CreateFrame("Button", "WebAddOnLibraryReload", box, "GlueButtonSmallTemplate")
reload:SetWidth(150)
reload:SetHeight(35)
reload:SetPoint("BOTTOMRIGHT", box, "BOTTOM", -6, 24)
reload:SetText("Reload Now")
reload:SetScript("OnClick", function() PlaySound("gsTitleOptionOK") WebReload() end)

local close = CreateFrame("Button", "WebAddOnLibraryClose", box, "GlueButtonSmallTemplate")
close:SetWidth(150)
close:SetHeight(35)
close:SetText("Close")
close:SetScript("OnClick", function() PlaySound("gsTitleOptionExit") dialog:Hide() end)

local search = CreateFrame("EditBox", "WebAddOnLibrarySearch", box)
search:SetWidth(240)
search:SetHeight(28)
search:SetPoint("TOPLEFT", 26, -72)
search:SetFontObject(AccountLoginAccountEdit:GetFontObject())
search:SetTextInsets(10, 6, 0, 0)
search:SetAutoFocus(false)
search:SetMaxLetters(32)
search:SetBackdrop({
    bgFile = "Interface\\Glues\\Common\\Glue-Tooltip-Background", edgeFile = "Interface\\Glues\\Common\\Glue-Tooltip-Border",
    tile = true, tileSize = 16, edgeSize = 16, insets = { left = 10, right = 5, top = 4, bottom = 9 },
})
local placeholder = search:CreateFontString(nil, "ARTWORK", "GlueFontHighlightSmall")
placeholder:SetPoint("LEFT", 12, 0)
placeholder:SetTextColor(0.5, 0.5, 0.5)
placeholder:SetText("Search")

-- Lower-case text without colour codes, for matching the search box.
local function Plain(text)
    return string.lower((text:gsub("|c%x%x%x%x%x%x%x%x", ""):gsub("|r", "")))
end

local function Filter()
    local query = string.lower(search:GetText() or "")
    shown = {}
    for _, a in ipairs(addons) do
        if query == "" or string.find(Plain(a.title .. " " .. a.name .. " " .. a.notes .. " " .. a.author), query, 1, true) then
            shown[#shown + 1] = a
        end
    end
    offset = 0
end

search:SetScript("OnTextChanged", function(self)
    if self:GetText() == "" then placeholder:Show() else placeholder:Hide() end
    Filter()
    Update()
end)
search:SetScript("OnEscapePressed", function(self) self:ClearFocus() end)
search:SetScript("OnEnterPressed", function(self) self:ClearFocus() end)

function Update()
    offset = math.max(0, math.min(offset, #shown - ROWS))
    for i, row in ipairs(rows) do
        local addon = shown[offset + i]
        row.addon = addon
        if addon then
            -- Some titles already carry the version (Questie's does).
            local shows = addon.version == "" or string.find(Plain(addon.title), string.lower(addon.version), 1, true)
            local version = shows and "" or ("  |cff999999" .. addon.version .. "|r")
            row.title:SetText(addon.title .. version)
            local notes = addon.notes
            if addon.author ~= "" then notes = (notes ~= "" and notes .. "  " or "") .. "|cff999999by " .. addon.author .. "|r" end
            row.notes:SetText(notes)
            row.size:SetText(Size(addon.bytes))
            row.button:SetText(addon.installed and "Remove" or "Install")
            row:Show()
        else
            row:Hide()
        end
    end
    page:SetText(#shown > ROWS and string.format("%d-%d of %d  (scroll for more)", offset + 1, math.min(offset + ROWS, #shown), #shown) or "")
    empty:SetText(#addons > 0 and #shown == 0 and "No AddOns match the search." or emptyText)
    -- Close sits centred until there is something to reload, then beside Reload Now.
    close:ClearAllPoints()
    if changed then
        reload:Show()
        close:SetPoint("BOTTOMLEFT", box, "BOTTOM", 6, 24)
    else
        reload:Hide()
        close:SetPoint("BOTTOM", box, "BOTTOM", 0, 24)
    end
end

dialog:SetScript("OnMouseWheel", function(self, delta)
    offset = offset - delta
    Update()
end)
dialog:SetScript("OnKeyDown", function(self, key)
    if key == "ESCAPE" then dialog:Hide() end
end)
dialog:SetScript("OnShow", function()
    status:SetText("")
    local list = WebAddOnLibrary()
    addons = list or {}
    emptyText = not list and "Cannot reach the AddOn Library. Is the bridge running?" or #addons == 0 and "The AddOn Library is empty." or ""
    search:SetText("")
    Filter()
    Update()
end)

-- Above the stock AddOns button (shown only when there are addons), in the same style.
local button = CreateFrame("Button", "WebAddOnLibraryButton", CharacterSelectAddonsButton:GetParent(), "GlueButtonSmallTemplate")
button:SetWidth(CharacterSelectAddonsButton:GetWidth())
button:SetHeight(CharacterSelectAddonsButton:GetHeight())
button:SetPoint("BOTTOM", CharacterSelectAddonsButton, "TOP", 0, 2)
button:SetText("Get AddOns")
button:SetScript("OnClick", function() PlaySound("gsCharacterSelectionAcctOptions") dialog:Show() end)
button:SetScript("OnUpdate", function(self) CharacterSelect_DeathKnightSwap(self) end)
end)

if not ok then WebLog("addon library setup failed: " .. tostring(err)) end
)lua";

}

void WebAddOnsInitialize() {
    FrameScript_RegisterFunction("WebAddOnLibrary", Script_WebAddOnLibrary);
    FrameScript_RegisterFunction("WebSetAddOnLibrary", Script_WebSetAddOnLibrary);
    FrameScript_RegisterFunction("WebReload", Script_WebReload);

    FrameScript_Execute(kLibraryUi, "WebAddOns.lua", nullptr);
}

#endif
