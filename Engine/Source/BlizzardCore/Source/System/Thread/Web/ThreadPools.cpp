#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cstdint>
#include <cstdlib>
#include <windows.h>
#include "Engine/Source/BlizzardCore/Source/System/Lock/Lock.h"
#include "Engine/Source/BlizzardCore/Source/System/Thread/Thread.h"
#include "Engine/Source/BlizzardCore/Source/System/Thread/ThreadLocalStorage.h"

System_Thread::ThreadWorker::~ThreadWorker() {
    Blizzard::Lock::EventSet(this->m_wakeEvent.m_event);
    Blizzard::Lock::EventWaitForever(this->m_readyEvent.m_event);

    if (this->m_thread) {
        Blizzard::Lock::EventSet(this->m_wakeEvent.m_event);

        while (this->m_thread->m_state >= 0) {
            Blizzard::Thread::Sleep(1);
        }

        Blizzard::Thread::Close(this->m_thread);
        this->m_thread = nullptr;
    }
}

uint32_t System_Thread::ThreadWorker::Executive(void* param) {
    ThreadWorker* worker = static_cast<ThreadWorker*>(param);

    Blizzard::Lock::EventSet(worker->m_readyEvent.m_event);
    Blizzard::Lock::EventWaitForever(worker->m_wakeEvent.m_event);

    while (worker->m_taskFunction) {
        worker->m_task->m_state = 1;
        worker->m_task->m_result = worker->m_taskFunction(worker->m_taskParam);

        // Priority and affinity are not adjustable in a browser, so unlike Windows there is nothing to restore here.
        worker->m_task->m_state = -1;
        Blizzard::Thread::Close(worker->m_task);
        worker->m_task = nullptr;
        worker->m_taskFunction = nullptr;
        worker->m_taskParam = nullptr;

        {
            Blizzard::Lock::ScopedMutex lock(worker->m_pool->m_mutex.m_mutex);
            worker->m_pool->m_idle.push_back(worker);
        }

        Blizzard::Lock::EventSet(worker->m_readyEvent.m_event);
        Blizzard::Lock::EventWaitForever(worker->m_wakeEvent.m_event);
    }

    Blizzard::Lock::EventSet(worker->m_readyEvent.m_event);

    return 0;
}

bool System_Thread::ThreadWorker::Start(Blizzard::Thread::ThreadPoolRecord* pool, int32_t flags, int32_t stackSize) {
    if (!Blizzard::Thread::Create(Executive, this, &this->m_thread, "Blizzard Thread Pool Worker", flags, stackSize)) {
        return false;
    }

    this->m_priority = 3;
    this->m_affinity = 0;
    this->m_pool = pool;

    return true;
}

#endif
