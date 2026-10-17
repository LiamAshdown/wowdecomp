#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <time.h>
#include <windows.h>
#include "Engine/Source/BlizzardCore/Source/System/Lock/Lock.h"
#include "Engine/Source/BlizzardCore/Source/System/String/String.h"
#include "Engine/Source/BlizzardCore/Source/System/Thread/Thread.h"
#include "Engine/Source/BlizzardCore/Source/System/Thread/ThreadLocalStorage.h"

bool System_Thread::s_initialized;

Blizzard::Lock::Mutex System_Thread::s_mutex;

Blizzard::Lock::Mutex System_Thread::s_registryMutex;

Blizzard::Thread::TLSSlot System_Thread::s_threadRecordTLS;

Blizzard::Thread::TLSSlot System_Thread::s_stackTraceEntryPointTLS;

Blizzard::Thread::ThreadRecord* System_Thread::s_mainThread;

std::set<Blizzard::Thread::ThreadRecord*, std::less<Blizzard::Thread::ThreadRecord*>, Blizzard::Memory::AllocSTL_raw<Blizzard::Thread::ThreadRecord*>>* System_Thread::s_ThreadRegistry;

int32_t System_Thread::s_DefaultStackSize;

System_Thread::ExternalTLSRecord System_Thread::s_externalTLS[32];

int32_t System_Thread::s_externalTLSCount;

// Pthread stacks are carved out of the wasm heap and the default is far smaller than the 1 MB a Windows thread gets.
static const int32_t kMinStackSize = 1024 * 1024;

static uint32_t CurrentThreadId() {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pthread_self()));
}

bool Blizzard::Thread::Sleep(uint32_t milliseconds) {
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = static_cast<long>(milliseconds % 1000) * 1000000L;
    nanosleep(&ts, nullptr);
    return true;
}

bool Blizzard::Thread::IsRunning(ThreadRecord* thread) {
    return thread->m_state >= 0;
}

// Browsers expose no thread priorities.
bool System_Thread::SetPriorityLevel(Blizzard::Thread::ThreadRecord* thread, int32_t level) {
    return level >= 0 && level <= 6;
}

bool System_Thread::GetPriorityLevel(Blizzard::Thread::ThreadRecord* thread, int32_t* level) {
    *level = 3;
    return true;
}

// Threads not created through Blizzard::Thread::Create are not tracked, so there is nothing to wait for.
void System_Thread::ShutdownThreadLocalStorage() {
}

void System_Thread::RunExternalTLSDestructors(HANDLE thread) {
}

void System_Thread::AddToRegistry(Blizzard::Thread::ThreadRecord* thread) {
    Blizzard::Lock::ScopedMutex lock(s_registryMutex);

    if (!s_ThreadRegistry) {
        s_ThreadRegistry = new std::set<Blizzard::Thread::ThreadRecord*, std::less<Blizzard::Thread::ThreadRecord*>, Blizzard::Memory::AllocSTL_raw<Blizzard::Thread::ThreadRecord*>>();
    }

    s_ThreadRegistry->insert(thread);
}

Blizzard::Thread::ThreadRecord* System_Thread::NewThread(uint32_t (*function)(void*), void* param, const char* name) {
    if (!name) {
        name = "";
    }

    uint32_t length = static_cast<uint32_t>(strlen(name));
    Blizzard::Thread::ThreadRecord* thread = static_cast<Blizzard::Thread::ThreadRecord*>(malloc(sizeof(Blizzard::Thread::ThreadRecord) + length + 1));
    memset(thread, 0, sizeof(Blizzard::Thread::ThreadRecord));
    thread->m_param = param;
    thread->m_function = function;
    thread->m_state = 0;
    thread->m_refCount.m_value = 2;
    Blizzard::String::Copy(thread->m_name, name, length + 1);
    AddToRegistry(thread);

    return thread;
}

void System_Thread::RemoveFromRegistry(Blizzard::Thread::ThreadRecord* thread) {
    Blizzard::Lock::ScopedMutex lock(s_registryMutex);
    s_ThreadRegistry->erase(thread);

    if (s_ThreadRegistry->empty()) {
        delete s_ThreadRegistry;
        s_ThreadRegistry = nullptr;
    }
}

// Record for the calling pthread, which Blizzard::Thread::Create did not start.
static Blizzard::Thread::ThreadRecord* AdoptCurrentThread() {
    Blizzard::Thread::ThreadRecord* thread = System_Thread::NewThread(nullptr, nullptr, nullptr);
    thread->m_handle = reinterpret_cast<HANDLE>(pthread_self());
    thread->m_threadId = CurrentThreadId();
    thread->m_state = 1;
    thread->m_refCount.m_value = 1;
    System_Thread::InternalSetLocalStorage(&System_Thread::s_threadRecordTLS, thread);

    return thread;
}

void System_Thread::InitMainThread() {
    s_mainThread = AdoptCurrentThread();
}

void System_Thread::InitThreadSystem() {
    if (s_initialized) {
        return;
    }

    s_initialized = true;
    Blizzard::Lock::MutexCreate(s_mutex);
    Blizzard::Lock::MutexCreate(s_registryMutex);
    AllocateLocalStorage(&s_threadRecordTLS, nullptr);
    AllocateLocalStorage(&s_stackTraceEntryPointTLS, nullptr);
    InitMainThread();
}

void System_Thread::SetStackTraceEntryPoint(void* entryPoint) {
    InitThreadSystem();
    InternalSetLocalStorage(&s_stackTraceEntryPointTLS, entryPoint);
}

Blizzard::Thread::ThreadRecord* System_Thread::GetThreadRecord() {
    InitThreadSystem();

    void* record = pthread_getspecific(~s_threadRecordTLS.m_key);

    // The proxied game thread (and any emscripten worker) is not started by us, so adopt it on first use.
    return record ? static_cast<Blizzard::Thread::ThreadRecord*>(record) : AdoptCurrentThread();
}

bool System_Thread::AllocateTLSSlot(Blizzard::Thread::TLSSlot* slot, Blizzard::Thread::TLSDestructor destructor) {
    InitThreadSystem();
    Blizzard::Lock::ScopedMutex lock(s_mutex);

    return slot->m_key || AllocateLocalStorage(slot, destructor);
}

void System_Thread::InitStackTraceEntryPoint() {
}

uint32_t System_Thread::ExternalTLSWatcher(void* param) {
    return 0;
}

uint32_t __stdcall System_Thread::ThreadStarter(void* param) {
    Blizzard::Thread::ThreadRecord* thread = static_cast<Blizzard::Thread::ThreadRecord*>(param);

    InternalSetLocalStorage(&s_threadRecordTLS, thread);
    InitStackTraceEntryPoint();

    while (!thread->m_state) {
        Blizzard::Thread::Sleep(1);
    }

    uint32_t result = thread->m_function(thread->m_param);
    thread->m_result = result;
    RunTLSDestructors();
    thread->m_state = -1;
    Blizzard::Thread::Close(thread);

    return result;
}

static void* ThreadEntry(void* param) {
    System_Thread::ThreadStarter(param);
    return nullptr;
}

bool Blizzard::Thread::Create(uint32_t (*function)(void*), void* param, ThreadRecord** thread, const char* name, int32_t flags, int32_t stackSize) {
    System_Thread::InitThreadSystem();
    ThreadRecord* record = System_Thread::NewThread(function, param, name);
    *thread = record;

    if (!stackSize) {
        stackSize = System_Thread::s_DefaultStackSize;
    }

    if (stackSize < kMinStackSize) {
        stackSize = kMinStackSize;
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, static_cast<size_t>(stackSize));
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    // The starter spins until m_state is non-zero, so flag 1 (created suspended) just leaves it at zero for the caller to set.
    record->m_state = (flags & 1) == 0;

    pthread_t id;
    int error = pthread_create(&id, &attr, ThreadEntry, record);
    pthread_attr_destroy(&attr);

    if (error) {
        record->m_state = -1;
        Close(record);
        Close(record);
        *thread = nullptr;
        return false;
    }

    record->m_handle = reinterpret_cast<HANDLE>(id);
    record->m_threadId = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(id));

    return true;
}

void System_Thread::RegisterExternalTLSDestructor(void* value, Blizzard::Thread::TLSDestructor destructor) {
    // ponytail: values stored by foreign threads leak instead of being destroyed when the thread exits.
}

void System_Thread::InternalSetLocalStorage(const Blizzard::Thread::TLSSlot* slot, const void* value) {
    pthread_setspecific(~slot->m_key, value);
}

#endif
