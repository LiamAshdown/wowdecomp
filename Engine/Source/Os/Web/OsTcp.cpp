#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cstdint>
#include <cstring>
#include <unistd.h>
#include "Engine/Source/Os/OsTcp.h"
#include "Engine/Source/Storm/H/storm.h"

// The OsNet IOCP framework is not used for login or world traffic (those go through WowConnection), so only the address helpers exist.
// A NETADDR is a sockaddr_in laid out by hand: family (2), port (2, big endian), IPv4 address (4, network order).

uint32_t OsNetAddrGetAddress(const NETADDR* addr, uint16_t* port) {
    if (port) {
        *port = static_cast<uint16_t>((addr->data[2] << 8) | addr->data[3]);
    }

    uint32_t address;
    memcpy(&address, &addr->data[4], sizeof(address));

    return address;
}

void OsNetAddrMake(uint32_t address, uint16_t port, NETADDR* addr) {
    memset(addr, 0, sizeof(*addr));
    addr->data[0] = 2; // AF_INET
    addr->data[2] = static_cast<uint8_t>(port >> 8);
    addr->data[3] = static_cast<uint8_t>(port);
    memcpy(&addr->data[4], &address, sizeof(address));
}

const char* OsNetAddrToStr(const NETADDR* addr, char* buffer, uint32_t size) {
    uint16_t port;
    uint32_t address = OsNetAddrGetAddress(addr, &port);
    const uint8_t* ip = reinterpret_cast<const uint8_t*>(&address);

    SStrPrintf(buffer, size, "%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], port);

    return buffer;
}

void OsNetPump(uint32_t timeout) {
    usleep(timeout * 1000);
}

#endif
