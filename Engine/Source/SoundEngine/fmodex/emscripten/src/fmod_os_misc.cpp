#include "fmod_settings.h"

#include "fmod.h"
#include "fmod_os_misc.h"
#include "fmod_memory.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/*
    Platform layer for the browser build: pthreads (the game runs with -sPROXY_TO_PTHREAD), clock_gettime and POSIX files.
*/

int gSizeofCriticalSection = sizeof(pthread_mutex_t);

struct FMOD_OS_SEMAPHORE
{
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    unsigned int    count;
};

int gSizeofSemaphore       = sizeof(FMOD_OS_SEMAPHORE);

static pthread_mutex_t gMemoryCrit;


void *FMOD_OS_Memory_Alloc(int size, FMOD_MEMORY_TYPE type)
{
    return malloc(size);
}


void *FMOD_OS_Memory_Realloc(void *ptr, int size, FMOD_MEMORY_TYPE type)
{
    return realloc(ptr, size);
}


void FMOD_OS_Memory_Free(void *ptr, FMOD_MEMORY_TYPE type)
{
    free(ptr);
}


FMOD_RESULT FMOD_OS_File_DriveStatus()
{
    return FMOD_OK;
}


/* File handles are the descriptor plus one so that descriptor 0 is not a null handle. */
FMOD_RESULT FMOD_OS_File_Open(const char *name, char *mode, int unicode, unsigned int *filesize, void **handle)
{
    char        utf8[1024];
    struct stat st;
    int         fd;

    if (unicode)
    {
        const unsigned short *wide = (const unsigned short *)name;
        unsigned int          pos  = 0;

        for (; *wide && pos < sizeof(utf8) - 4; wide++)
        {
            unsigned int c = *wide;

            if (c < 0x80)
            {
                utf8[pos++] = (char)c;
            }
            else if (c < 0x800)
            {
                utf8[pos++] = (char)(0xC0 | (c >> 6));
                utf8[pos++] = (char)(0x80 | (c & 0x3F));
            }
            else
            {
                utf8[pos++] = (char)(0xE0 | (c >> 12));
                utf8[pos++] = (char)(0x80 | ((c >> 6) & 0x3F));
                utf8[pos++] = (char)(0x80 | (c & 0x3F));
            }
        }

        utf8[pos] = 0;
        name = utf8;
    }

    fd = open(name, O_RDONLY);
    if (fd < 0)
    {
        if (errno != ENOENT && errno != ENOTDIR && errno != EINVAL)
        {
            return FMOD_ERR_FILE_BAD;
        }

        return FMOD_ERR_FILE_NOTFOUND;
    }

    if (fstat(fd, &st))
    {
        close(fd);
        return FMOD_ERR_FILE_BAD;
    }

    *filesize = (unsigned int)st.st_size;
    *handle   = (void *)(size_t)(fd + 1);

    return FMOD_OK;
}


FMOD_RESULT FMOD_OS_File_Close(void *handle)
{
    if (!handle)
    {
        return FMOD_ERR_FILE_BAD;
    }

    close((int)(size_t)handle - 1);

    return FMOD_OK;
}


FMOD_RESULT FMOD_OS_File_Cancel(void *handle)
{
    return FMOD_OK;
}


FMOD_RESULT FMOD_OS_File_Read(void *handle, void *buf, unsigned int count, unsigned int *read)
{
    int          fd    = (int)(size_t)handle - 1;
    unsigned int total = 0;

    while (total < count)
    {
        ssize_t rd = ::read(fd, (char *)buf + total, count - total);

        if (rd < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            return FMOD_ERR_FILE_BAD;
        }

        if (!rd)
        {
            break;
        }

        total += (unsigned int)rd;
    }

    *read = total;

    if (total != count)
    {
        return FMOD_ERR_FILE_EOF;
    }

    return FMOD_OK;
}


FMOD_RESULT FMOD_OS_File_Seek(void *handle, unsigned int offset)
{
    if (lseek((int)(size_t)handle - 1, (off_t)offset, SEEK_SET) < 0)
    {
        return FMOD_ERR_FILE_BAD;
    }

    return FMOD_OK;
}


FMOD_RESULT FMOD_OS_Debug_OutputStr(const char *s)
{
    fputs(s, stderr);

    return FMOD_OK;
}


/* Like the Win32 layer this is microseconds (despite the name), truncated to 32 bits. */
FMOD_RESULT FMOD_OS_Time_GetNs(unsigned int *ns)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    *ns = (unsigned int)((unsigned long long)ts.tv_sec * 1000000ULL + (unsigned long long)ts.tv_nsec / 1000ULL);

    return FMOD_OK;
}


FMOD_RESULT FMOD_OS_Time_GetMs(unsigned int *ms)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    *ms = (unsigned int)((unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)ts.tv_nsec / 1000000ULL);

    return FMOD_OK;
}


FMOD_RESULT FMOD_OS_Time_Sleep(unsigned int ms)
{
    struct timespec ts;

    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;

    while (nanosleep(&ts, &ts) && errno == EINTR)
    {
    }

    return FMOD_OK;
}


FMOD_RESULT FMOD_OS_Thread_GetCurrentID(FMOD_UINT_NATIVE *id)
{
    *id = (FMOD_UINT_NATIVE)(size_t)pthread_self();

    return FMOD_OK;
}


struct FMOD_OS_THREADSTART
{
    THREAD_RETURNTYPE (THREAD_CALLCONV *func)(void *param);
    void *param;
};

static void *FMOD_OS_ThreadMain(void *arg)
{
    FMOD_OS_THREADSTART start = *(FMOD_OS_THREADSTART *)arg;

    free(arg);
    start.func(start.param);

    return 0;
}


/* Threads are detached; the handle only exists so that Destroy has something to release. Priorities do not exist in the browser. */
FMOD_RESULT FMOD_OS_Thread_Create(const char *name, THREAD_RETURNTYPE (THREAD_CALLCONV *func)(void *param), void *param, FMOD_THREAD_PRIORITY priority, void *stack, int stacksize, void **handle)
{
    pthread_attr_t       attr;
    pthread_t            thread;
    FMOD_OS_THREADSTART *start;
    int                  rc;

    if (!handle)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    start = (FMOD_OS_THREADSTART *)malloc(sizeof(FMOD_OS_THREADSTART));
    if (!start)
    {
        return FMOD_ERR_MEMORY;
    }

    start->func  = func;
    start->param = param;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, stacksize > 512 * 1024 ? (size_t)stacksize : 512 * 1024);

    rc = pthread_create(&thread, &attr, FMOD_OS_ThreadMain, start);
    pthread_attr_destroy(&attr);

    if (rc)
    {
        free(start);
        return FMOD_ERR_MEMORY;
    }

    *handle = malloc(sizeof(pthread_t));
    if (*handle)
    {
        *(pthread_t *)*handle = thread;
    }

    return FMOD_OK;
}


FMOD_RESULT FMOD_OS_Thread_Destroy(void *handle)
{
    free(handle);

    return FMOD_OK;
}


FMOD_RESULT FMOD_OS_CriticalSection_Create(FMOD_OS_CRITICALSECTION **crit, bool memorycrit)
{
    pthread_mutexattr_t attr;
    pthread_mutex_t    *mutex;

    if (!crit)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    if (memorycrit)
    {
        mutex = &gMemoryCrit;    /* Cannot use alloc on the critical section used in the memory system. */
    }
    else
    {
        mutex = (pthread_mutex_t *)FMOD_Memory_Alloc(sizeof(pthread_mutex_t));
        if (!mutex)
        {
            return FMOD_ERR_MEMORY;
        }
    }

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    *crit = (FMOD_OS_CRITICALSECTION *)mutex;

    return FMOD_OK;
}


FMOD_RESULT FMOD_OS_CriticalSection_Free(FMOD_OS_CRITICALSECTION *crit)
{
    if (!crit)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    pthread_mutex_destroy((pthread_mutex_t *)crit);

    if ((pthread_mutex_t *)crit != &gMemoryCrit)
    {
        FMOD_Memory_Free(crit);
    }

    return FMOD_OK;
}


FMOD_RESULT FMOD_OS_CriticalSection_Enter(FMOD_OS_CRITICALSECTION *crit)
{
    if (!crit)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    pthread_mutex_lock((pthread_mutex_t *)crit);

    return FMOD_OK;
}


FMOD_RESULT FMOD_OS_CriticalSection_Leave(FMOD_OS_CRITICALSECTION *crit)
{
    if (!crit)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    pthread_mutex_unlock((pthread_mutex_t *)crit);

    return FMOD_OK;
}


FMOD_RESULT FMOD_OS_Semaphore_Create(FMOD_OS_SEMAPHORE **sema)
{
    if (!sema)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    *sema = (FMOD_OS_SEMAPHORE *)malloc(sizeof(FMOD_OS_SEMAPHORE));
    if (!*sema)
    {
        return FMOD_ERR_MEMORY;
    }

    pthread_mutex_init(&(*sema)->mutex, 0);
    pthread_cond_init(&(*sema)->cond, 0);
    (*sema)->count = 0;

    return FMOD_OK;
}


FMOD_RESULT FMOD_OS_Semaphore_Free(FMOD_OS_SEMAPHORE *sema)
{
    if (!sema)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    pthread_cond_destroy(&sema->cond);
    pthread_mutex_destroy(&sema->mutex);
    free(sema);

    return FMOD_OK;
}


FMOD_RESULT FMOD_OS_Semaphore_Wait(FMOD_OS_SEMAPHORE *sema)
{
    if (!sema)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    pthread_mutex_lock(&sema->mutex);

    while (!sema->count)
    {
        pthread_cond_wait(&sema->cond, &sema->mutex);
    }

    sema->count--;
    pthread_mutex_unlock(&sema->mutex);

    return FMOD_OK;
}


FMOD_RESULT FMOD_OS_Semaphore_Signal(FMOD_OS_SEMAPHORE *sema, bool interrupt)
{
    if (!sema)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    pthread_mutex_lock(&sema->mutex);
    sema->count++;
    pthread_cond_signal(&sema->cond);
    pthread_mutex_unlock(&sema->mutex);

    return FMOD_OK;
}


/* No dynamic code loading in wasm. */
FMOD_RESULT FMOD_OS_Library_Load(const char *dllname, FMOD_OS_LIBRARY **handle)
{
    return FMOD_ERR_UNSUPPORTED;
}


FMOD_RESULT FMOD_OS_Library_GetProcAddress(FMOD_OS_LIBRARY *handle, const char *procname, void **address)
{
    return FMOD_ERR_UNSUPPORTED;
}


FMOD_RESULT FMOD_OS_Library_Free(FMOD_OS_LIBRARY *handle)
{
    return FMOD_ERR_UNSUPPORTED;
}


FMOD_RESULT FMOD_OS_CheckDriverList(bool *devicelistchanged)
{
    *devicelistchanged = false;

    return FMOD_OK;
}
