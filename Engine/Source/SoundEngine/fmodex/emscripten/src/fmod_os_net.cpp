#include "fmod_settings.h"

#include "fmod_net.h"
#include "fmod_os_net.h"

/*
    Browsers have no raw sockets, so FMOD's network features (profiler listener, net file streaming) are unavailable.
    Init succeeds so that callers get a normal connection failure from Connect/Listen instead of failing initialization.
*/
FMOD_RESULT FMOD_OS_Net_Init()
{
    return FMOD_OK;
}


FMOD_RESULT FMOD_OS_Net_Shutdown()
{
    return FMOD_OK;
}


FMOD_RESULT FMOD_OS_Net_Connect(const char *host, const unsigned short port, void **handle)
{
    if (handle)
    {
        *handle = (void *)-1;
    }

    return FMOD_ERR_NET_CONNECT;
}


FMOD_RESULT FMOD_OS_Net_Listen(const unsigned short port, void **listenhandle)
{
    if (listenhandle)
    {
        *listenhandle = (void *)-1;
    }

    return FMOD_ERR_NET_SOCKET_ERROR;
}


FMOD_RESULT FMOD_OS_Net_Accept(const void *listenhandle, void **clienthandle)
{
    return FMOD_ERR_NET_SOCKET_ERROR;
}


FMOD_RESULT FMOD_OS_Net_Close(const void *handle)
{
    return FMOD_OK;
}


FMOD_RESULT FMOD_OS_Net_Write(const void *handle, const char *buf, const unsigned int len, unsigned int *byteswritten)
{
    if (byteswritten)
    {
        *byteswritten = 0;
    }

    return FMOD_ERR_NET_SOCKET_ERROR;
}


FMOD_RESULT FMOD_OS_Net_Read(const void *handle, char *buf, const unsigned int len, unsigned int *bytesread)
{
    if (bytesread)
    {
        *bytesread = 0;
    }

    return FMOD_ERR_NET_SOCKET_ERROR;
}


FMOD_RESULT FMOD_OS_Net_ReadLine(const void *handle, char *buf, const unsigned int len)
{
    if (buf && len)
    {
        buf[0] = 0;
    }

    return FMOD_ERR_NET_SOCKET_ERROR;
}
