#if defined(WOWDECOMP_PLATFORM_WEB)

#include <arpa/inet.h>
#include "WoW/Source/WowServices/BattlenetLogin.h"
#include "Engine/Source/Storm/H/storm.h"
#include "WoW/Source/Battlenet/Tumor/TumorManager.h"

// Only dotted-quad addresses resolve; the browser has no DNS.
uint32_t BattlenetApplication::Resolve(uint32_t token, const char* hostname) {
    if (!this->m_login || !g_tumorManager) {
        SErrDisplayAppFatalMessage("", "", 0);
    }

    Battlenet::SystemClient& client = g_tumorManager->m_tumor;

    char name[256];
    const char* separator = SStrChr(hostname, ':');

    if (separator) {
        int32_t length = separator - hostname;

        if (length >= 255) {
            length = 255;
        }

        SStrCopy(name, hostname, length);
        name[length] = '\0';
    } else {
        SStrCopy(name, hostname, sizeof(name));
    }

    uint32_t ip = inet_addr(name);

    if (ip == INADDR_NONE) {
        BattlenetLogin::LogConnectionStatus("Failed to resolve %s", hostname);

        Battlenet::Client::DNSInfo info;
        info.m_name = "";
        client.Resolved(token, info);
        return 0;
    }

    Battlenet::Client::DNSInfo info;
    info.m_addresses.SetCount(1);

    Battlenet::IP4::Address& address = info.m_addresses[0];
    address.m_value[0] = ip;
    address.m_value[1] = ip >> 8;
    address.m_value[2] = ip >> 16;
    address.m_value[3] = ip >> 24;

    info.m_name = "";
    client.Resolved(token, info);
    return 0;
}

#endif
