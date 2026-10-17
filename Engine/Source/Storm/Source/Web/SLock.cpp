#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cstring>
#include <pthread.h>
#include "Engine/Source/BlizzardCore/Source/System/Lock/Web/WebSync.h"
#include "Engine/Source/Storm/Source/SLock.h"
#include "Engine/Source/Storm/Source/SThread.h"
#include "Engine/Source/Storm/H/storm.h"

// SSyncObject::m_opaqueData points at a WebSync::Obj (see WebSync.h) instead of a kernel handle.
static WebSync::Obj* Sync(const SSyncObject* object) {
    return static_cast<WebSync::Obj*>(object->m_opaqueData);
}

SCritSect::SCritSect() {
    InitializeCriticalSection(&this->m_critsect);
}

SCritSect::~SCritSect() {
    DeleteCriticalSection(&this->m_critsect);
}

void SCritSect::Enter() {
    EnterCriticalSection(&this->m_critsect);
}

void SCritSect::Leave() {
    LeaveCriticalSection(&this->m_critsect);
}

SSyncObject::SSyncObject() {
    this->m_opaqueData = nullptr;
}

int32_t SSyncObject::Valid() {
    return this->m_opaqueData != nullptr;
}

uint32_t SSyncObject::Wait(uint32_t timeout) {
    return WebSync::Wait(Sync(this), timeout);
}

uint32_t STORMAPI SSyncObject::WaitMultiplePtr(uint32_t count, SSyncObject** objects, int32_t waitAll, uint32_t timeout) {
    WebSync::Obj* handles[64];

    if (count > 64) {
        return 0xFFFFFFFF;
    }

    uint32_t handleCount = 0;

    for (uint32_t i = 0; i < count; i++) {
        if (objects[i] && objects[i]->m_opaqueData) {
            handles[handleCount++] = Sync(objects[i]);
        }
    }

    return WebSync::WaitMultiple(handleCount, handles, waitAll != 0, timeout);
}

void SSyncObject::Close() {
    if (this->m_opaqueData) {
        WebSync::Release(Sync(this));
        this->m_opaqueData = nullptr;
    }
}

SSyncObject::~SSyncObject() {
    this->Close();
}

SEvent::SEvent(int32_t manualReset, int32_t initialState) {
    this->m_opaqueData = WebSync::Create(WebSync::KindEvent, manualReset != 0, initialState != 0);
}

int32_t SEvent::Set() {
    WebSync::SetSignaled(Sync(this), true);
    return 1;
}

int32_t SEvent::Reset() {
    WebSync::SetSignaled(Sync(this), false);
    return 1;
}

SSemaphore::SSemaphore(int32_t initialCount, int32_t maximumCount) {
    this->m_opaqueData = WebSync::Create(WebSync::KindSemaphore, false, false, initialCount, maximumCount);
}

int32_t SSemaphore::Signal(uint32_t count) {
    return WebSync::ReleaseSemaphore(Sync(this), count);
}

SMutex::SMutex() {
    this->Create(0, nullptr);
}

// Named (cross-process) mutexes do not exist in a browser, so the name is ignored.
void SMutex::Create(int32_t initialOwner, const char* name) {
    this->Close();

    WebSync::Obj* mutex = WebSync::Create(WebSync::KindMutex, false, false);

    if (initialOwner) {
        mutex->owner = pthread_self();
        mutex->recursion = 1;
    }

    this->m_opaqueData = mutex;
}

int32_t SMutex::Release() {
    return WebSync::ReleaseMutex(Sync(this));
}

int32_t SThread::Create(uint32_t (STORMAPI* threadProc)(void*), void* param, SThread& thread, char* threadName, uint32_t stackSize) {
    uint32_t threadId;
    thread.m_opaqueData = SCreateThread(threadProc, param, &threadId, nullptr, nullptr, stackSize);
    return thread.m_opaqueData != nullptr;
}

// Browsers expose no thread priorities.
int32_t SThread::SetPriority(int32_t priority) {
    return priority < 6;
}

// Reader/writer lock: m_state is 0 when free, -1 when write-locked, otherwise the reader count. Uncontended
// enter/leave is a single compare-exchange; a contended enter sleeps on one shared condvar.
// ponytail: readers can starve a writer, and every contended lock shares the one condvar.
static pthread_mutex_t s_rwMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_rwCond = PTHREAD_COND_INITIALIZER;
static volatile LONG s_rwWaiters;

static bool TryAcquire(volatile LONG* state, int32_t forwriting) {
    if (forwriting) {
        return InterlockedCompareExchange(state, -1, 0) == 0;
    }

    LONG current = *state;

    while (current >= 0) {
        if (InterlockedCompareExchange(state, current + 1, current) == current) {
            return true;
        }

        current = *state;
    }

    return false;
}

CSRWLock::CSRWLock() {
    this->m_opaqueData.m_mutex.m_state = 0;
    this->m_opaqueData.m_mutex.m_event = 0;
    this->m_opaqueData.m_readerEvent = 0;
}

CSRWLock::~CSRWLock() {
}

void CSRWLock::Enter(int32_t forwriting) {
    volatile LONG* state = &this->m_opaqueData.m_mutex.m_state;

    if (TryAcquire(state, forwriting)) {
        return;
    }

    pthread_mutex_lock(&s_rwMutex);
    InterlockedIncrement(&s_rwWaiters);

    while (!TryAcquire(state, forwriting)) {
        pthread_cond_wait(&s_rwCond, &s_rwMutex);
    }

    InterlockedDecrement(&s_rwWaiters);
    pthread_mutex_unlock(&s_rwMutex);
}

void CSRWLock::Leave(int32_t fromwriting) {
    volatile LONG* state = &this->m_opaqueData.m_mutex.m_state;

    if (fromwriting) {
        InterlockedExchange(state, 0);
    } else {
        InterlockedDecrement(state);
    }

    if (s_rwWaiters) {
        pthread_mutex_lock(&s_rwMutex);
        pthread_cond_broadcast(&s_rwCond);
        pthread_mutex_unlock(&s_rwMutex);
    }
}

#endif
