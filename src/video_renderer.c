#include "video_renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <d3d9.h>

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

VideoRendererContext g_VideoContext = {0};

typedef IDirect3D9* (WINAPI *LPDIRECT3DCREATE9)(UINT);

static HMODULE g_hD3D9Lib = NULL;
static LPDIRECT3DCREATE9 pfnDirect3DCreate9 = NULL;

typedef enum {
    RENDER_BACKEND_D3D9,
    RENDER_BACKEND_GDI
} RenderBackend;

static RenderBackend g_Backend = RENDER_BACKEND_GDI;

static LPDIRECT3D9 g_pD3D = NULL;
static LPDIRECT3DDEVICE9 g_pd3dDevice = NULL;
static LPDIRECT3DSURFACE9 g_pOffscreenSurface = NULL;
static LPDIRECT3DSURFACE9 g_pBackBuffer = NULL;
static bool g_bUseHardwareYV12 = false;

// GDI fallback structures
static uint8_t* g_pGdiRgbBuffer = NULL;
static int g_GdiRgbBufferSize = 0;
static BITMAPINFO g_Bmi;

static const AVCodec* g_pCodec = NULL;
static AVCodecContext* g_pCodecCtx = NULL;
static AVFrame* g_pFrame = NULL;
static AVPacket* g_pPacket = NULL;
static struct SwsContext* g_pSwsCtx = NULL;

static CRITICAL_SECTION g_RenderLock;

#define FOURCC_YV12 0x32315659 // 'YV12'

static void render_frame_d3d9(AVFrame* frame) {
    D3DLOCKED_RECT lockedRect;
    if (g_bUseHardwareYV12 && g_pOffscreenSurface) {
        if (SUCCEEDED(IDirect3DSurface9_LockRect(g_pOffscreenSurface, &lockedRect, NULL, D3DLOCK_DISCARD))) {
            unsigned char* dst = (unsigned char*)lockedRect.pBits;
            int pitch = lockedRect.Pitch;

            // Y plane
            for (int y = 0; y < frame->height; y++) {
                memcpy(dst + y * pitch, frame->data[0] + y * frame->linesize[0], frame->width);
            }

            // V plane
            unsigned char* dstV = dst + pitch * frame->height;
            int uvPitch = pitch / 2;
            for (int y = 0; y < frame->height / 2; y++) {
                memcpy(dstV + y * uvPitch, frame->data[2] + y * frame->linesize[2], frame->width / 2);
            }

            // U plane
            unsigned char* dstU = dstV + uvPitch * (frame->height / 2);
            for (int y = 0; y < frame->height / 2; y++) {
                memcpy(dstU + y * uvPitch, frame->data[1] + y * frame->linesize[1], frame->width / 2);
            }

            IDirect3DSurface9_UnlockRect(g_pOffscreenSurface);
        }
    } else {
        if (!g_pSwsCtx) {
            g_pSwsCtx = sws_getContext(
                frame->width, frame->height, AV_PIX_FMT_YUV420P,
                frame->width, frame->height, AV_PIX_FMT_BGRA,
                SWS_FAST_BILINEAR, NULL, NULL, NULL);
        }

        if (g_pOffscreenSurface && SUCCEEDED(IDirect3DSurface9_LockRect(g_pOffscreenSurface, &lockedRect, NULL, D3DLOCK_DISCARD))) {
            uint8_t* dstData[4] = { (uint8_t*)lockedRect.pBits, 0, 0, 0 };
            int dstLinesize[4] = { lockedRect.Pitch, 0, 0, 0 };

            sws_scale(g_pSwsCtx, (const uint8_t* const*)frame->data, frame->linesize,
                      0, frame->height, dstData, dstLinesize);

            IDirect3DSurface9_UnlockRect(g_pOffscreenSurface);
        }
    }

    if (SUCCEEDED(IDirect3DDevice9_BeginScene(g_pd3dDevice))) {
        if (g_pOffscreenSurface && g_pBackBuffer) {
            RECT srcRect = { 0, 0, frame->width, frame->height };
            RECT dstRect;
            GetClientRect(g_VideoContext.hwnd, &dstRect);

            IDirect3DDevice9_StretchRect(g_pd3dDevice, g_pOffscreenSurface, &srcRect, g_pBackBuffer, &dstRect, D3DTEXF_LINEAR);
        }
        IDirect3DDevice9_EndScene(g_pd3dDevice);
        IDirect3DDevice9_Present(g_pd3dDevice, NULL, NULL, NULL, NULL);
    }
}

static void render_frame_gdi(AVFrame* frame) {
    if (!g_pGdiRgbBuffer || !g_VideoContext.hwnd) return;

    if (!g_pSwsCtx) {
        g_pSwsCtx = sws_getContext(
            frame->width, frame->height, AV_PIX_FMT_YUV420P,
            frame->width, frame->height, AV_PIX_FMT_BGRA,
            SWS_FAST_BILINEAR, NULL, NULL, NULL);
    }

    uint8_t* dstData[4] = { g_pGdiRgbBuffer, 0, 0, 0 };
    int dstLinesize[4] = { frame->width * 4, 0, 0, 0 };

    sws_scale(g_pSwsCtx, (const uint8_t* const*)frame->data, frame->linesize,
              0, frame->height, dstData, dstLinesize);

    HDC hdc = GetDC(g_VideoContext.hwnd);
    if (hdc) {
        RECT clientRect;
        GetClientRect(g_VideoContext.hwnd, &clientRect);
        int clientW = clientRect.right - clientRect.left;
        int clientH = clientRect.bottom - clientRect.top;

        SetStretchBltMode(hdc, COLORONCOLOR);
        StretchDIBits(
            hdc,
            0, 0, clientW, clientH,
            0, 0, frame->width, frame->height,
            g_pGdiRgbBuffer,
            &g_Bmi,
            DIB_RGB_COLORS,
            SRCCOPY);

        ReleaseDC(g_VideoContext.hwnd, hdc);
    }
}

static void render_frame(AVFrame* frame) {
    if (!frame) return;

    EnterCriticalSection(&g_RenderLock);

    if (g_Backend == RENDER_BACKEND_D3D9 && g_pd3dDevice) {
        render_frame_d3d9(frame);
    } else {
        render_frame_gdi(frame);
    }

    LeaveCriticalSection(&g_RenderLock);
}

static bool init_d3d9_backend(int width, int height) {
    g_hD3D9Lib = LoadLibraryA("d3d9.dll");
    if (!g_hD3D9Lib) return false;

    pfnDirect3DCreate9 = (LPDIRECT3DCREATE9)GetProcAddress(g_hD3D9Lib, "Direct3DCreate9");
    if (!pfnDirect3DCreate9) return false;

    g_pD3D = pfnDirect3DCreate9(D3D_SDK_VERSION);
    if (!g_pD3D) return false;

    D3DPRESENT_PARAMETERS d3dpp;
    memset(&d3dpp, 0, sizeof(d3dpp));
    d3dpp.Windowed = TRUE;
    d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    d3dpp.hDeviceWindow = g_VideoContext.hwnd;
    d3dpp.BackBufferFormat = D3DFMT_X8R8G8B8;
    d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    HRESULT hr = IDirect3D9_CreateDevice(
        g_pD3D, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_VideoContext.hwnd,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
        &d3dpp, &g_pd3dDevice);

    if (FAILED(hr)) {
        hr = IDirect3D9_CreateDevice(
            g_pD3D, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_VideoContext.hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
            &d3dpp, &g_pd3dDevice);
    }

    if (FAILED(hr) || !g_pd3dDevice) return false;

    IDirect3DDevice9_GetBackBuffer(g_pd3dDevice, 0, 0, D3DBACKBUFFER_TYPE_MONO, &g_pBackBuffer);

    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(
        g_pd3dDevice, width, height, (D3DFORMAT)FOURCC_YV12,
        D3DPOOL_DEFAULT, &g_pOffscreenSurface, NULL);

    if (SUCCEEDED(hr)) {
        g_bUseHardwareYV12 = true;
    } else {
        g_bUseHardwareYV12 = false;
        IDirect3DDevice9_CreateOffscreenPlainSurface(
            g_pd3dDevice, width, height, D3DFMT_X8R8G8B8,
            D3DPOOL_DEFAULT, &g_pOffscreenSurface, NULL);
    }

    return true;
}

static int dr_setup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
    g_VideoContext.stream_width = width;
    g_VideoContext.stream_height = height;
    g_VideoContext.stream_fps = redrawRate;

    InitializeCriticalSection(&g_RenderLock);

    // Initialize FFmpeg H.264 decoder
    g_pCodec = avcodec_find_decoder_by_name("h264");
    if (!g_pCodec) {
        g_pCodec = avcodec_find_decoder(AV_CODEC_ID_H264);
    }
    if (!g_pCodec) return -1;

    g_pCodecCtx = avcodec_alloc_context3(g_pCodec);
    if (!g_pCodecCtx) return -1;

    g_pCodecCtx->width = width;
    g_pCodecCtx->height = height;
    g_pCodecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    g_pCodecCtx->thread_count = 2;
    g_pCodecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    g_pCodecCtx->flags2 |= AV_CODEC_FLAG2_FAST;

    if (avcodec_open2(g_pCodecCtx, g_pCodec, NULL) < 0) {
        return -1;
    }

    g_pFrame = av_frame_alloc();
    g_pPacket = av_packet_alloc();

    // Prepare GDI fallback DIB buffer
    g_GdiRgbBufferSize = width * height * 4;
    g_pGdiRgbBuffer = (uint8_t*)malloc(g_GdiRgbBufferSize);

    memset(&g_Bmi, 0, sizeof(g_Bmi));
    g_Bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    g_Bmi.bmiHeader.biWidth = width;
    g_Bmi.bmiHeader.biHeight = -height; // Negative for top-down DIB
    g_Bmi.bmiHeader.biPlanes = 1;
    g_Bmi.bmiHeader.biBitCount = 32;
    g_Bmi.bmiHeader.biCompression = BI_RGB;

    // Try Direct3D 9 first; if unavailable, fallback to pure GDI
    if (init_d3d9_backend(width, height)) {
        g_Backend = RENDER_BACKEND_D3D9;
    } else {
        g_Backend = RENDER_BACKEND_GDI;
    }

    return DR_OK;
}

static void dr_start(void) {}

static void dr_stop(void) {}

static void dr_release(void) {
    EnterCriticalSection(&g_RenderLock);

    if (g_pOffscreenSurface) {
        IDirect3DSurface9_Release(g_pOffscreenSurface);
        g_pOffscreenSurface = NULL;
    }
    if (g_pBackBuffer) {
        IDirect3DSurface9_Release(g_pBackBuffer);
        g_pBackBuffer = NULL;
    }
    if (g_pd3dDevice) {
        IDirect3DDevice9_Release(g_pd3dDevice);
        g_pd3dDevice = NULL;
    }
    if (g_pD3D) {
        IDirect3D9_Release(g_pD3D);
        g_pD3D = NULL;
    }
    if (g_hD3D9Lib) {
        FreeLibrary(g_hD3D9Lib);
        g_hD3D9Lib = NULL;
        pfnDirect3DCreate9 = NULL;
    }

    if (g_pGdiRgbBuffer) {
        free(g_pGdiRgbBuffer);
        g_pGdiRgbBuffer = NULL;
    }

    if (g_pSwsCtx) {
        sws_freeContext(g_pSwsCtx);
        g_pSwsCtx = NULL;
    }
    if (g_pFrame) {
        av_frame_free(&g_pFrame);
    }
    if (g_pPacket) {
        av_packet_free(&g_pPacket);
    }
    if (g_pCodecCtx) {
        avcodec_free_context(&g_pCodecCtx);
    }

    LeaveCriticalSection(&g_RenderLock);
    DeleteCriticalSection(&g_RenderLock);
}

static int dr_submit_decode_unit(PDECODE_UNIT du) {
    if (!g_pCodecCtx || !du || du->fullLength == 0) return DR_OK;

    AVPacket* pkt = g_pPacket;
    av_packet_unref(pkt);

    if (du->bufferList && du->bufferList->next == NULL) {
        pkt->data = (uint8_t*)du->bufferList->data;
        pkt->size = du->bufferList->length;
    } else {
        if (av_new_packet(pkt, du->fullLength) < 0) return DR_OK;
        int offset = 0;
        for (PLENTRY entry = du->bufferList; entry != NULL; entry = entry->next) {
            memcpy(pkt->data + offset, entry->data, entry->length);
            offset += entry->length;
        }
    }

    int ret = avcodec_send_packet(g_pCodecCtx, pkt);
    if (ret < 0) return DR_OK;

    while (ret >= 0) {
        ret = avcodec_receive_frame(g_pCodecCtx, g_pFrame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            break;
        }
        render_frame(g_pFrame);
    }

    return DR_OK;
}

DECODER_RENDERER_CALLBACKS g_VideoCallbacks = {
    .setup = dr_setup,
    .start = dr_start,
    .stop = dr_stop,
    .cleanup = dr_release,
    .submitDecodeUnit = dr_submit_decode_unit,
    .capabilities = CAPABILITY_DIRECT_SUBMIT,
};
