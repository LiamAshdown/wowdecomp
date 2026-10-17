#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cstdint>
#include <cstdlib>
#include <windows.h>
#include "Engine/Source/BlizzardCore/Source/System/Thread/Thread.h"

bool Blizzard::Thread::Close(ThreadRecord* thread) {
    if (thread->m_refCount.Decrement() > 0) {
        return true;
    }

    // Threads are created detached, so there is no handle to close.
    thread->m_handle = nullptr;
    System_Thread::RemoveFromRegistry(thread);
    free(thread);

    return true;
}

bool Blizzard::Thread::WaitForever(ThreadRecord* thread) {
    if (thread->m_state < 0) {
        return true;
    }

    System_Thread::ThreadWorker* worker = thread->m_worker;

    if (worker && thread != worker->m_thread) {
        if (thread == worker->m_task) {
            Blizzard::Lock::EventWaitForever(worker->m_readyEvent.m_event);
            Blizzard::Lock::EventSet(worker->m_readyEvent.m_event);
        }

        return true;
    }

    // ponytail: poll instead of a join (threads are detached); upgrade to a per-record condvar if the 1 ms latency matters.
    while (thread->m_state >= 0) {
        Blizzard::Thread::Sleep(1);
    }

    return true;
}

#endif
