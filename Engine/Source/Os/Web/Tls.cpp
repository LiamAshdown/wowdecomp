#if defined(WOWDECOMP_PLATFORM_WEB)

#include <cstdint>
#include <pthread.h>
#include "Engine/Source/Os/Tls.h"

uint32_t OsTlsAlloc() {
    pthread_key_t key;

    if (pthread_key_create(&key, nullptr)) {
        return 0xFFFFFFFF;
    }

    return key;
}

void OsTlsFree(uint32_t index) {
    pthread_key_delete(index);
}

void* OsTlsGetValue(uint32_t index) {
    return pthread_getspecific(index);
}

int32_t OsTlsSetValue(uint32_t index, void* value) {
    return pthread_setspecific(index, value) == 0;
}

#endif
