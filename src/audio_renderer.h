#ifndef AUDIO_RENDERER_H
#define AUDIO_RENDERER_H

#include <windows.h>
#include "Limelight.h"

#ifdef __cplusplus
extern "C" {
#endif

extern AUDIO_RENDERER_CALLBACKS g_AudioCallbacks;

void audio_renderer_set_hwnd(HWND hwnd);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_RENDERER_H
