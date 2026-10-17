#if defined(WOWDECOMP_PLATFORM_WEB)

#include <windows.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <emscripten.h>
#include <emscripten/websocket.h>
#include <cerrno>
#include <cstdio>
#include <malloc.h>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>
#include "WoW/Source/WowServices/WowConnection.h"
#include "Engine/Source/Base/CDataStore.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Storm/H/storm.h"
#include "WoW/Source/WowServices/WDataStore.h"
#include "WoW/Source/WowServices/WowConnectionNet.h"

// Browser sockets. A "socket" is a small id into a table of Ctx objects; each Ctx owns one emscripten WebSocket to
// ws(s)://<page-host>/tcp/<ip>/<port> (Tools/Bridge relays it to the AzerothCore TCP port).
//
// Threading: emscripten_websocket_* calls from the net/worker pthreads are proxied synchronously to the browser main
// thread, and this emscripten version always runs the on* callbacks there too. The callbacks therefore only append to the
// Ctx buffer / change its state under Ctx::lock and then poke the net thread's event; they never call back into
// WowConnection. No emscripten_websocket_* call is ever made with Ctx::lock held, because the main thread may be spinning
// on that lock inside a callback while the calling pthread waits for the proxied call to finish.
namespace {
    enum SockState { SOCK_CONNECTING, SOCK_OPEN, SOCK_CLOSED, SOCK_FAILED };

    struct Ctx {
        std::mutex lock;
        SockState state = SOCK_CONNECTING;
        EMSCRIPTEN_WEBSOCKET_T ws = 0;
        std::vector<uint8_t> rx;
        size_t rxPos = 0;
        uint32_t address = 0;
        uint16_t port = 0;
    };

    const int32_t MAX_SOCKETS = 1024;

    std::mutex s_tableLock;
    std::shared_ptr<Ctx> s_table[MAX_SOCKETS];
    thread_local int32_t s_webSockLastError;

    bool WebSockWouldBlock() {
        return s_webSockLastError == EWOULDBLOCK;
    }

    bool WebSockInterrupted() {
        return s_webSockLastError == EINTR;
    }

    std::shared_ptr<Ctx> WebSockGet(int32_t sock) {
        if (sock < 0 || sock >= MAX_SOCKETS) {
            return nullptr;
        }

        std::lock_guard<std::mutex> guard(s_tableLock);
        return s_table[sock];
    }

    // Wakes WowConnectionNet::PlatformRun (safe from the main browser thread; it only takes the event's own mutex).
    void WebSockSignal() {
        if (WowConnection::s_network) {
            WowConnection::s_network->PlatformWorkerReady();
        }
    }

    // A socket that already opened and then dies becomes CLOSED, one that never opened becomes FAILED (the equivalent
    // of a refused connect).
    void WebSockSetEnded(Ctx* ctx) {
        {
            std::lock_guard<std::mutex> guard(ctx->lock);
            ctx->state = ctx->state == SOCK_CONNECTING ? SOCK_FAILED : SOCK_CLOSED;
        }

        WebSockSignal();
    }

    bool WebSockOnOpen(int, const EmscriptenWebSocketOpenEvent*, void* userData) {
        Ctx* ctx = static_cast<Ctx*>(userData);

        {
            std::lock_guard<std::mutex> guard(ctx->lock);

            if (ctx->state == SOCK_CONNECTING) {
                ctx->state = SOCK_OPEN;
            }
        }

        WebSockSignal();
        return true;
    }

    bool WebSockOnMessage(int, const EmscriptenWebSocketMessageEvent* event, void* userData) {
        Ctx* ctx = static_cast<Ctx*>(userData);

        if (event->isText || !event->numBytes) {
            return true;
        }

        {
            std::lock_guard<std::mutex> guard(ctx->lock);
            ctx->rx.insert(ctx->rx.end(), event->data, event->data + event->numBytes);
        }

        WebSockSignal();
        return true;
    }

    bool WebSockOnClose(int, const EmscriptenWebSocketCloseEvent*, void* userData) {
        WebSockSetEnded(static_cast<Ctx*>(userData));
        return true;
    }

    bool WebSockOnError(int, const EmscriptenWebSocketErrorEvent*, void* userData) {
        WebSockSetEnded(static_cast<Ctx*>(userData));
        return true;
    }

    const char* WebSockBaseUrl() {
        static const std::string base = [] {
            char buffer[256];
            buffer[0] = 0;
            MAIN_THREAD_EM_ASM({
                var s = new TextEncoder().encode(location.protocol.replace('http', 'ws') + '//' + location.host).subarray(0, $1 - 1);
                HEAPU8.set(s, $0);
                HEAPU8[$0 + s.length] = 0;
            }, buffer, sizeof(buffer));
            return std::string(buffer);
        }();

        return base.c_str();
    }

    int32_t WebSockOpen() {
        std::lock_guard<std::mutex> guard(s_tableLock);

        for (int32_t i = 0; i < MAX_SOCKETS; i++) {
            if (!s_table[i]) {
                s_table[i] = std::make_shared<Ctx>();
                return i;
            }
        }

        return -1;
    }

    void WebSockClose(int32_t sock) {
        std::shared_ptr<Ctx> ctx;

        if (sock >= 0 && sock < MAX_SOCKETS) {
            std::lock_guard<std::mutex> guard(s_tableLock);
            ctx.swap(s_table[sock]);
        }

        if (!ctx) {
            return;
        }

        EMSCRIPTEN_WEBSOCKET_T ws;

        {
            std::lock_guard<std::mutex> guard(ctx->lock);
            ws = ctx->ws;
            ctx->ws = 0;
            ctx->state = ctx->state == SOCK_CONNECTING ? SOCK_FAILED : SOCK_CLOSED;
            ctx->rx.clear();
            ctx->rxPos = 0;
        }

        if (ws > 0) {
            // delete clears the JS handlers on the main thread, so no callback can touch ctx once it returns.
            emscripten_websocket_close(ws, 1000, nullptr);
            emscripten_websocket_delete(ws);
        }
    }

    // Starts the connect. Returns 0 on success, or a non-zero error when it cannot be started. Failures to start the
    // WebSocket surface through WebSockConnectError instead, the same CheckConnect path a refused TCP connect takes
    // on Win64.
    int32_t WebSockConnect(int32_t sock, uint32_t address, uint16_t port) {
        std::shared_ptr<Ctx> ctx = WebSockGet(sock);

        if (!ctx) {
            return EBADF;
        }

        ctx->address = address;
        ctx->port = port;

        // Only dotted IPv4 addresses are relayed; a name that did not parse arrives here as 0.
        if (!address || !port) {
            WebSockSetEnded(ctx.get());
            return 0;
        }

        char url[320];
        snprintf(url, sizeof(url), "%s/tcp/%u.%u.%u.%u/%u", WebSockBaseUrl(), address & 0xFF, (address >> 8) & 0xFF, (address >> 16) & 0xFF, address >> 24, port);

        EmscriptenWebSocketCreateAttributes attributes;
        emscripten_websocket_init_create_attributes(&attributes);
        attributes.url = url;
        attributes.createOnMainThread = true;

        EMSCRIPTEN_WEBSOCKET_T ws = emscripten_websocket_new(&attributes);

        if (ws <= 0) {
            WebSockSetEnded(ctx.get());
            return 0;
        }

        {
            std::lock_guard<std::mutex> guard(ctx->lock);
            ctx->ws = ws;
        }

        // The handlers are installed by separate proxied calls, so an event could fire in between. Message goes first
        // (data can only follow open), open last, and the ready state is rechecked below to recover a missed open.
        pthread_t main = EM_CALLBACK_THREAD_CONTEXT_MAIN_RUNTIME_THREAD;
        emscripten_websocket_set_onmessage_callback_on_thread(ws, ctx.get(), WebSockOnMessage, main);
        emscripten_websocket_set_onclose_callback_on_thread(ws, ctx.get(), WebSockOnClose, main);
        emscripten_websocket_set_onerror_callback_on_thread(ws, ctx.get(), WebSockOnError, main);
        emscripten_websocket_set_onopen_callback_on_thread(ws, ctx.get(), WebSockOnOpen, main);

        unsigned short readyState;

        if (emscripten_websocket_get_ready_state(ws, &readyState) == EMSCRIPTEN_RESULT_SUCCESS) {
            if (readyState == 1) {
                WebSockOnOpen(0, nullptr, ctx.get());
            } else if (readyState == 3) {
                WebSockSetEnded(ctx.get());
            }
        }

        return 0;
    }

    // The pending connect error, 0 once the socket is open.
    int32_t WebSockConnectError(int32_t sock) {
        std::shared_ptr<Ctx> ctx = WebSockGet(sock);

        if (!ctx) {
            return EBADF;
        }

        std::lock_guard<std::mutex> guard(ctx->lock);

        switch (ctx->state) {
            case SOCK_CONNECTING:
                return EINPROGRESS;

            case SOCK_FAILED:
                return ECONNREFUSED;

            default:
                return 0;
        }
    }

    int32_t WebSockSend(int32_t sock, const uint8_t* data, int32_t size) {
        std::shared_ptr<Ctx> ctx = WebSockGet(sock);

        if (!ctx) {
            s_webSockLastError = ENOTCONN;
            return -1;
        }

        SockState state;
        EMSCRIPTEN_WEBSOCKET_T ws;

        {
            std::lock_guard<std::mutex> guard(ctx->lock);
            state = ctx->state;
            ws = ctx->ws;
        }

        if (state != SOCK_OPEN || ws <= 0) {
            s_webSockLastError = state == SOCK_CONNECTING ? ENOTCONN : ECONNRESET;
            return -1;
        }

        // Fully written as far as the caller is concerned: the browser queues it.
        if (emscripten_websocket_send_binary(ws, const_cast<uint8_t*>(data), size) != EMSCRIPTEN_RESULT_SUCCESS) {
            s_webSockLastError = ECONNRESET;
            return -1;
        }

        return size;
    }

    // Returns 0 when the peer closed the connection, -1 with EWOULDBLOCK when nothing is buffered yet.
    int32_t WebSockRecv(int32_t sock, uint8_t* data, int32_t size) {
        std::shared_ptr<Ctx> ctx = WebSockGet(sock);

        if (!ctx) {
            s_webSockLastError = ENOTCONN;
            return -1;
        }

        std::lock_guard<std::mutex> guard(ctx->lock);

        size_t available = ctx->rx.size() - ctx->rxPos;

        if (available) {
            size_t count = available < static_cast<size_t>(size) ? available : static_cast<size_t>(size);
            memcpy(data, ctx->rx.data() + ctx->rxPos, count);
            ctx->rxPos += count;

            if (ctx->rxPos == ctx->rx.size()) {
                ctx->rx.clear();
                ctx->rxPos = 0;
            }

            return static_cast<int32_t>(count);
        }

        if (ctx->state == SOCK_CONNECTING || ctx->state == SOCK_OPEN) {
            s_webSockLastError = EWOULDBLOCK;
            return -1;
        }

        return 0;
    }

    void WebSockNames(int32_t sock, NETCONNADDR* names) {
        memset(names, 0, sizeof(*names));

        std::shared_ptr<Ctx> ctx = WebSockGet(sock);

        if (ctx) {
            sockaddr_in* peer = reinterpret_cast<sockaddr_in*>(&names->peerAddr);
            peer->sin_family = AF_INET;
            peer->sin_port = htons(ctx->port);
            peer->sin_addr.s_addr = ctx->address;
        }
    }
}

// WowConnectionNet::PlatformRun's stand-in for WSAEnumNetworkEvents. Bit 0: the connect has resolved (FD_CONNECT).
// Bit 1: data is buffered or the peer closed (FD_READ | FD_CLOSE).
uint32_t WebSockPoll(int32_t sock) {
    std::shared_ptr<Ctx> ctx = WebSockGet(sock);

    if (!ctx) {
        return 0;
    }

    std::lock_guard<std::mutex> guard(ctx->lock);

    uint32_t events = ctx->state != SOCK_CONNECTING ? 1 : 0;

    if (ctx->rx.size() > ctx->rxPos || ctx->state == SOCK_CLOSED || ctx->state == SOCK_FAILED) {
        events |= 2;
    }

    return events;
}

char* WowConnection::PeerAddrToStr(char* buffer, uint32_t size) {
    const sockaddr_in* peer = reinterpret_cast<const sockaddr_in*>(&this->m_peer.peerAddr);
    uint32_t address = peer->sin_addr.s_addr;
    SStrPrintf(buffer, size, "%d.%d.%d.%d:%u", address & 0xFF, (address >> 8) & 0xFF, (address >> 16) & 0xFF, address >> 24, ntohs(peer->sin_port));

    return buffer;
}

void WowConnection::CloseSocket(int32_t sock) {
    WebSockClose(sock);

    if (sock >= 0) {
        WowConnection::s_idLock.Enter();
        WowConnection::s_idBits.m_bits.m_data[sock >> 5] &= ~(1 << (sock & 0x1F));
        WowConnection::s_idLock.Leave();
    }
}

void WowConnection::DoStreamReads() {
    uint8_t buffer[4096];
    uint32_t startTime = OsGetAsyncTimeMs();
    int32_t bytes;

    while (true) {
        do {
            bytes = WebSockRecv(this->m_sock, buffer, sizeof(buffer));
        } while (bytes < 0 && WebSockInterrupted());

        if (bytes <= 0) {
            break;
        }

        this->AcquireResponseRef();
        WowConnectionResponse* response = this->m_response;
        this->m_lock.Leave();

        if (response) {
            response->WCDataReady(this, OsGetAsyncTimeMs(), buffer, bytes);
        }

        this->m_lock.Enter();
        this->ReleaseResponseRef();

        if (this->m_connState == WOWC_DISCONNECTING || static_cast<int32_t>(OsGetAsyncTimeMs() - startTime) >= 5) {
            return;
        }
    }

    if (bytes < 0 && WebSockWouldBlock()) {
        return;
    }

    this->AcquireResponseRef();
    WowConnectionResponse* response = this->m_response;

    WowConnection::s_network->Remove(this);
    this->CloseSocket(this->m_sock);
    this->SetState(WOWC_DISCONNECTED);

    int32_t sock = this->m_sock;
    this->m_lock.Leave();

    if (response && sock >= 0) {
        response->WCDisconnected(this, OsGetAsyncTimeMs(), &this->m_peer);
    }

    this->m_lock.Enter();
    this->m_sock = -1;
    this->ReleaseResponseRef();
}

WowConnection::WowConnection(int32_t sock, const NETADDR* addr, WowConnectionResponse* response) {
    this->Init(response, nullptr);
    WebSockNames(sock, &this->m_peer);
    this->m_sock = sock;
    this->m_connState = WOWC_CONNECTED;
}

int32_t WowConnection::SendPacket(CDataStore* msg, int32_t headroom) {
    int32_t size = msg->m_size;
    void* data;
    msg->GetDataInSitu(data, size);
    WowConnection::s_sendBytes += size;

    int32_t result = 2;

    this->m_lock.Enter();

    if (size <= 0) {
        result = 2;
    } else if (this->m_connState == WOWC_CONNECTED) {
        if (WowConnection::s_lagTestDelayMin || this->m_sendList.Head()) {
            WowConnection::SENDNODE* node = this->NewSendNodePacket(data, size, false);

            if (this->m_encrypt) {
                uint32_t length = this->m_uint375 - node->m_datasize + node->m_size;

                if (length > node->m_size) {
                    length = node->m_size;
                }

                SARC4ProcessBuffer(node->m_data, length, &this->m_sendKey, &this->m_sendKey);
            }

            this->m_sendList.LinkToTail(node);
            this->m_sendDepth++;
            this->m_sendBytes += node->m_size;
            WowConnection::s_network->PlatformChangeState(this, this->m_connState);

            if (this->m_sendDepth >= this->m_maxSendDepth) {
                this->SetState(WOWC_DISCONNECTING);
                result = 2;

                if (this->m_response) {
                    this->m_response->NotifySendDepthExceeded(this);
                }

                char peer[80];
                this->PeerAddrToStr(peer, sizeof(peer));
                DebugLog("WowConnection: MaxSendDepth Reached! m_sendDepth=%d, m_maxSendDepth=%d, Dropping connection to %s", this->m_sendDepth, this->m_maxSendDepth, peer);
            } else {
                result = 1;
            }
        } else {
            WowConnection::SENDNODE* node;
            bool stackNode;

            if (msg->m_size >= 100000) {
                stackNode = false;
                node = this->NewSendNodePacket(data, size, false);
            } else {
                stackNode = true;

                if (headroom < 3) {
                    void* buffer = alloca(size + sizeof(WowConnection::SENDNODE) + 3);
                    node = new (buffer) WowConnection::SENDNODE(data, size, static_cast<uint8_t*>(buffer) + sizeof(WowConnection::SENDNODE), false);
                } else {
                    void* buffer = alloca(sizeof(WowConnection::SENDNODE));
                    node = new (buffer) WowConnection::SENDNODE(nullptr, size, static_cast<uint8_t*>(data), false);
                }
            }

            if (this->m_encrypt) {
                uint32_t length = this->m_uint375 - node->m_datasize + node->m_size;

                if (length > node->m_size) {
                    length = node->m_size;
                }

                this->Encrypt(node->m_data, length);
            }

            int32_t sent = WebSockSend(this->m_sock, node->m_data, node->m_size);

            if (sent == static_cast<int32_t>(node->m_size)) {
                if (!stackNode) {
                    this->FreeSendNode(node);
                }

                result = 0;
            } else {
                bool wouldBlock = WebSockWouldBlock();

                if (stackNode) {
                    node = this->NewSendNodeCopy(node, size);
                }

                if (sent > 0) {
                    node->m_offset += sent;
                    result = 1;
                } else if (!wouldBlock) {
                    this->SetState(WOWC_DISCONNECTING);
                    result = 2;
                }

                this->m_sendList.LinkToTail(node);
                this->m_sendDepth++;
                this->m_sendBytes += node->m_size;

                if (this->m_sendDepth >= this->m_maxSendDepth) {
                    this->SetState(WOWC_DISCONNECTING);
                    result = 2;

                    if (this->m_response) {
                        this->m_response->NotifySendDepthExceeded(this);
                    }

                    char peer[80];
                    this->PeerAddrToStr(peer, sizeof(peer));
                    DebugLog("WowConnection: MaxSendDepth Reached! m_sendDepth=%d, m_maxSendDepth=%d, Dropping connection to %s", this->m_sendDepth, this->m_maxSendDepth, peer);
                }

                WowConnection::s_network->PlatformChangeState(this, this->m_connState);
            }
        }
    }

    this->m_lock.Leave();

    return result;
}

int32_t WowConnection::Send(uint8_t* data, int32_t size, bool raw) {
    WowConnection::s_sendBytes += size;

    int32_t result = 2;

    this->m_lock.Enter();

    if (!raw && this->m_encrypt) {
        int32_t length = this->m_uint375 + (size - 2 > 0x7FFF) + 2;

        if (length > size) {
            length = size;
        }

        SARC4ProcessBuffer(data, length, &this->m_sendKey, &this->m_sendKey);
    }

    if (size > 0 && this->m_connState == WOWC_CONNECTED) {
        if (!this->m_sendList.Head()) {
            int32_t sent = WebSockSend(this->m_sock, data, size);

            if (sent == size) {
                this->m_lock.Leave();
                return 0;
            }

            if (sent < 0) {
                if (WebSockWouldBlock()) {
                    sent = 0;
                } else if (this->m_sendDepth < this->m_maxSendDepth) {
                    this->SetState(WOWC_DISCONNECTING);
                    this->m_lock.Leave();
                    return 2;
                }
            }

            WowConnection::SENDNODE* node = this->NewSendNodePacket(data + sent, size - sent, true);
            this->m_sendList.LinkToTail(node);
            result = 1;
            this->m_sendDepth++;
            this->m_sendBytes += node->m_size;
            WowConnection::s_network->PlatformChangeState(this, this->m_connState);
        } else {
            result = 1;

            WowConnection::SENDNODE* node = this->NewSendNodePacket(data, size, true);
            this->m_sendList.LinkToTail(node);
            this->m_sendDepth++;
            this->m_sendBytes += node->m_size;

            if (this->m_sendDepth >= this->m_maxSendDepth) {
                this->SetState(WOWC_DISCONNECTING);

                if (this->m_response) {
                    this->m_response->NotifySendDepthExceeded(this);
                }

                char peer[80];
                this->PeerAddrToStr(peer, sizeof(peer));
                DebugLog("WowConnection: MaxSendDepth Reached! m_sendDepth=%d, m_maxSendDepth=%d, Dropping connection to %s", this->m_sendDepth, this->m_maxSendDepth, peer);

                this->m_lock.Leave();
                return 2;
            }

            WowConnection::s_network->PlatformChangeState(this, this->m_connState);
        }
    }

    this->m_lock.Leave();

    return result;
}

void WowConnection::DoMessageReads() {
    if (!this->m_readBuffer) {
        this->m_readBuffer = static_cast<uint8_t*>(SMemAlloc(1024, __FILE__, __LINE__, 0));
        this->m_readBufferSize = 1024;
        this->m_readBytes = 0;
    }

    uint32_t timeStamp = OsGetAsyncTimeMs();

    this->AcquireResponseRef();

    if (this->m_response) {
        this->m_response->NotifyAboutToDoReads();
    }

    this->ReleaseResponseRef();

    while (true) {
        WowConnection::SENDNODE* node = this->m_lagTestRecvList.Head();

        if (!node || static_cast<int32_t>(timeStamp - node->m_sendTime) < 0) {
            break;
        }

        CDataStore msg(node->m_data, node->m_size);

        this->AcquireResponseRef();
        WowConnectionResponse* response = this->m_response;

        if (response) {
            this->m_lock.Leave();
            response->WCMessageReady(this, timeStamp, &msg);
            this->m_lock.Enter();
        }

        this->ReleaseResponseRef();
        this->m_lagTestRecvList.UnlinkNode(node);
        WDataStore::FreeBuffer(node, node->m_datasize + sizeof(WowConnection::SENDNODE) + 3);
    }

    int32_t bytes;

    while (true) {
        int32_t headerSize = 2;
        int32_t packetSize = -1;

        if (this->m_readBytes >= 2) {
            uint8_t* buffer = this->m_readBuffer;

            if (buffer[0] & 0x80) {
                headerSize = 3;

                if (this->m_readBytes >= 3) {
                    packetSize = (buffer[2] | ((buffer[1] | ((buffer[0] & 0x7F) << 8)) << 8)) + 3;
                }
            } else {
                packetSize = (buffer[1] | ((buffer[0] & 0x7F) << 8)) + 2;
            }
        }

        if (this->m_readBytes >= this->m_readBufferSize) {
            this->m_readBuffer = static_cast<uint8_t*>(SMemReAlloc(this->m_readBuffer, 2 * this->m_readBufferSize, __FILE__, __LINE__, 0));
            this->m_readBufferSize *= 2;

            if (static_cast<uint32_t>(this->m_readBufferSize) > 32000000) {
                this->Disconnect();
                return;
            }
        }

        int32_t toRead;

        if (packetSize >= 0) {
            toRead = packetSize - this->m_readBytes;

            if (this->m_readBufferSize - this->m_readBytes < toRead) {
                toRead = this->m_readBufferSize - this->m_readBytes;
            }
        } else {
            toRead = headerSize - this->m_readBytes;
        }

        if (toRead <= 0) {
            bytes = 0;
        } else {
            do {
                bytes = WebSockRecv(this->m_sock, this->m_readBuffer + this->m_readBytes, toRead);
            } while (bytes < 0 && WebSockInterrupted());

            if (bytes <= 0) {
                break;
            }
        }

        if (this->m_encrypt) {
            int32_t length = headerSize + this->m_uint376 - this->m_readBytes;

            if (length <= 0) {
                length = 0;
            }

            if (length >= bytes) {
                length = bytes;
            }

            SARC4ProcessBuffer(this->m_readBuffer + this->m_readBytes, length, &this->m_receiveKey, &this->m_receiveKey);
        }

        this->m_readBytes += bytes;

        if (packetSize >= 0 && this->m_readBytes >= packetSize) {
            CDataStore msg(this->m_readBuffer + headerSize, packetSize - headerSize);

            this->AcquireResponseRef();
            WowConnectionResponse* response = this->m_response;

            if (WowConnection::s_lagTestDelayMin) {
                WowConnection::SENDNODE* node = this->NewSendNodePacket(this->m_readBuffer + headerSize, packetSize - headerSize, true);
                this->m_lagTestRecvList.LinkToTail(node);
            } else if (response) {
                this->m_lock.Leave();
                response->WCMessageReady(this, timeStamp, &msg);
                this->m_lock.Enter();
            }

            this->m_readBytes = 0;
            this->ReleaseResponseRef();
        }

        if (bytes <= 0) {
            return;
        }
    }

    if (bytes < 0 && WebSockWouldBlock()) {
        return;
    }

    this->AcquireResponseRef();
    WowConnectionResponse* response = this->m_response;

    WowConnection::s_network->Remove(this);
    this->CloseSocket(this->m_sock);
    this->SetState(WOWC_DISCONNECTED);

    int32_t sock = this->m_sock;
    this->m_lock.Leave();

    if (response && sock >= 0) {
        response->WCDisconnected(this, timeStamp, &this->m_peer);
    }

    this->m_lock.Enter();
    this->m_sock = -1;
    this->ReleaseResponseRef();
}

void WowConnection::CheckConnect() {
    int32_t error = WebSockConnectError(this->m_sock);

    if (error) {
        WowConnection::s_network->Remove(this);
        this->CloseSocket(this->m_sock);
        this->m_sock = -1;
        this->SetState(WOWC_DISCONNECTED);
        this->AddRef();
        this->AcquireResponseRef();

        WowConnectionResponse* response = this->m_response;
        this->m_lock.Leave();

        if (response) {
            response->WCCantConnect(this, OsGetAsyncTimeMs(), &this->m_peer);
        }
    } else {
        this->SetState(WOWC_CONNECTED);
        this->AddRef();
        this->AcquireResponseRef();

        WowConnectionResponse* response = this->m_response;
        this->m_lock.Leave();

        WebSockNames(this->m_sock, &this->m_peer);

        if (response) {
            response->WCConnected(this, nullptr, OsGetAsyncTimeMs(), &this->m_peer);
        }
    }

    this->m_lock.Enter();
    this->ReleaseResponseRef();
    this->Release();
}

void WowConnection::CheckAccept() {
    // Browsers cannot listen, so there is never anything to accept.
}

void WowConnection::DoWrites() {
    this->AddRef();
    this->m_lock.Enter();

    bool disconnected = false;

    if (this->m_connState == WOWC_CONNECTING) {
        this->CheckConnect();
    } else if (this->m_sendList.Head()) {
        while (true) {
            WowConnection::SENDNODE* node = this->m_sendList.Head();

            if (!node) {
                break;
            }

            uint32_t now = OsGetAsyncTimeMs();

            if (WowConnection::s_lagTestDelayMin && static_cast<int32_t>(now - node->m_sendTime) < 0) {
                break;
            }

            uint32_t bytes = node->m_size - node->m_offset;

            if (bytes >= 0x400) {
                bytes = 0x400;
            }

            if (bytes) {
                int32_t sent = WebSockSend(this->m_sock, node->m_data + node->m_offset, bytes);

                if (sent != static_cast<int32_t>(bytes)) {
                    if (sent > 0) {
                        node->m_offset += sent;
                        break;
                    }

                    if (sent >= 0 || WebSockWouldBlock()) {
                        break;
                    }

                    disconnected = true;
                }
            }

            if (!disconnected) {
                if (bytes == node->m_size - node->m_offset) {
                    this->m_sendList.UnlinkNode(node);
                    this->m_sendDepth--;
                    this->m_sendBytes -= node->m_size;
                    WDataStore::FreeBuffer(node, node->m_datasize + sizeof(WowConnection::SENDNODE) + 3);
                } else {
                    node->m_offset += bytes;
                }
            }

            if (!this->m_sendList.Head() || disconnected) {
                break;
            }
        }
    } else {
        WowConnection::s_network->PlatformChangeState(this, this->m_connState);
    }

    WowConnectionResponse* response = nullptr;
    int32_t sock = -1;
    bool writeReady = false;

    if (disconnected) {
        this->AcquireResponseRef();
        response = this->m_response;
        WowConnection::s_network->Remove(this);
        this->CloseSocket(this->m_sock);
        sock = this->m_sock;
        this->SetState(WOWC_DISCONNECTED);
    } else if (this->m_notifyWriteReady && !this->m_sendList.Head()) {
        writeReady = true;
        this->m_notifyWriteReady = 0;
        this->AcquireResponseRef();
        response = this->m_response;
    }

    this->m_lock.Leave();

    if (disconnected) {
        if (response && sock >= 0) {
            response->WCDisconnected(this, OsGetAsyncTimeMs(), &this->m_peer);
        }

        this->m_lock.Enter();
        this->m_sock = -1;
        this->ReleaseResponseRef();
        this->m_lock.Leave();
    } else if (writeReady) {
        if (response) {
            response->WCWriteReady(this);
        }

        this->m_lock.Enter();
        this->ReleaseResponseRef();
        this->m_lock.Leave();
    }

    this->Release();
}

void WowConnection::StartConnect() {
    if (this->m_sock >= 0) {
        if (this->m_netLink.IsLinked()) {
            WowConnection::s_network->Remove(this);
        }

        this->CloseSocket(this->m_sock);
        this->m_sock = -1;
    }

    this->m_lock.Enter();

    int32_t sock = WebSockOpen();

    WowConnection::MarkSocketId(sock);
    this->m_sock = sock;

    if (sock >= 0) {
        if (!this->m_netLink.IsLinked()) {
            WowConnection::s_network->Add(this);
        }

        this->m_readBytes = 0;
        this->SetState(WOWC_CONNECTING);

        if (!WebSockConnect(this->m_sock, this->m_connectAddress, this->m_connectPort)) {
            this->m_lock.Leave();
            return;
        }

        WowConnection::s_network->Remove(this);
        this->CloseSocket(this->m_sock);
        this->m_sock = -1;
    }

    this->SetState(WOWC_ERROR);
    this->m_lock.Leave();
}

bool WowConnection::Connect(const char* host, uint16_t port, int32_t timeout) {
    // The relay takes dotted IPv4 addresses only (the realmlist is parsed that way), so a hostname is left as address 0
    // and the connect fails through CheckConnect.
    uint32_t address = inet_addr(host);

    this->m_connectAddress = address == INADDR_NONE ? 0 : address;
    this->m_connectPort = port;
    this->StartConnect();

    return true;
}

#endif
