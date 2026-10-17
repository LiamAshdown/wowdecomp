#if defined(WOWDECOMP_PLATFORM_WEB)

#include "Engine/Source/ComSat/ComSatNetwork.h"

// Browsers have no UDP sockets, so voice chat transport is inert: nothing binds, sends or receives.
ComSat::Network::SocketPosix::SocketPosix() {
    this->m_socket = static_cast<uintptr_t>(-1);
}

ComSat::Network::SocketPosix::~SocketPosix() {
}

bool ComSat::Network::SocketPosix::Bind(int32_t port) {
    return false;
}

int32_t ComSat::Network::SocketPosix::SendTo(const ComSat::Network::Address& address, const uint8_t* data, uint32_t size) {
    return -1;
}

bool ComSat::Network::SocketPosix::RecvFrom(ComSat::Network::Address& address, uint8_t* data, uint32_t& size) {
    return false;
}

#endif
