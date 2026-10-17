#if defined(WOWDECOMP_PLATFORM_WEB)

// In-client account creation and management for the browser build.
//
// The login screen gets a "Create Account" button and its "Manage Account" button opens a sign-in dialog that leads to
// the account's details with Change Password and Change Email actions (the Lua below, run once GlueXML has loaded).
// The work happens against the bridge's /account service (Tools/Bridge/account.mjs): registration sends the SRP6 salt
// and verifier computed here, and signing in proves the password with the same SRP6 exchange the login uses, which
// returns a short-lived token for the follow-up changes. The password itself never leaves the client.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <emscripten.h>
#include "Engine/Source/BlizzardCore/Source/Packages/Util/SHA1.h"
#include "Engine/Source/BlizzardCore/Source/System/File/Web/WebVfs.h"
#include "Engine/Source/FrameScript/FrameScript.h"
#include "Engine/Source/Os/OsSecureRandom.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/lua-5.1/src/lua.h"
#include "WoW/Source/WowServices/BigInteger.h"
#include "WoW/Source/WowServices/SRP6_Client.h"
#include "WoW/Source/WowServices/SRP6_Random.h"

EM_JS_DEPS(WebAccount, "$stringToNewUTF8,$UTF8ToString");

// Synchronous POST to the bridge from the game thread (a worker, where sync XHR is allowed). Returns the reply text
// (malloc'd) or null when the bridge cannot be reached.
EM_JS(char*, WebAccountPost, (const char* path, const char* body), {
    var xhr = new XMLHttpRequest();
    xhr.open("POST", UTF8ToString(path), false);
    xhr.setRequestHeader("Content-Type", "application/x-www-form-urlencoded");
    xhr.setRequestHeader("X-Wow-Client", "1");
    try {
        xhr.send(UTF8ToString(body));
    } catch (e) {
        return 0;
    }
    return stringToNewUTF8(xhr.responseText || "");
});

namespace {

// The WoW SRP6 group (little-endian), as sent by the auth server in the logon challenge.
const uint8_t kPrime[32] = {
    0xB7, 0x9B, 0x3E, 0x2A, 0x87, 0x82, 0x3C, 0xAB, 0x8F, 0x5E, 0xBF, 0xBF, 0x8E, 0xB1, 0x01, 0x08,
    0x53, 0x50, 0x06, 0x29, 0x8B, 0x5B, 0xAD, 0xBD, 0x5B, 0x53, 0xE1, 0x89, 0x5E, 0x64, 0x4B, 0x89,
};
const uint8_t kGenerator[1] = { 7 };

std::string Hex(const uint8_t* data, size_t size) {
    static const char kDigits[] = "0123456789abcdef";
    std::string out;

    for (size_t i = 0; i < size; i++) {
        out += kDigits[data[i] >> 4];
        out += kDigits[data[i] & 0xF];
    }

    return out;
}

bool Unhex(const std::string& text, uint8_t* out, size_t size) {
    if (text.size() != size * 2) {
        return false;
    }

    for (size_t i = 0; i < size; i++) {
        char pair[3] = { text[i * 2], text[i * 2 + 1], 0 };
        char* end;
        out[i] = static_cast<uint8_t>(strtoul(pair, &end, 16));

        if (*end) {
            return false;
        }
    }

    return true;
}

std::string UrlEncode(const std::string& text) {
    std::string out;

    for (unsigned char c : text) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '@') {
            out += static_cast<char>(c);
        } else {
            char escaped[4];
            snprintf(escaped, sizeof(escaped), "%%%02X", c);
            out += escaped;
        }
    }

    return out;
}

std::string Upper(const char* text) {
    std::vector<char> buffer(text, text + strlen(text) + 1);
    SStrUpper(buffer.data());
    return buffer.data();
}

// Salt and verifier as AzerothCore stores them: x = SHA1(salt | SHA1(USER:PASS)), v = g^x mod N, both little-endian.
void MakeRegistration(const std::string& user, const std::string& password, uint8_t* salt, uint8_t* verifier) {
    OsSecureRandom(salt, 32);

    SHA1_CONTEXT context;
    uint8_t credentials[20];
    SHA1_Init(&context);
    SHA1_Update(&context, reinterpret_cast<const uint8_t*>(user.data()), static_cast<uint32_t>(user.size()));
    SHA1_Update(&context, reinterpret_cast<const uint8_t*>(":"), 1);
    SHA1_Update(&context, reinterpret_cast<const uint8_t*>(password.data()), static_cast<uint32_t>(password.size()));
    SHA1_Final(credentials, &context);

    uint8_t xHash[20];
    SHA1_Init(&context);
    SHA1_Update(&context, salt, 32);
    SHA1_Update(&context, credentials, sizeof(credentials));
    SHA1_Final(xHash, &context);

    BigData* N = BigIntegerFromBytes(kPrime, sizeof(kPrime));
    BigData* g = BigIntegerFromBytes(kGenerator, sizeof(kGenerator));
    BigData* x = BigIntegerFromBytes(xHash, sizeof(xHash));
    BigData* v = BigIntegerFromInt(0);
    BigIntegerModExp(v, g, x, N);

    memset(verifier, 0, 32);
    BigIntegerToBytes(v, verifier, 32);

    BigIntegerFree(v);
    BigIntegerFree(x);
    BigIntegerFree(g);
    BigIntegerFree(N);
}

// Splits a bridge reply ("OK"/"ERR", then lines) into its lines; false if the bridge could not be reached.
bool Request(const char* path, const std::string& body, std::vector<std::string>& lines) {
    char* reply = WebAccountPost(path, body.c_str());

    if (!reply) {
        return false;
    }

    std::string text(reply);
    free(reply);
    lines.clear();

    for (size_t pos = 0; pos <= text.size();) {
        size_t end = text.find('\n', pos);
        end = end == std::string::npos ? text.size() : end;
        lines.push_back(text.substr(pos, end - pos));
        pos = end + 1;
    }

    return !lines.empty() && (lines[0] == "OK" || lines[0] == "ERR");
}

int32_t Result(lua_State* L, bool ok, const std::string& message) {
    lua_pushboolean(L, ok);
    lua_pushstring(L, message.c_str());
    return 2;
}

const char kUnreachable[] = "Cannot reach the account service. Is the bridge running?";

bool ValidCredentials(const char* user, const char* password, std::string& error) {
    size_t userLength = strlen(user);
    size_t passwordLength = strlen(password);

    if (userLength < 3 || userLength > 16) {
        error = "Account names are 3 to 16 letters or digits.";
        return false;
    }

    for (size_t i = 0; i < userLength; i++) {
        if (!isalnum(static_cast<unsigned char>(user[i]))) {
            error = "Account names are 3 to 16 letters or digits.";
            return false;
        }
    }

    if (passwordLength < 1 || passwordLength > 16) {
        error = "Passwords are 1 to 16 characters.";
        return false;
    }

    return true;
}

// ok, message = WebCreateAccount(accountName, password, email)
int32_t Script_WebCreateAccount(lua_State* L) {
    const char* user = lua_tolstring(L, 1, nullptr);
    const char* password = lua_tolstring(L, 2, nullptr);
    const char* email = lua_tolstring(L, 3, nullptr);
    std::string error;

    if (!user || !password || !ValidCredentials(user, password, error)) {
        return Result(L, false, error.empty() ? "Enter an account name and password." : error);
    }

    uint8_t salt[32];
    uint8_t verifier[32];
    std::string upperUser = Upper(user);
    MakeRegistration(upperUser, Upper(password), salt, verifier);

    std::string body = "user=" + UrlEncode(upperUser) + "&salt=" + Hex(salt, 32) + "&verifier=" + Hex(verifier, 32) + "&email=" + UrlEncode(email ? email : "");
    std::vector<std::string> reply;

    if (!Request("/account/register", body, reply)) {
        return Result(L, false, kUnreachable);
    }

    return Result(L, reply[0] == "OK", reply.size() > 1 ? reply[1] : "");
}

// The eight detail lines of a bridge reply (from `first`) as a Lua array table; one nil for a short reply.
void PushDetails(lua_State* L, const std::vector<std::string>& lines, size_t first) {
    lua_createtable(L, 8, 0);

    for (size_t i = 0; i < 8 && first + i < lines.size(); i++) {
        lua_pushstring(L, lines[first + i].c_str());
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
}

// ok, tokenOrMessage, details = WebAccountLogin(accountName, password)
// Proves the password with SRP6 (the same exchange the login does) and gets back a five minute token and the details.
int32_t Script_WebAccountLogin(lua_State* L) {
    const char* user = lua_tolstring(L, 1, nullptr);
    const char* password = lua_tolstring(L, 2, nullptr);
    std::string error;

    if (!user || !password || !ValidCredentials(user, password, error)) {
        return Result(L, false, error.empty() ? "Enter an account name and password." : error);
    }

    std::string upperUser = Upper(user);
    std::vector<std::string> challenge;

    if (!Request("/account/challenge", "user=" + UrlEncode(upperUser), challenge)) {
        return Result(L, false, kUnreachable);
    }

    uint8_t salt[32];
    uint8_t serverPublicKey[32];

    if (challenge[0] != "OK") {
        return Result(L, false, challenge.size() > 1 ? challenge[1] : "");
    }

    if (challenge.size() < 4 || !Unhex(challenge[1], salt, 32) || !Unhex(challenge[2], serverPublicKey, 32)) {
        return Result(L, false, "The account service sent a malformed reply.");
    }

    SRP6_Client client;
    client.BeginAuthentication(upperUser.c_str(), Upper(password).c_str());

    uint8_t seed[16];
    OsSecureRandom(seed, sizeof(seed));
    SRP6_Random random(seed, sizeof(seed));

    if (client.CalculateProof(kPrime, sizeof(kPrime), kGenerator, sizeof(kGenerator), salt, sizeof(salt), serverPublicKey, sizeof(serverPublicKey), random)) {
        return Result(L, false, "The account name or password is incorrect.");
    }

    std::string body = "id=" + UrlEncode(challenge[3]) + "&a=" + Hex(client.m_publicKey, 32) + "&m1=" + Hex(client.m_clientProof, 20);
    std::vector<std::string> reply;

    if (!Request("/account/login", body, reply)) {
        return Result(L, false, kUnreachable);
    }

    if (reply[0] != "OK" || reply.size() < 10) {
        return Result(L, false, reply.size() > 1 && reply[0] != "OK" ? reply[1] : "The account service sent a malformed reply.");
    }

    lua_pushboolean(L, true);
    lua_pushstring(L, reply[1].c_str());
    PushDetails(L, reply, 2);
    return 3;
}

// ok, messageOrNothing, details = WebAccountInfo(token)
int32_t Script_WebAccountInfo(lua_State* L) {
    const char* token = lua_tolstring(L, 1, nullptr);
    std::vector<std::string> reply;

    if (!token || !Request("/account/info", "token=" + UrlEncode(token), reply)) {
        return Result(L, false, kUnreachable);
    }

    if (reply[0] != "OK" || reply.size() < 9) {
        return Result(L, false, reply.size() > 1 && reply[0] != "OK" ? reply[1] : "The account service sent a malformed reply.");
    }

    lua_pushboolean(L, true);
    lua_pushstring(L, "");
    PushDetails(L, reply, 1);
    return 3;
}

// ok, message = WebChangeEmail(token, email)
int32_t Script_WebChangeEmail(lua_State* L) {
    const char* token = lua_tolstring(L, 1, nullptr);
    const char* email = lua_tolstring(L, 2, nullptr);
    std::vector<std::string> reply;

    if (!token || !email || !*email) {
        return Result(L, false, "Enter an email address.");
    }

    if (!Request("/account/email", "token=" + UrlEncode(token) + "&email=" + UrlEncode(email), reply)) {
        return Result(L, false, kUnreachable);
    }

    return Result(L, reply[0] == "OK", reply.size() > 1 ? reply[1] : "");
}

// ok, message = WebChangePassword(accountName, token, newPassword)
// The token (from WebAccountLogin) authorises it; only the new salt and verifier are sent.
int32_t Script_WebChangePassword(lua_State* L) {
    const char* user = lua_tolstring(L, 1, nullptr);
    const char* token = lua_tolstring(L, 2, nullptr);
    const char* newPassword = lua_tolstring(L, 3, nullptr);
    std::string error;

    if (!user || !token || !newPassword || !ValidCredentials(user, newPassword, error)) {
        return Result(L, false, error.empty() ? "Fill in every field." : error);
    }

    uint8_t salt[32];
    uint8_t verifier[32];
    std::string upperUser = Upper(user);
    MakeRegistration(upperUser, Upper(newPassword), salt, verifier);

    std::string body = "user=" + UrlEncode(upperUser) + "&token=" + UrlEncode(token) + "&salt=" + Hex(salt, 32) + "&verifier=" + Hex(verifier, 32);
    std::vector<std::string> reply;

    if (!Request("/account/password", body, reply)) {
        return Result(L, false, kUnreachable);
    }

    return Result(L, reply[0] == "OK", reply.size() > 1 ? reply[1] : "");
}

// Lua errors from the account UI go to the page log.
int32_t Script_WebLog(lua_State* L) {
    printf("[WebAccount] %s\n", lua_tolstring(L, 1, nullptr));
    return 0;
}

// The Fullscreen button: the page switches (index.html), within the click's user activation.
int32_t Script_WebToggleFullscreen(lua_State* L) {
    MAIN_THREAD_ASYNC_EM_ASM({
        if (window.wowToggleFullscreen) window.wowToggleFullscreen();
    });
    return 0;
}

// The account dialog and the login screen changes. AccountLogin* and TokenEnterDialog* come from the stock GlueXML.
const char kAccountUi[] = R"lua(
if not AccountLoginUI or WebAccountCreateButton then return end

local ok, err = pcall(function()

local function CopyBackdrop(frame, source, fallback)
    local backdrop = source and source.GetBackdrop and source:GetBackdrop()
    frame:SetBackdrop(backdrop or fallback)
    if backdrop then
        frame:SetBackdropColor(source:GetBackdropColor())
        frame:SetBackdropBorderColor(source:GetBackdropBorderColor())
    end
end

local MODES = {
    create = {
        title = "Create Account", accept = "Create", cancel = "Cancel",
        labels = { "Account Name", "Password", "Confirm Password", "Email (optional)" },
        password = { false, true, true, false }, maxLetters = { 16, 16, 16, 128 },
    },
    signin = {
        title = "Manage Account", accept = "Sign In", cancel = "Cancel",
        labels = { "Account Name", "Password" },
        password = { false, true }, maxLetters = { 16, 16 },
    },
    details = { title = "Account", accept = "Change Password", cancel = "Close", labels = {}, password = {}, maxLetters = {} },
    password = {
        title = "Change Password", accept = "Change", cancel = "Back",
        labels = { "New Password", "Confirm New Password" },
        password = { true, true }, maxLetters = { 16, 16 },
    },
    email = {
        title = "Change Email", accept = "Save", cancel = "Back",
        labels = { "New Email Address" },
        password = { false }, maxLetters = { 128 },
    },
}

local DETAIL_LABELS = { "Account", "Email", "Joined", "Last Login", "Last IP", "Expansion", "Ban Status", "Characters" }

local dialog = CreateFrame("Frame", "WebAccountDialog", GlueParent)
dialog:SetAllPoints(GlueParent)
dialog:SetFrameStrata("DIALOG")
dialog:EnableMouse(true)
dialog:EnableKeyboard(true)
dialog:Hide()

local shade = dialog:CreateTexture(nil, "BACKGROUND")
shade:SetAllPoints(dialog)
shade:SetTexture(0, 0, 0, 0.6)

local box = CreateFrame("Frame", "WebAccountDialogBox", dialog)
box:SetWidth(380)
box:SetHeight(420)
box:SetPoint("CENTER")
CopyBackdrop(box, TokenEnterDialogBackground, {
    bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background", edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
    tile = true, tileSize = 32, edgeSize = 32, insets = { left = 11, right = 12, top = 12, bottom = 11 },
})

do
    local r, g, b = box:GetBackdropColor()
    box:SetBackdropColor(r or 0, g or 0, b or 0, 1)
end

local title = box:CreateFontString(nil, "ARTWORK", "GlueFontNormalLarge")
title:SetPoint("TOP", 0, -22)

local status = box:CreateFontString(nil, "ARTWORK", "GlueFontHighlightSmall")
status:SetPoint("BOTTOMLEFT", 30, 66)
status:SetPoint("BOTTOMRIGHT", -30, 66)
status:SetHeight(28)

local function SetStatus(text, isError)
    status:SetText(text or "")
    if isError then status:SetTextColor(1, 0.3, 0.3) else status:SetTextColor(0.3, 1, 0.3) end
end

local fields = {}
local accept, cancel, emailButton
local ShowMode, ShowDetails

local function Back()
    if dialog.mode == "password" or dialog.mode == "email" then ShowDetails(dialog.info) else dialog:Hide() end
end

-- A request that fails because the token ran out sends the player back to sign in.
local function Failed(message)
    if message:find("expired") then ShowMode("signin", message, true) else SetStatus(message, true) end
end

local function Submit()
    local mode = dialog.mode
    local values = {}
    for i, edit in ipairs(fields) do values[i] = edit:GetText() or "" end
    local ok, message, info

    if mode == "create" then
        if values[2] ~= values[3] then return SetStatus("The passwords do not match.", true) end
        ok, message = WebCreateAccount(values[1], values[2], values[4])
        SetStatus(message, not ok)

        if ok then
            AccountLoginAccountEdit:SetText(string.upper(values[1]))
            AccountLoginPasswordEdit:SetText("")
            for i = 2, #fields do fields[i]:SetText("") end
            cancel:SetText("Close")
        end
    elseif mode == "signin" then
        ok, message, info = WebAccountLogin(values[1], values[2])
        if not ok then return SetStatus(message, true) end
        dialog.token, dialog.user = message, string.upper(values[1])
        ShowDetails(info)
    elseif mode == "password" then
        if values[1] ~= values[2] then return SetStatus("The new passwords do not match.", true) end
        ok, message = WebChangePassword(dialog.user, dialog.token, values[1])
        if not ok then return Failed(message) end
        if string.upper(AccountLoginAccountEdit:GetText() or "") == dialog.user then AccountLoginPasswordEdit:SetText("") end
        ShowDetails(dialog.info, message)
    elseif mode == "email" then
        ok, message = WebChangeEmail(dialog.token, values[1])
        if not ok then return Failed(message) end
        local fresh
        ok, _, fresh = WebAccountInfo(dialog.token)
        ShowDetails(ok and fresh or dialog.info, message)
    end
end

local rows = {}
for i, text in ipairs(DETAIL_LABELS) do
    local label = box:CreateFontString(nil, "ARTWORK", "GlueFontNormal")
    label:SetPoint("TOPLEFT", 34, -58 - (i - 1) * 24)
    label:SetText(text)
    local value = box:CreateFontString(nil, "ARTWORK", "GlueFontHighlight")
    value:SetPoint("TOPLEFT", 140, -58 - (i - 1) * 24)
    value:SetWidth(206)
    value:SetJustifyH("LEFT")
    rows[i] = { label = label, value = value }
end

for i = 1, 4 do
    local label = box:CreateFontString(nil, "ARTWORK", "GlueFontNormal")
    label:SetPoint("TOPLEFT", 34, -52 - (i - 1) * 64)

    local edit = CreateFrame("EditBox", "WebAccountDialogEdit" .. i, box)
    edit:SetWidth(312)
    edit:SetHeight(32)
    edit:SetPoint("TOPLEFT", label, "BOTTOMLEFT", -6, 0)
    edit:SetFontObject(AccountLoginAccountEdit:GetFontObject() or GlueEditBoxFont)
    edit:SetTextInsets(AccountLoginAccountEdit:GetTextInsets())
    edit:SetAutoFocus(false)
    CopyBackdrop(edit, AccountLoginAccountEdit, {
        bgFile = "Interface\\Glues\\Common\\Glue-Tooltip-Background", edgeFile = "Interface\\Glues\\Common\\Glue-Tooltip-Border",
        tile = true, tileSize = 16, edgeSize = 16, insets = { left = 10, right = 5, top = 4, bottom = 9 },
    })
    edit.label = label
    edit:SetScript("OnEscapePressed", Back)
    edit:SetScript("OnEnterPressed", Submit)
    edit:SetScript("OnTabPressed", function(self)
        fields[(i % #MODES[dialog.mode].labels) + 1]:SetFocus()
    end)
    fields[i] = edit
end

accept = CreateFrame("Button", "WebAccountDialogAccept", box, "GlueButtonTemplate")
accept:SetWidth(150)
accept:SetHeight(AccountLoginLoginButton:GetHeight())
accept:SetPoint("BOTTOMRIGHT", box, "BOTTOM", -6, 24)
accept:SetScript("OnClick", function()
    if dialog.mode == "details" then ShowMode("password") else Submit() end
end)

cancel = CreateFrame("Button", "WebAccountDialogCancel", box, "GlueButtonTemplate")
cancel:SetWidth(150)
cancel:SetHeight(AccountLoginLoginButton:GetHeight())
cancel:SetPoint("BOTTOMLEFT", box, "BOTTOM", 6, 24)
cancel:SetScript("OnClick", Back)

dialog:SetScript("OnKeyDown", function(self, key)
    if key == "ESCAPE" then Back() elseif key == "ENTER" and dialog.mode ~= "details" then Submit() end
end)
dialog:SetScript("OnHide", function()
    for _, edit in ipairs(fields) do edit:SetText(""); edit:ClearFocus() end
    dialog.token, dialog.user, dialog.info = nil, nil, nil
    AccountLoginPasswordEdit:SetFocus()
end)

emailButton = CreateFrame("Button", "WebAccountDialogEmail", box, "GlueButtonTemplate")
emailButton:SetWidth(150)
emailButton:SetHeight(AccountLoginLoginButton:GetHeight())
emailButton:SetText("Change Email")
emailButton:SetScript("OnClick", function() ShowMode("email") end)

-- The details view uses two button rows (Change Password / Change Email, then Close) and a higher status line.
local function Layout(details)
    accept:ClearAllPoints()
    cancel:ClearAllPoints()
    emailButton:ClearAllPoints()
    status:ClearAllPoints()
    status:SetPoint("BOTTOMLEFT", 30, details and 104 or 66)
    status:SetPoint("BOTTOMRIGHT", -30, details and 104 or 66)

    if details then
        accept:SetPoint("BOTTOMRIGHT", box, "BOTTOM", -6, 64)
        emailButton:SetPoint("BOTTOMLEFT", box, "BOTTOM", 6, 64)
        cancel:SetPoint("BOTTOM", box, "BOTTOM", 0, 24)
        emailButton:Show()
    else
        accept:SetPoint("BOTTOMRIGHT", box, "BOTTOM", -6, 24)
        cancel:SetPoint("BOTTOMLEFT", box, "BOTTOM", 6, 24)
        emailButton:Hide()
    end
end

function ShowMode(mode, message, isError)
    local m = MODES[mode]
    dialog.mode = mode
    title:SetText(m.title)
    accept:SetText(m.accept)
    cancel:SetText(m.cancel)
    Layout(mode == "details")
    for i, edit in ipairs(fields) do
        edit:SetText("")
        if m.labels[i] then
            edit.label:SetText(m.labels[i])
            edit:SetPassword(m.password[i])
            edit:SetMaxLetters(m.maxLetters[i])
            edit:Show()
            edit.label:Show()
        else
            edit:Hide()
            edit.label:Hide()
        end
    end
    for _, row in ipairs(rows) do
        if mode == "details" then row.label:Show(); row.value:Show() else row.label:Hide(); row.value:Hide() end
    end
    SetStatus(message or "", isError)
    dialog:Show()
    if m.labels[1] then fields[1]:SetFocus() end
end

function ShowDetails(info, message)
    dialog.info = info
    for i, row in ipairs(rows) do
        local text = info and info[i] or ""
        if #text > 28 then text = string.sub(text, 1, 25) .. "..." end
        row.value:SetText(text)
    end
    ShowMode("details", message)
end

-- mode is "create" or "signin" (the Manage Account button); both start from the name typed on the login screen.
function WebAccountDialog_Show(mode)
    ShowMode(mode)
    fields[1]:SetText(AccountLoginAccountEdit:GetText() or "")
    if mode == "signin" and fields[1]:GetText() ~= "" then fields[2]:SetFocus() end
end

-- The login screen's server alert panel (left side) comes from the bridge's server-alert.txt (OsURLDownload, web).
SERVER_ALERT_URL = "/server-alert.txt"

-- The realm's name in place of the WoW logo, over a line of Friz Quadrata. Centred over the news panel, which always
-- sits on the login scene (a wide window pillarboxes the scene, and the logo's own corner can land in the black bar).
-- The title is a texture (Tools/Bridge/make-logo.py) because glyphs are rasterised at 32 px at most
-- (CGxFont::UpdateDimensions), which blurs large text; without it, the same words as text.
AccountLoginLogo:Hide()
local parent = AccountLoginLogo:GetParent()
local title = parent:CreateTexture(nil, "OVERLAY")
local subtitleAnchor = title
if title:SetTexture("Interface\\Glues\\LasthavensLogo") then
    title:SetWidth(360)
    title:SetHeight(90)
    title:SetPoint("BOTTOM", ServerAlertFrame, "TOP", 0, 10)
else
    title:Hide()
    subtitleAnchor = parent:CreateFontString(nil, "OVERLAY")
    subtitleAnchor:SetFont("Fonts\\MORPHEUS.TTF", 32, "OUTLINE")
    subtitleAnchor:SetTextHeight(64)
    subtitleAnchor:SetTextColor(1, 0.82, 0)
    subtitleAnchor:SetShadowColor(0, 0, 0, 1)
    subtitleAnchor:SetShadowOffset(3, -3)
    subtitleAnchor:SetPoint("BOTTOM", ServerAlertFrame, "TOP", 0, 26)
    subtitleAnchor:SetText("Lasthavens")
end
local subtitle = parent:CreateFontString(nil, "OVERLAY")
subtitle:SetFont("Fonts\\FRIZQT__.TTF", 16, "OUTLINE")
subtitle:SetTextColor(0.78, 0.88, 1)
-- The texture has transparent space below the letters.
subtitle:SetPoint("TOP", subtitleAnchor, "BOTTOM", 0, subtitleAnchor == title and 16 or 2)
subtitle:SetText("Wrath of the Lich King")

-- A Fullscreen button above Options, in the same style (Alt+Enter does the same anywhere).
local fullscreen = CreateFrame("Button", "WebFullscreenButton", OptionsButton:GetParent(), "GlueButtonSmallTemplateBlue")
fullscreen:SetPoint("BOTTOM", OptionsButton, "TOP", 0, 2)
fullscreen:SetText("Fullscreen")
fullscreen:SetScript("OnClick", function() PlaySound("gsTitleOptions") WebToggleFullscreen() end)

-- "Community Site" becomes the realm's Discord.
AccountLoginCommunityButton:SetText("Join Discord")
AccountLoginCommunityButton:SetScript("OnClick", function() LaunchURL("https://discord.gg/2EZTV34rfN") end)

-- Login screen: "Manage Account" opens the dialog instead of a web page.
local manage = AccountLoginManageAccountButton
manage:SetScript("OnClick", function() WebAccountDialog_Show("signin") end)

-- "Create Account" sits right above Login, in the red template so a first-time player sees it before anything else.
-- It takes the gap between the password box and Login, where the authenticator box would go (AzerothCore never asks).
local login = AccountLoginLoginButton
local create = CreateFrame("Button", "WebAccountCreateButton", login:GetParent(), "GlueButtonTemplate")
create:SetWidth(login:GetWidth())
create:SetHeight(login:GetHeight() - 5)
create:SetPoint("BOTTOM", login, "TOP", 0, 2)
create:SetText("Create Account")
create:SetScript("OnClick", function() WebAccountDialog_Show("create") end)

-- AzerothCore accounts are names, not Battle.net email logins.
local function Relabel(frame)
    for _, region in ipairs({ frame:GetRegions() }) do
        if region:GetObjectType() == "FontString" then
            local text = region:GetText() or ""
            if text:find("Battle.net") then region:SetText("Account Name")
            elseif text:find("email") then region:SetText("") end
        end
    end
end
Relabel(AccountLoginUI)
Relabel(AccountLoginAccountEdit)
end)

if not ok then WebLog("setup failed: " .. tostring(err)) end

local show = WebAccountDialog_Show
if show then
    WebAccountDialog_Show = function(mode)
        local ok, err = pcall(show, mode)
        if not ok then WebLog("show failed: " .. tostring(err)) end
    end
end
)lua";

}

void WebAccountInitialize() {
    FrameScript_RegisterFunction("WebCreateAccount", Script_WebCreateAccount);
    FrameScript_RegisterFunction("WebAccountLogin", Script_WebAccountLogin);
    FrameScript_RegisterFunction("WebAccountInfo", Script_WebAccountInfo);
    FrameScript_RegisterFunction("WebChangeEmail", Script_WebChangeEmail);
    FrameScript_RegisterFunction("WebChangePassword", Script_WebChangePassword);
    FrameScript_RegisterFunction("WebLog", Script_WebLog);
    FrameScript_RegisterFunction("WebToggleFullscreen", Script_WebToggleFullscreen);

    FrameScript_Execute(kAccountUi, "WebAccount.lua", nullptr);

    // The glue frames are loaded. The page (Tools/Bridge/web/index.html) reads the number of game-data blocks fetched so
    // far to tell when the login scene has finished streaming in, and to show that a download is under way later on.
    MAIN_THREAD_ASYNC_EM_ASM({
        window.wowFetchCount = function() { return HEAP32[$0 >> 2]; };
        if (window.wowFramesLoaded) window.wowFramesLoaded();
    }, WebVfs::FetchCounter());
}

#endif
