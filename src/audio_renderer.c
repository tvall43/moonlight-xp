#include "audio_renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <opus/opus_multistream.h>

typedef HRESULT (WINAPI *LPDIRECTSOUNDCREATE)(LPCGUID, LPDIRECTSOUND*, LPUNKNOWN);

static HMODULE g_hDSoundLib = NULL;
static LPDIRECTSOUNDCREATE pfnDirectSoundCreate = NULL;

typedef enum {
    AUDIO_BACKEND_DSOUND,
    AUDIO_BACKEND_WAVEOUT,
    AUDIO_BACKEND_NULL
} AudioBackend;

static AudioBackend g_AudioBackend = AUDIO_BACKEND_NULL;

static HWND g_AudioHwnd = NULL;
static LPDIRECTSOUND g_pDS = NULL;
static LPDIRECTSOUNDBUFFER g_pDSBuffer = NULL;

// WaveOut fallback structures
#define NUM_WAVEOUT_BUFFERS 4
#define WAVEOUT_BUFFER_SAMPLES 960 // 20ms at 48kHz
static HWAVEOUT g_hWaveOut = NULL;
static WAVEHDR g_WaveHdrs[NUM_WAVEOUT_BUFFERS];
static int16_t* g_pWaveBuffers[NUM_WAVEOUT_BUFFERS];
static int g_CurrentWaveHdr = 0;

static OpusMSDecoder* g_pOpusDecoder = NULL;
static int g_AudioChannels = 2;
static int g_SampleRate = 48000;
static DWORD g_BufferSize = 0;
static DWORD g_WriteOffset = 0;

void audio_renderer_set_hwnd(HWND hwnd) {
    g_AudioHwnd = hwnd;
}

static bool init_dsound(WAVEFORMATEX* pwfx) {
    g_hDSoundLib = LoadLibraryA("dsound.dll");
    if (!g_hDSoundLib) return false;

    pfnDirectSoundCreate = (LPDIRECTSOUNDCREATE)GetProcAddress(g_hDSoundLib, "DirectSoundCreate");
    if (!pfnDirectSoundCreate) return false;

    if (FAILED(pfnDirectSoundCreate(NULL, &g_pDS, NULL)) || !g_pDS) {
        return false;
    }

    HWND hwnd = g_AudioHwnd ? g_AudioHwnd : GetDesktopWindow();
    IDirectSound_SetCooperativeLevel(g_pDS, hwnd, DSSCL_PRIORITY);

    g_BufferSize = pwfx->nAvgBytesPerSec / 10; // 100ms

    DSBUFFERDESC dsbd;
    memset(&dsbd, 0, sizeof(dsbd));
    dsbd.dwSize = sizeof(dsbd);
    dsbd.dwFlags = DSBCAPS_GLOBALFOCUS | DSBCAPS_GETCURRENTPOSITION2;
    dsbd.dwBufferBytes = g_BufferSize;
    dsbd.lpwfxFormat = pwfx;

    if (FAILED(IDirectSound_CreateSoundBuffer(g_pDS, &dsbd, &g_pDSBuffer, NULL)) || !g_pDSBuffer) {
        return false;
    }

    return true;
}

static bool init_waveout(WAVEFORMATEX* pwfx) {
    if (waveOutOpen(&g_hWaveOut, WAVE_MAPPER, pwfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        return false;
    }

    int bufBytes = WAVEOUT_BUFFER_SAMPLES * pwfx->nBlockAlign;
    for (int i = 0; i < NUM_WAVEOUT_BUFFERS; i++) {
        g_pWaveBuffers[i] = (int16_t*)malloc(bufBytes);
        memset(&g_WaveHdrs[i], 0, sizeof(WAVEHDR));
        g_WaveHdrs[i].lpData = (LPSTR)g_pWaveBuffers[i];
        g_WaveHdrs[i].dwBufferLength = bufBytes;
        g_WaveHdrs[i].dwFlags = WHDR_DONE;
    }
    g_CurrentWaveHdr = 0;
    return true;
}

static int ar_init(int audioConfiguration, const POPUS_MULTISTREAM_CONFIGURATION opusConfig, void* context, int arFlags) {
    g_AudioChannels = opusConfig->channelCount;
    g_SampleRate = opusConfig->sampleRate;

    int error = OPUS_OK;
    g_pOpusDecoder = opus_multistream_decoder_create(
        opusConfig->sampleRate,
        opusConfig->channelCount,
        opusConfig->streams,
        opusConfig->coupledStreams,
        opusConfig->mapping,
        &error);

    WAVEFORMATEX wfx;
    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = (WORD)g_AudioChannels;
    wfx.nSamplesPerSec = g_SampleRate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (WORD)(wfx.nChannels * (wfx.wBitsPerSample / 8));
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    // Try DirectSound first; fallback to waveOut; fallback to null silent sink
    if (init_dsound(&wfx)) {
        g_AudioBackend = AUDIO_BACKEND_DSOUND;
    } else if (init_waveout(&wfx)) {
        g_AudioBackend = AUDIO_BACKEND_WAVEOUT;
    } else {
        g_AudioBackend = AUDIO_BACKEND_NULL;
    }

    g_WriteOffset = 0;
    return 0;
}

static void ar_start(void) {
    if (g_AudioBackend == AUDIO_BACKEND_DSOUND && g_pDSBuffer) {
        IDirectSoundBuffer_SetCurrentPosition(g_pDSBuffer, 0);
        IDirectSoundBuffer_Play(g_pDSBuffer, 0, 0, DSBPLAY_LOOPING);
    }
}

static void ar_stop(void) {
    if (g_AudioBackend == AUDIO_BACKEND_DSOUND && g_pDSBuffer) {
        IDirectSoundBuffer_Stop(g_pDSBuffer);
    } else if (g_AudioBackend == AUDIO_BACKEND_WAVEOUT && g_hWaveOut) {
        waveOutReset(g_hWaveOut);
    }
}

static void ar_release(void) {
    if (g_pOpusDecoder) {
        opus_multistream_decoder_destroy(g_pOpusDecoder);
        g_pOpusDecoder = NULL;
    }

    if (g_pDSBuffer) {
        IDirectSoundBuffer_Release(g_pDSBuffer);
        g_pDSBuffer = NULL;
    }
    if (g_pDS) {
        IDirectSound_Release(g_pDS);
        g_pDS = NULL;
    }
    if (g_hDSoundLib) {
        FreeLibrary(g_hDSoundLib);
        g_hDSoundLib = NULL;
    }

    if (g_hWaveOut) {
        waveOutReset(g_hWaveOut);
        for (int i = 0; i < NUM_WAVEOUT_BUFFERS; i++) {
            if (g_WaveHdrs[i].dwFlags & WHDR_PREPARED) {
                waveOutUnprepareHeader(g_hWaveOut, &g_WaveHdrs[i], sizeof(WAVEHDR));
            }
            if (g_pWaveBuffers[i]) {
                free(g_pWaveBuffers[i]);
                g_pWaveBuffers[i] = NULL;
            }
        }
        waveOutClose(g_hWaveOut);
        g_hWaveOut = NULL;
    }

    g_AudioBackend = AUDIO_BACKEND_NULL;
}

static void play_dsound(const int16_t* pcm, int samples) {
    if (!g_pDSBuffer) return;

    DWORD bytesToWrite = samples * g_AudioChannels * sizeof(int16_t);
    void* p1 = NULL;
    void* p2 = NULL;
    DWORD s1 = 0, s2 = 0;

    DWORD playPos = 0, writePos = 0;
    IDirectSoundBuffer_GetCurrentPosition(g_pDSBuffer, &playPos, &writePos);

    HRESULT hr = IDirectSoundBuffer_Lock(g_pDSBuffer, g_WriteOffset, bytesToWrite, &p1, &s1, &p2, &s2, 0);
    if (hr == DSERR_BUFFERLOST) {
        IDirectSoundBuffer_Restore(g_pDSBuffer);
        hr = IDirectSoundBuffer_Lock(g_pDSBuffer, g_WriteOffset, bytesToWrite, &p1, &s1, &p2, &s2, 0);
    }

    if (SUCCEEDED(hr)) {
        if (p1 && s1 > 0) {
            memcpy(p1, pcm, s1);
        }
        if (p2 && s2 > 0) {
            memcpy(p2, (uint8_t*)pcm + s1, s2);
        }
        IDirectSoundBuffer_Unlock(g_pDSBuffer, p1, s1, p2, s2);
        g_WriteOffset = (g_WriteOffset + bytesToWrite) % g_BufferSize;
    }
}

static void play_waveout(const int16_t* pcm, int samples) {
    if (!g_hWaveOut) return;

    WAVEHDR* pHdr = &g_WaveHdrs[g_CurrentWaveHdr];
    if (pHdr->dwFlags & WHDR_PREPARED) {
        while (!(pHdr->dwFlags & WHDR_DONE)) {
            Sleep(1);
        }
        waveOutUnprepareHeader(g_hWaveOut, pHdr, sizeof(WAVEHDR));
    }

    int bytesToWrite = samples * g_AudioChannels * sizeof(int16_t);
    if (bytesToWrite > (int)pHdr->dwBufferLength) {
        bytesToWrite = pHdr->dwBufferLength;
    }

    memcpy(pHdr->lpData, pcm, bytesToWrite);
    pHdr->dwBufferLength = bytesToWrite;
    pHdr->dwFlags = 0;

    waveOutPrepareHeader(g_hWaveOut, pHdr, sizeof(WAVEHDR));
    waveOutWrite(g_hWaveOut, pHdr, sizeof(WAVEHDR));

    g_CurrentWaveHdr = (g_CurrentWaveHdr + 1) % NUM_WAVEOUT_BUFFERS;
}

static void ar_decode_and_play_sample(char* sampleData, int sampleLength) {
    if (g_AudioBackend == AUDIO_BACKEND_NULL || !g_pOpusDecoder) {
        return;
    }

    int16_t pcmBuf[5760 * 8];
    int decodedSamples = opus_multistream_decode(
        g_pOpusDecoder,
        (const unsigned char*)sampleData,
        sampleLength,
        pcmBuf,
        5760,
        0);

    if (decodedSamples <= 0) return;

    if (g_AudioBackend == AUDIO_BACKEND_DSOUND) {
        play_dsound(pcmBuf, decodedSamples);
    } else if (g_AudioBackend == AUDIO_BACKEND_WAVEOUT) {
        play_waveout(pcmBuf, decodedSamples);
    }
}

AUDIO_RENDERER_CALLBACKS g_AudioCallbacks = {
    .init = ar_init,
    .start = ar_start,
    .stop = ar_stop,
    .cleanup = ar_release,
    .decodeAndPlaySample = ar_decode_and_play_sample,
    .capabilities = 0,
};
