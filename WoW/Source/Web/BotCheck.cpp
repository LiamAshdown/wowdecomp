#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cstdint>
#include "Engine/Source/Base/CDataStore.h"
#include "WoW/Source/Client.h"
#include "WoW/Source/Net/Types.h"

// Bot checks are inert on the web build: there is no module to load and nothing to scan.
void AdditionalCheck1() {
}

void InitTokenSeed(const uint8_t* key) {
}

int32_t Packet_SMSG_CHECK_FOR_BOTS(void* param, NETMESSAGE msgId, uint32_t time, CDataStore* msg) {
    uint8_t key[16];
    msg->GetArray(key, sizeof(key));

    return 1;
}

int32_t GatherAdditionalChecks(uint8_t* check1, uint8_t* check2, uint8_t* check3) {
    *check1 = 0;
    *check2 = 0;
    *check3 = 0;

    return 0;
}

void LoadEncryptedModule(const uint8_t* key, uint32_t length, void (*callback)()) {
}

#endif
