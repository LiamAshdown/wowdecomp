#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cstdint>
#include <cstring>
#include <new>
#include <pthread.h>
#include <unistd.h>
#include "Engine/Source/BlizzardCore/Source/System/Lock/Web/WebSync.h"
#include "Engine/Source/Storm/Source/SThread.h"
#include "Engine/Source/Storm/H/storm.h"

// Pthread stacks are carved out of the wasm heap and the default is far smaller than the 1 MB a Windows thread gets.
static const uint32_t kMinStackSize = 1024 * 1024;

struct THREADLAUNCH {
    /* 0000 */ uint32_t (STORMAPI* threadProc)(void*);
    /* 0004 */ void* param;
    /* 0008 */ HANDLE threadH;
};

// What the pthread entry point needs: the Storm launch record and the handle to signal when the thread ends.
struct THREADSTART {
    THREADLAUNCH* launch;
    WebSync::Obj* done;
};

namespace S_Thread {
    SThreadTrack s_threads[s_maxthreads];

    int32_t s_numthreads;

    CInitCritSect s_threadCrit;
}

uint32_t STORMAPI SGetCurrentThreadId() {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pthread_self()));
}

uint32_t STORMAPI SGetCurrentProcessId() {
    return static_cast<uint32_t>(getpid());
}

// Browsers expose no thread or process scheduling controls; report "normal" and success.
int32_t STORMAPI SGetCurrentThreadPriority() {
    return 0;
}

int32_t STORMAPI SSetCurrentThreadPriority(int32_t priority) {
    return 1;
}

int32_t STORMAPI SSetCurrentProcessAffinityMask(size_t mask) {
    return 1;
}

int32_t CInitCritSect::Enter() {
    int32_t result = 0;

    if (!this->m_critsect) {
        while (InterlockedExchange(&this->m_spinLock, 1)) {
        }

        if (!this->m_critsect) {
            this->m_critsect = reinterpret_cast<CCritSect*>(this->m_critsectData);
            new (this->m_critsectData) CCritSect();
            result = 1;
        }

        this->m_spinLock = 0;
    }

    this->m_critsect->Enter();
    return result;
}

void CInitCritSect::Leave() {
    this->m_critsect->Leave();
}

uint32_t STORMAPI S_Thread::s_SLaunchThread(void* threadParam) {
    s_threadCrit.Enter();

    THREADLAUNCH* launch = static_cast<THREADLAUNCH*>(threadParam);
    uint32_t (STORMAPI* threadProc)(void*) = launch->threadProc;
    void* param = launch->param;
    uint32_t threadId = SGetCurrentThreadId();

    SMemFree(launch, __FILE__, __LINE__, 0);
    s_threadCrit.Leave();

    uint32_t threadVal = threadProc(param);

    s_threadCrit.Enter();

    int32_t numthreads = s_numthreads;

    for (int32_t i = 0; i < numthreads; i++) {
        if (s_threads[i].live && s_threads[i].threadId == threadId) {
            memcpy(&s_threads[i], &s_threads[i + 1], (s_maxthreads - 1 - i) * sizeof(SThreadTrack));
            numthreads--;
        }
    }

    s_numthreads = numthreads;
    s_threadCrit.Leave();

    return threadVal;
}

static void* ThreadEntry(void* param) {
    THREADSTART* start = static_cast<THREADSTART*>(param);
    WebSync::Obj* done = start->done;

    S_Thread::s_SLaunchThread(start->launch);
    delete start;

    WebSync::SetSignaled(done, true);
    WebSync::Release(done);

    return nullptr;
}

// The returned HANDLE is a WebSync thread object that becomes signaled when the thread function returns.
HANDLE SCreateThread(uint32_t dwStackSize, uint32_t (STORMAPI* lpStartAddress)(void*), void* lpParameter, uint32_t dwCreationFlags, uint32_t* lpThreadId, char* threadName) {
    if (!threadName) {
        threadName = const_cast<char*>("");
    }

    S_Thread::s_threadCrit.Enter();

    if (!S_Thread::s_numthreads) {
        S_Thread::SThreadTrack* track = &S_Thread::s_threads[S_Thread::s_numthreads];
        track->threadId = SGetCurrentThreadId();
        track->threadH = nullptr;
        track->live = 1;
        track->suspended = 0;
        SStrCopy(track->name, "main", sizeof(track->name));
        S_Thread::s_numthreads++;
    }

    S_Thread::s_threadCrit.Leave();

    THREADLAUNCH* launch = static_cast<THREADLAUNCH*>(SMemAlloc(sizeof(THREADLAUNCH), __FILE__, __LINE__, SMEM_FLAG_ZEROMEMORY));
    launch->threadProc = lpStartAddress;
    launch->param = lpParameter;

    WebSync::Obj* handle = WebSync::Create(WebSync::KindThread, false, false);
    WebSync::AddRef(handle);

    THREADSTART* start = new THREADSTART;
    start->launch = launch;
    start->done = handle;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, dwStackSize > kMinStackSize ? dwStackSize : kMinStackSize);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    S_Thread::s_threadCrit.Enter();

    pthread_t id;
    int error = pthread_create(&id, &attr, ThreadEntry, start);
    pthread_attr_destroy(&attr);

    if (error) {
        S_Thread::s_threadCrit.Leave();
        SMemFree(launch, __FILE__, __LINE__, 0);
        delete start;
        WebSync::Release(handle);
        WebSync::Release(handle);
        return nullptr;
    }

    launch->threadH = handle;

    uint32_t threadId = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(id));

    if (lpThreadId) {
        *lpThreadId = threadId;
    }

    S_Thread::SThreadTrack* track = &S_Thread::s_threads[S_Thread::s_numthreads];
    track->threadId = threadId;
    track->threadH = handle;
    track->live = 1;
    track->suspended = 0;

    if (threadName) {
        SStrCopy(track->name, threadName, sizeof(track->name));
    }

    S_Thread::s_numthreads++;
    S_Thread::s_threadCrit.Leave();

    return handle;
}

int32_t STORMAPI SGetThreadName(uint32_t threadId, char* buffer, int32_t bufferchars) {
    bool found = false;

    if (!buffer || bufferchars < 1) {
        return 0;
    }

    if (bufferchars > static_cast<int32_t>(sizeof(S_Thread::s_threads[0].name))) {
        bufferchars = sizeof(S_Thread::s_threads[0].name);
    }

    *buffer = 0;

    S_Thread::s_threadCrit.Enter();

    for (int32_t i = 0; i < S_Thread::s_numthreads; i++) {
        if (S_Thread::s_threads[i].threadId == threadId) {
            SStrCopy(buffer, S_Thread::s_threads[i].name, bufferchars);
            found = true;
            break;
        }
    }

    S_Thread::s_threadCrit.Leave();

    return found;
}

HANDLE STORMAPI SCreateThread(uint32_t (STORMAPI* lpStartAddress)(void*), void* lpParameter, uint32_t* lpThreadId, void* linuxData, char* threadName, uint32_t stackSize) {
    return SCreateThread(stackSize, lpStartAddress, lpParameter, 0, lpThreadId, threadName);
}

// A browser tab cannot spawn processes.
int32_t STORMAPI SCreateProcess(const char* appName, char* commandLine, void (*callbackWhenProcessCompletes)(void*), void* callbackData) {
    return 0;
}

#endif
