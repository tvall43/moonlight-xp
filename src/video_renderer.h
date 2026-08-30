#ifndef VIDEO_RENDERER_H
#define VIDEO_RENDERER_H

#include <windows.h>
#include <stdbool.h>
#include "Limelight.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    HWND hwnd;
    int stream_width;
    int stream_height;
    int stream_fps;
    bool fullscreen;
    bool quit_requested;
} VideoRendererContext;

extern DECODER_RENDERER_CALLBACKS g_VideoCallbacks;
extern VideoRendererContext g_VideoContext;

bool video_renderer_init(HWND parent_hwnd, int width, int height, int fps, bool fullscreen);
void video_renderer_destroy(void);

#ifdef __cplusplus
}
#endif

#endif // VIDEO_RENDERER_H
