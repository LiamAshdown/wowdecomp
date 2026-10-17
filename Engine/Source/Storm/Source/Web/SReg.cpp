#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <pthread.h>
#include <string>
#include "Engine/Source/Storm/Source/SOption.h"
#include "Engine/Source/Storm/H/storm.h"

// The Windows registry is replaced by an in-memory table. HKEY_CURRENT_USER and HKEY_LOCAL_MACHINE share it, so the
// SREG_EXCLUDE_* flags are ignored. ponytail: nothing persists across page loads yet; save/load it to IndexedDB or the
// bridge when settings need to survive a reload.
struct RegValue {
    bool isString;
    uint32_t number;
    std::string text;
};

static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;

static std::map<std::string, RegValue>& Values() {
    static std::map<std::string, RegValue> values;
    return values;
}

int32_t STORMAPI SRegGetBaseKey(uint32_t flags, char* buffer, uint32_t bufferchars) {
    if (flags & SREG_BATTLE_NET) {
        SStrCopy(buffer, "Software\\Battle.net\\", bufferchars);
    } else {
        SStrCopy(buffer, "Software\\Blizzard Entertainment\\", bufferchars);
    }

    return 1;
}

// Registry names are case-insensitive.
static std::string MakeName(const char* keyname, const char* valuename, uint32_t flags) {
    char base[260];
    base[0] = 0;

    if (!(flags & SREG_ABSOLUTE)) {
        SRegGetBaseKey(flags, base, sizeof(base));
    }

    std::string name = std::string(base) + keyname + "\\" + valuename;

    for (size_t i = 0; i < name.size(); i++) {
        name[i] = static_cast<char>(tolower(static_cast<unsigned char>(name[i])));
    }

    return name;
}

static bool Load(const char* keyname, const char* valuename, uint32_t flags, RegValue* out) {
    std::string name = MakeName(keyname, valuename, flags);
    bool found = false;

    pthread_mutex_lock(&s_mutex);

    std::map<std::string, RegValue>::iterator it = Values().find(name);

    if (it != Values().end()) {
        *out = it->second;
        found = true;
    }

    pthread_mutex_unlock(&s_mutex);

    return found;
}

static void Save(const char* keyname, const char* valuename, uint32_t flags, const RegValue& value) {
    std::string name = MakeName(keyname, valuename, flags);

    pthread_mutex_lock(&s_mutex);
    Values()[name] = value;
    pthread_mutex_unlock(&s_mutex);
}

int32_t STORMAPI SRegLoadString(const char* keyname, const char* valuename, uint32_t flags, char* buffer, uint32_t buffersize) {
    if (!keyname || !*keyname || !valuename || !*valuename || !buffer || !buffersize) {
        SErrSetLastError(STORM_ERROR_INVALID_PARAMETER);
        return 0;
    }

    RegValue value;

    if (!Load(keyname, valuename, flags, &value)) {
        return 0;
    }

    if (value.isString) {
        SStrCopy(buffer, value.text.c_str(), buffersize);
    } else {
        SStrPrintf(buffer, buffersize, "%u", value.number);
    }

    return 1;
}

int32_t STORMAPI SRegLoadValue(const char* keyname, const char* valuename, uint32_t flags, uint32_t* value) {
    if (!keyname || !*keyname || !valuename || !*valuename || !value) {
        SErrSetLastError(STORM_ERROR_INVALID_PARAMETER);
        return 0;
    }

    RegValue stored;

    if (!Load(keyname, valuename, flags, &stored)) {
        return 0;
    }

    *value = stored.isString ? static_cast<uint32_t>(strtoul(stored.text.c_str(), nullptr, 0)) : stored.number;

    return 1;
}

int32_t STORMAPI SRegSaveString(const char* keyname, const char* valuename, uint32_t flags, const char* string) {
    if (!keyname || !*keyname || !valuename || !*valuename || !string) {
        SErrSetLastError(STORM_ERROR_INVALID_PARAMETER);
        return 0;
    }

    RegValue value;
    value.isString = true;
    value.number = 0;
    value.text = string;
    Save(keyname, valuename, flags, value);

    return 1;
}

int32_t STORMAPI SRegSaveValue(const char* keyname, const char* valuename, uint32_t flags, uint32_t number) {
    if (!keyname || !*keyname || !valuename || !*valuename) {
        SErrSetLastError(STORM_ERROR_INVALID_PARAMETER);
        return 0;
    }

    RegValue value;
    value.isString = false;
    value.number = number;
    Save(keyname, valuename, flags, value);

    return 1;
}

#endif
