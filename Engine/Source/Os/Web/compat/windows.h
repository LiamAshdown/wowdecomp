// Web build stand-in for <windows.h>: the Win32 types, CRT names and the few static inline APIs that shared
// (non-platform) code uses. Platform code goes in Web/ directories, not here.
#pragma once

#include <pthread.h>
#include <stdint.h>
#include <unistd.h>
#include "msvc_crt.h"

// ---- types -------------------------------------------------------------------------------------------------------

typedef int BOOL;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef long LONG;
typedef unsigned long ULONG;
typedef short SHORT;
typedef unsigned short USHORT;
typedef unsigned int UINT;
typedef int INT;
typedef char CHAR;
typedef unsigned char UCHAR;
typedef float FLOAT;
typedef int64_t LONGLONG;
typedef uint64_t ULONGLONG;

typedef uint64_t DWORD64;
typedef intptr_t INT_PTR;
typedef uintptr_t UINT_PTR;
typedef intptr_t LONG_PTR;
typedef uintptr_t ULONG_PTR;
typedef uintptr_t DWORD_PTR;
typedef size_t SIZE_T;
typedef void* LPVOID;
typedef const void* LPCVOID;
typedef void* PVOID;
typedef char* LPSTR;
typedef const char* LPCSTR;
typedef DWORD* LPDWORD;
typedef BYTE* LPBYTE;
typedef LONG* PLONG;
typedef void* HANDLE;
struct HINSTANCE__;
struct HWND__;
typedef struct HINSTANCE__* HINSTANCE;
typedef HINSTANCE HMODULE;
typedef struct HWND__* HWND;
typedef HANDLE HDC;
typedef HANDLE HBITMAP;
typedef HANDLE HKEY;
typedef UINT_PTR WPARAM;
typedef LONG_PTR LPARAM;
typedef LONG_PTR LRESULT;
typedef LONG HRESULT;
typedef void* FARPROC;
typedef uint16_t WCHAR;
typedef DWORD COLORREF;

typedef struct tagSIZE {
    LONG cx, cy;
} SIZE;

typedef union _LARGE_INTEGER {
    struct {
        DWORD LowPart;
        LONG HighPart;
    };
    LONGLONG QuadPart;
} LARGE_INTEGER;

typedef struct _OVERLAPPED {
    ULONG_PTR Internal;
    ULONG_PTR InternalHigh;
    union {
        struct {
            DWORD Offset;
            DWORD OffsetHigh;
        };
        PVOID Pointer;
    };
    HANDLE hEvent;
} OVERLAPPED, *LPOVERLAPPED;

typedef struct _FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME;

typedef struct _SYSTEMTIME {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME;

typedef struct tagPOINT {
    LONG x, y;
} POINT;

typedef struct tagRECT {
    LONG left, top, right, bottom;
} RECT;

typedef struct _GUID {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
} GUID;

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#define INVALID_HANDLE_VALUE ((HANDLE)(LONG_PTR)-1)
#define INFINITE 0xFFFFFFFF
#define MAX_PATH 260
#define WINAPI
#define CALLBACK
#define APIENTRY
#define __stdcall
#define __cdecl
#define __fastcall
#define S_OK ((HRESULT)0)
#define ERROR_SUCCESS 0L
#define MAKELONG(a, b) ((LONG)(((WORD)((DWORD_PTR)(a) & 0xffff)) | ((DWORD)((WORD)((DWORD_PTR)(b) & 0xffff))) << 16))
#define LOWORD(l) ((WORD)((DWORD_PTR)(l) & 0xffff))
#define HIWORD(l) ((WORD)((DWORD_PTR)(l) >> 16))

// ---- interlocked -------------------------------------------------------------------------------------------------

static inline LONG InterlockedIncrement(volatile LONG* p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedDecrement(volatile LONG* p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedExchange(volatile LONG* p, LONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedExchangeAdd(volatile LONG* p, LONG v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }

static inline LONG InterlockedCompareExchange(volatile LONG* p, LONG v, LONG cmp) {
    __atomic_compare_exchange_n(p, &cmp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}

static inline PVOID InterlockedCompareExchangePointer(PVOID volatile* p, PVOID v, PVOID cmp) {
    __atomic_compare_exchange_n(p, &cmp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}

static inline PVOID InterlockedExchangePointer(PVOID volatile* p, PVOID v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }

// ---- critical sections (recursive pthread mutex) -----------------------------------------------------------------

typedef struct _CRITICAL_SECTION {
    pthread_mutex_t mutex;
} CRITICAL_SECTION, *LPCRITICAL_SECTION;

static inline void InitializeCriticalSection(CRITICAL_SECTION* cs) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&cs->mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}

static inline BOOL InitializeCriticalSectionAndSpinCount(CRITICAL_SECTION* cs, DWORD spin) {
    InitializeCriticalSection(cs);
    return TRUE;
}

static inline void DeleteCriticalSection(CRITICAL_SECTION* cs) { pthread_mutex_destroy(&cs->mutex); }
static inline void EnterCriticalSection(CRITICAL_SECTION* cs) { pthread_mutex_lock(&cs->mutex); }
static inline BOOL TryEnterCriticalSection(CRITICAL_SECTION* cs) { return pthread_mutex_trylock(&cs->mutex) == 0; }
static inline void LeaveCriticalSection(CRITICAL_SECTION* cs) { pthread_mutex_unlock(&cs->mutex); }

// ---- process -------------------------------------------------------------------------------------------------------

static inline HMODULE GetModuleHandleA(const char* name) { (void)name; return 0; }
static inline void ExitProcess(UINT code) { _exit((int)code); }
