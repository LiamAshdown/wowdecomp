// Web build: the waitable object behind every Win32 event / semaphore / mutex / thread HANDLE that the Blizzard and
// Storm lock code hands around (CreateEvent, WaitForSingleObject and friends). Header-only so Storm can share it.
// ponytail: one global mutex + condvar guards every object (broadcast on any signal), so WaitForMultipleObjects is
// trivial. Upgrade to per-object condvars only if contention shows up in a profile.
#pragma once

#include <cstdint>
#include <pthread.h>
#include <time.h>

namespace WebSync {

enum Kind { KindEvent, KindSemaphore, KindMutex, KindThread };

const uint32_t kWaitTimeout = 0x102;
const uint32_t kWaitFailed = 0xFFFFFFFF;
const uint32_t kInfinite = 0xFFFFFFFF;

struct Obj {
    int kind;
    bool manual;      // events: manual reset
    bool signaled;    // events, threads
    uint32_t count;   // semaphores
    uint32_t max;     // semaphores
    pthread_t owner;  // mutexes
    uint32_t recursion;
    int refs;
};

struct Global {
    pthread_mutex_t mutex;
    pthread_cond_t cond;

    Global() {
        pthread_mutex_init(&mutex, nullptr);
        pthread_condattr_t attr;
        pthread_condattr_init(&attr);
        pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
        pthread_cond_init(&cond, &attr);
        pthread_condattr_destroy(&attr);
    }
};

inline Global& G() {
    static Global global;
    return global;
}

inline Obj* Create(int kind, bool manual, bool signaled, uint32_t count = 0, uint32_t max = 0) {
    Obj* obj = new Obj();
    obj->kind = kind;
    obj->manual = manual;
    obj->signaled = signaled;
    obj->count = count;
    obj->max = max;
    obj->owner = 0;
    obj->recursion = 0;
    obj->refs = 1;
    return obj;
}

inline void AddRef(Obj* obj) {
    pthread_mutex_lock(&G().mutex);
    obj->refs++;
    pthread_mutex_unlock(&G().mutex);
}

// CloseHandle: drops one reference.
inline void Release(Obj* obj) {
    if (!obj) {
        return;
    }

    pthread_mutex_lock(&G().mutex);
    int refs = --obj->refs;
    pthread_mutex_unlock(&G().mutex);

    if (refs == 0) {
        delete obj;
    }
}

// Caller holds G().mutex.
inline bool Ready(const Obj* obj) {
    switch (obj->kind) {
        case KindSemaphore:
            return obj->count > 0;
        case KindMutex:
            return obj->recursion == 0 || pthread_equal(obj->owner, pthread_self());
        default:
            return obj->signaled;
    }
}

// Caller holds G().mutex and has checked Ready().
inline void Take(Obj* obj) {
    switch (obj->kind) {
        case KindSemaphore:
            obj->count--;
            break;
        case KindMutex:
            obj->owner = pthread_self();
            obj->recursion++;
            break;
        case KindEvent:
            if (!obj->manual) {
                obj->signaled = false;
            }
            break;
        default:
            break;
    }
}

// Returns 0 + index (WAIT_OBJECT_0 + n), kWaitTimeout, or kWaitFailed.
inline uint32_t WaitMultiple(uint32_t count, Obj** objs, bool waitAll, uint32_t timeoutMs) {
    if (!count) {
        return kWaitFailed;
    }

    struct timespec deadline;

    if (timeoutMs != kInfinite) {
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += timeoutMs / 1000;
        deadline.tv_nsec += static_cast<long>(timeoutMs % 1000) * 1000000L;

        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
    }

    Global& g = G();
    pthread_mutex_lock(&g.mutex);

    for (;;) {
        if (waitAll) {
            uint32_t ready = 0;

            for (uint32_t i = 0; i < count; i++) {
                ready += Ready(objs[i]);
            }

            if (ready == count) {
                for (uint32_t i = 0; i < count; i++) {
                    Take(objs[i]);
                }

                pthread_mutex_unlock(&g.mutex);
                return 0;
            }
        } else {
            for (uint32_t i = 0; i < count; i++) {
                if (Ready(objs[i])) {
                    Take(objs[i]);
                    pthread_mutex_unlock(&g.mutex);
                    return i;
                }
            }
        }

        if (timeoutMs == 0) {
            pthread_mutex_unlock(&g.mutex);
            return kWaitTimeout;
        }

        if (timeoutMs == kInfinite) {
            pthread_cond_wait(&g.cond, &g.mutex);
        } else if (pthread_cond_timedwait(&g.cond, &g.mutex, &deadline) != 0) {
            // One last look so a signal that raced the timeout is not lost.
            bool done = false;
            uint32_t index = 0;

            if (!waitAll) {
                for (; index < count && !(done = Ready(objs[index])); index++) {
                }

                if (done) {
                    Take(objs[index]);
                }
            }

            pthread_mutex_unlock(&g.mutex);
            return done ? index : kWaitTimeout;
        }
    }
}

inline uint32_t Wait(Obj* obj, uint32_t timeoutMs) {
    return obj ? WaitMultiple(1, &obj, false, timeoutMs) : kWaitFailed;
}

inline void SetSignaled(Obj* obj, bool signaled) {
    pthread_mutex_lock(&G().mutex);
    obj->signaled = signaled;
    pthread_mutex_unlock(&G().mutex);

    if (signaled) {
        pthread_cond_broadcast(&G().cond);
    }
}

// ReleaseSemaphore. Returns false if it would pass the maximum count.
inline bool ReleaseSemaphore(Obj* obj, uint32_t count) {
    pthread_mutex_lock(&G().mutex);
    bool ok = obj->count + count <= obj->max;

    if (ok) {
        obj->count += count;
    }

    pthread_mutex_unlock(&G().mutex);
    pthread_cond_broadcast(&G().cond);

    return ok;
}

// ReleaseMutex. Returns false if the caller does not own it.
inline bool ReleaseMutex(Obj* obj) {
    pthread_mutex_lock(&G().mutex);
    bool ok = obj->recursion && pthread_equal(obj->owner, pthread_self());

    if (ok) {
        obj->recursion--;
    }

    pthread_mutex_unlock(&G().mutex);
    pthread_cond_broadcast(&G().cond);

    return ok;
}

}
