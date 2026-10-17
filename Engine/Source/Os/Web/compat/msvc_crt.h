// Web build: MSVC CRT names used by the decompiled code. Force-included into every web translation unit.
#pragma once

#include <alloca.h>
#include <ctype.h>
#include <float.h>
#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>

#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define stricmp strcasecmp
#define strnicmp strncasecmp
#define _snprintf snprintf
#define _vsnprintf vsnprintf
#define _alloca alloca
#define _finite isfinite
#define _isnan isnan
#define _strtoi64 strtoll
#define _strtoui64 strtoull
#define _countof(a) (sizeof(a) / sizeof((a)[0]))

static inline char* _strupr(char* s) {
    for (char* p = s; *p; ++p) *p = (char)toupper((unsigned char)*p);
    return s;
}

static inline char* _strlwr(char* s) {
    for (char* p = s; *p; ++p) *p = (char)tolower((unsigned char)*p);
    return s;
}

static inline unsigned int GetACP(void) {
    return 1252;
}

#define __forceinline inline __attribute__((always_inline))
#define _msize malloc_usable_size
#define _atoi64 atoll
#define _ismbcspace isspace
#define _cexit()

static inline int fopen_s(FILE** f, const char* path, const char* mode) {
    *f = fopen(path, mode);
    return *f ? 0 : errno;
}

// MSVC's rand() range. The decompiled code scales rand() by 1/32767 (DayNight's cloud noise), and musl's RAND_MAX is
// 0x7FFFFFFF, which blew the noise up 65536 times into black blocks. <cstdlib>'s "using ::rand" picks this up too.
static inline int wowdecomp_rand(void) {
    return rand() & 0x7FFF;
}
#define rand wowdecomp_rand
#undef RAND_MAX
#define RAND_MAX 0x7FFF

// MSVC's std::type_info::raw_name() (decorated name, used only as an allocation tag); other compilers have name().
#define raw_name name

typedef int64_t __int64;
#define _rotl(v, s) ((unsigned int)(((unsigned int)(v) << ((s) & 31)) | ((unsigned int)(v) >> ((32 - ((s) & 31)) & 31))))
#define _rotr(v, s) ((unsigned int)(((unsigned int)(v) >> ((s) & 31)) | ((unsigned int)(v) << ((32 - ((s) & 31)) & 31))))
