#if defined(WOWDECOMP_PLATFORM_WEB)

#include <windows.h>
#include <pthread.h>
#include <time.h>
#include "WoW/Source/WowServices/WowConnectionNet.h"

// Defined in WowConnection.cpp: bit 0 = connect resolved, bit 1 = readable or closed.
uint32_t WebSockPoll(int32_t sock);

namespace {
    const uint32_t MAX_SERVICED_CONNECTIONS = 64;

    // Manual-reset event for the network thread (Win64: CreateEventA). WebSocket callbacks signal it from the main browser thread.
    struct NetEvent {
        pthread_mutex_t mutex;
        pthread_cond_t cond;
        bool signaled;
    };

    void NetEventSet(void* handle) {
        NetEvent* event = static_cast<NetEvent*>(handle);

        if (!event) {
            return;
        }

        pthread_mutex_lock(&event->mutex);
        event->signaled = true;
        pthread_cond_broadcast(&event->cond);
        pthread_mutex_unlock(&event->mutex);
    }

    void NetEventWait(void* handle, uint32_t timeoutMs) {
        NetEvent* event = static_cast<NetEvent*>(handle);
        timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += timeoutMs / 1000;
        deadline.tv_nsec += static_cast<long>(timeoutMs % 1000) * 1000000;

        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000;
        }

        pthread_mutex_lock(&event->mutex);

        while (!event->signaled) {
            if (pthread_cond_timedwait(&event->cond, &event->mutex, &deadline)) {
                break;
            }
        }

        event->signaled = false;
        pthread_mutex_unlock(&event->mutex);
    }
}

void WowConnectionNet::PlatformChangeState(WowConnection* connection, WOW_CONN_STATE state) {
    NetEventSet(this->m_event);
}

void WowConnectionNet::PlatformRun() {
    this->m_networkStarted = true;

    uint32_t timeout = 500;

    if (this->m_stop) {
        return;
    }

    do {
        WowConnection* connections[MAX_SERVICED_CONNECTIONS];
        uint32_t flags[MAX_SERVICED_CONNECTIONS];

        this->m_connectionsLock.Enter();

        uint32_t count = 0;
        timeout = 500;

        for (WowConnection* connection = this->m_connections.Head(); connection; connection = this->m_connections.Next(connection)) {
            if (connection->m_serviceCount || count >= MAX_SERVICED_CONNECTIONS) {
                continue;
            }

            // The WebSocket callbacks wake the loop through m_event, so readiness is derived from each socket's buffer
            // state here (Win64 gets the same bits from WSAEnumNetworkEvents) and the worker is only signalled when
            // there is something to do.
            uint32_t connectionFlags = 0;
            uint32_t events = WebSockPoll(connection->m_sock);

            switch (connection->m_connState) {
                case WOWC_CONNECTING:
                    connectionFlags = events & 1;
                    break;

                case WOWC_CONNECTED:
                    connectionFlags = (events & 2) | (connection->m_sendList.Head() || connection->m_notifyWriteReady ? 1 : 0);
                    break;

                case WOWC_DISCONNECTING:
                    connectionFlags = 8;
                    timeout = 0;
                    break;

                default:
                    continue;
            }

            if (WowConnection::s_lagTestDelayMin || WowConnection::s_lagTestDelayMax) {
                connectionFlags |= 3;
                timeout = 10;
            }

            if (!connectionFlags) {
                continue;
            }

            connection->m_refCount.Increment();
            connections[count] = connection;
            flags[count] = connectionFlags;
            count++;

            if (connection->m_lagTestRecvList.Head()) {
                timeout = 0;
            }
        }

        this->m_connectionsLock.Leave();

        for (uint32_t i = 0; i < count; i++) {
            this->SignalWorker(connections[i], flags[i]);
        }

        for (uint32_t i = 0; i < count; i++) {
            connections[i]->Release();
        }

        if (timeout) {
            NetEventWait(this->m_event, timeout);
        }
    } while (!this->m_stop);
}

void WowConnectionNet::PlatformInit(bool useEngine) {
    NetEvent* event = new NetEvent;
    pthread_mutex_init(&event->mutex, nullptr);
    pthread_cond_init(&event->cond, nullptr);
    event->signaled = false;
    this->m_event = event;
}

void WowConnectionNet::PlatformDestruct(WowConnection* connection) {
}

void WowConnectionNet::PlatformWorkerReady() {
    NetEventSet(this->m_event);
}

void WowConnectionNet::PlatformAdd(WowConnection* connection) {
    NetEventSet(this->m_event);
}

void WowConnectionNet::PlatformRemove(WowConnection* connection) {
    NetEventSet(this->m_event);
}

void WowConnectionNet::PlatformStop() {
}

void WowConnectionNet::PlatformDestroy() {
    NetEvent* event = static_cast<NetEvent*>(this->m_event);

    if (event) {
        this->m_event = nullptr;
        pthread_cond_destroy(&event->cond);
        pthread_mutex_destroy(&event->mutex);
        delete event;
    }
}

#endif
