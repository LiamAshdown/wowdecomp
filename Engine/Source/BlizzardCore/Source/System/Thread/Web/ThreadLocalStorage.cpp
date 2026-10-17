#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cstdint>
#include <pthread.h>
#include <windows.h>
#include "Engine/Source/BlizzardCore/Source/System/Lock/Lock.h"
#include "Engine/Source/BlizzardCore/Source/System/Thread/Thread.h"
#include "Engine/Source/BlizzardCore/Source/System/Thread/ThreadLocalStorage.h"

// TLSSlot::m_key holds the inverted pthread key so that zero still means "not allocated".
// The destructor is run by RunTLSDestructors (not by pthreads) so that it matches the Windows ordering.
bool System_Thread::AllocateLocalStorage(Blizzard::Thread::TLSSlot* slot, Blizzard::Thread::TLSDestructor destructor) {
    InitThreadSystem();

    pthread_key_t key;

    if (pthread_key_create(&key, nullptr)) {
        return false;
    }

    slot->m_key = ~static_cast<uint32_t>(key);
    slot->m_destructor = destructor;
    Blizzard::Lock::MutexEnter(s_mutex);
    s_slotList[s_slotListUsed++] = slot;
    Blizzard::Lock::MutexLeave(s_mutex);

    return true;
}

void System_Thread::RunTLSDestructors() {
    if (!s_initialized) {
        return;
    }

    Blizzard::Lock::ScopedMutex lock(s_mutex);

    for (uint32_t i = 0; i < s_slotListUsed; i++) {
        Blizzard::Thread::TLSSlot* slot = s_slotList[i];
        Blizzard::Thread::TLSDestructor destructor = slot->m_destructor;

        if (destructor) {
            void* value = pthread_getspecific(~slot->m_key);

            if (value) {
                destructor(value);
            }
        }
    }
}

void* Blizzard::Thread::RegisterLocalStorage(TLSSlot* slot, void* (*constructor)(void*), void* param, TLSDestructor destructor) {
    void* value;

    if (!slot->m_key) {
        if (!System_Thread::AllocateTLSSlot(slot, destructor)) {
            return nullptr;
        }
    } else {
        value = pthread_getspecific(~slot->m_key);

        if (value) {
            return value;
        }
    }

    value = constructor(param);
    System_Thread::InternalSetLocalStorage(slot, value);

    return value;
}

#endif
