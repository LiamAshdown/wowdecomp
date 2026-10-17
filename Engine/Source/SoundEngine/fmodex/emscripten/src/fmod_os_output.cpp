#include "fmod_settings.h"

#include "fmod.h"
#include "fmod_memory.h"
#include "fmod_outputi.h"
#include "fmod_pluginfactory.h"
#include "fmod_os_output.h"
#include "fmod_string.h"
#include "fmod_systemi.h"

#include <pthread.h>
#include <unistd.h>

/*
    Web Audio output for the browser build.

    FMOD's software mixer runs on its own pthread and renders float stereo blocks into a ring buffer in wasm
    shared memory. An AudioWorkletProcessor (library_fmod_webaudio.js) reads the ring on the audio thread.
    The ring is single-producer/single-consumer and lock-free: header[0] is the frame count written by the
    mixer, header[1] the frame count consumed by the worklet. The worklet plays silence when fewer than one
    render quantum is buffered, so underruns never replay stale data.

    Latency is (Sound_DSPBufferSize - 1) blocks of 1024 frames, i.e. 3 blocks (~64 ms at 48 kHz) by default.
*/

extern "C"
{
    int  fmod_webaudio_available(void);
    int  fmod_webaudio_open(int rate);
    void fmod_webaudio_start(int *ring, int capacity, int channels);
    void fmod_webaudio_stop(int *ring);
}

static const FMOD_OUTPUTTYPE FMOD_OUTPUTTYPE_WEBAUDIO = FMOD_OUTPUTTYPE_UNKNOWN;
static const int             WEBAUDIO_HEADER_INTS     = 4;    /* keeps the sample data 16-byte aligned */

namespace FMOD
{

static FMOD_OUTPUT_DESCRIPTION_EX webaudiooutput;

/* Allocated by PluginFactory as a plain Output of mSize bytes, so no virtuals and no constructor. */
class OutputWebAudio : public Output
{
  public:

    int           *mRing;
    unsigned int   mBlock;
    unsigned int   mTarget;
    unsigned int   mCapacity;
    int            mChannels;
    int            mRunning;
    pthread_t      mThread;

    static void *mixThread(void *arg)
    {
        OutputWebAudio *out  = (OutputWebAudio *)arg;
        float          *data = (float *)(out->mRing + WEBAUDIO_HEADER_INTS);
        unsigned int    written = 0;
        unsigned int    pos = 0;

        while (__atomic_load_n(&out->mRunning, __ATOMIC_ACQUIRE))
        {
            unsigned int consumed = (unsigned int)__atomic_load_n(&out->mRing[1], __ATOMIC_ACQUIRE);

            while ((int)(written - consumed) <= (int)(out->mTarget - out->mBlock))
            {
                out->mix(data + pos * out->mChannels, out->mBlock);

                pos += out->mBlock;
                if (pos >= out->mCapacity)
                {
                    pos = 0;
                }
                written += out->mBlock;

                __atomic_store_n(&out->mRing[0], (int)written, __ATOMIC_RELEASE);
            }

            /* ponytail: 2 ms poll, a block is ~21 ms so this never starves; Atomics.notify from the worklet if CPU matters */
            usleep(2000);
        }

        return 0;
    }

    static FMOD_RESULT F_CALLBACK getNumDriversCallback(FMOD_OUTPUT_STATE *output, int *numdrivers)
    {
        *numdrivers = 1;
        return FMOD_OK;
    }

    static FMOD_RESULT F_CALLBACK getDriverNameCallback(FMOD_OUTPUT_STATE *output, int id, char *name, int namelen)
    {
        FMOD_strncpy(name, "Web Audio", namelen);
        return FMOD_OK;
    }

    static FMOD_RESULT F_CALLBACK getDriverCapsExCallback(FMOD_OUTPUT_STATE *output, int id, FMOD_CAPS *caps, int *minfrequency, int *maxfrequency, FMOD_SPEAKERMODE *controlpanelspeakermode)
    {
        *caps = (FMOD_CAPS)(*caps | FMOD_CAPS_OUTPUT_FORMAT_PCMFLOAT);
        *minfrequency = 8000;
        *maxfrequency = 96000;
        *controlpanelspeakermode = FMOD_SPEAKERMODE_STEREO;
        return FMOD_OK;
    }

    static FMOD_RESULT F_CALLBACK initCallback(FMOD_OUTPUT_STATE *output, int selecteddriver, FMOD_INITFLAGS flags, int *outputrate, int outputchannels, FMOD_SOUND_FORMAT *outputformat, int dspbufferlength, int dspnumbuffers, void *extradriverdata)
    {
        OutputWebAudio *out = (OutputWebAudio *)output;
        int             rate;

        out->Plugin::init();

        rate = fmod_webaudio_open(*outputrate);
        if (!rate)
        {
            return FMOD_ERR_OUTPUT_INIT;
        }

        /* The AudioContext decides the rate; FMOD mixes at it so the worklet never resamples. */
        *outputrate   = rate;
        *outputformat = FMOD_SOUND_FORMAT_PCMFLOAT;

        out->mBlock    = dspbufferlength;
        out->mTarget   = dspbufferlength * (dspnumbuffers > 1 ? dspnumbuffers - 1 : 1);
        out->mCapacity = out->mTarget + dspbufferlength;
        out->mRing     = 0;

        return FMOD_OK;
    }

    static FMOD_RESULT F_CALLBACK startCallback(FMOD_OUTPUT_STATE *output)
    {
        OutputWebAudio *out = (OutputWebAudio *)output;
        FMOD_RESULT     result;
        pthread_attr_t  attr;
        int             rc;

        result = out->mSystem->getSoftwareFormat(0, 0, &out->mChannels, 0, 0, 0);
        if (result != FMOD_OK)
        {
            return result;
        }

        out->mRing = (int *)FMOD_Memory_Calloc(WEBAUDIO_HEADER_INTS * sizeof(int) + out->mCapacity * out->mChannels * sizeof(float));
        if (!out->mRing)
        {
            return FMOD_ERR_MEMORY;
        }

        out->mRunning = 1;

        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 512 * 1024);
        rc = pthread_create(&out->mThread, &attr, mixThread, out);
        pthread_attr_destroy(&attr);

        if (rc)
        {
            out->mRunning = 0;
            FMOD_Memory_Free(out->mRing);
            out->mRing = 0;
            return FMOD_ERR_OUTPUT_INIT;
        }

        fmod_webaudio_start(out->mRing, out->mCapacity, out->mChannels);

        return FMOD_OK;
    }

    static FMOD_RESULT F_CALLBACK stopCallback(FMOD_OUTPUT_STATE *output)
    {
        OutputWebAudio *out = (OutputWebAudio *)output;

        if (!out->mRing)
        {
            return FMOD_OK;
        }

        fmod_webaudio_stop(out->mRing);    /* synchronous: the worklet node is disconnected before the ring is freed */

        __atomic_store_n(&out->mRunning, 0, __ATOMIC_RELEASE);
        pthread_join(out->mThread, 0);

        FMOD_Memory_Free(out->mRing);
        out->mRing = 0;

        return FMOD_OK;
    }

    static FMOD_RESULT F_CALLBACK closeCallback(FMOD_OUTPUT_STATE *output)
    {
        return stopCallback(output);
    }

    static FMOD_OUTPUT_DESCRIPTION_EX *getDescriptionEx()
    {
        FMOD_memset(&webaudiooutput, 0, sizeof(FMOD_OUTPUT_DESCRIPTION_EX));

        webaudiooutput.name            = "FMOD Web Audio Output";
        webaudiooutput.version         = 0x00010100;
        webaudiooutput.polling         = false;
        webaudiooutput.getnumdrivers   = &getNumDriversCallback;
        webaudiooutput.getdrivername   = &getDriverNameCallback;
        webaudiooutput.getdrivercapsex = &getDriverCapsExCallback;
        webaudiooutput.init            = &initCallback;
        webaudiooutput.close           = &closeCallback;
        webaudiooutput.start           = &startCallback;
        webaudiooutput.stop            = &stopCallback;

        webaudiooutput.mType           = FMOD_OUTPUTTYPE_WEBAUDIO;
        webaudiooutput.mSize           = sizeof(OutputWebAudio);

        return &webaudiooutput;
    }
};

}


FMOD_RESULT FMOD_OS_Output_Register(FMOD::PluginFactory *pluginfactory)
{
    return pluginfactory->registerOutput(FMOD::OutputWebAudio::getDescriptionEx());
}


/* Web Audio when the page has AudioWorklet and SharedArrayBuffer, else NOSOUND (registered by the system itself). */
FMOD_RESULT FMOD_OS_Output_GetDefault(FMOD_OUTPUTTYPE *outputtype)
{
    if (!outputtype)
    {
        return FMOD_ERR_INVALID_PARAM;
    }

    *outputtype = fmod_webaudio_available() ? FMOD_OUTPUTTYPE_WEBAUDIO : FMOD_OUTPUTTYPE_NOSOUND;

    return FMOD_OK;
}
