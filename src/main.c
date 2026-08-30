#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>

#include "Limelight.h"
#include "http_client.h"
#include "nv_pairing.h"
#include "video_renderer.h"
#include "audio_renderer.h"
#include "input_handler.h"
#include <psa/crypto.h>

#define IDC_EDIT_HOST      1001
#define IDC_BTN_CONNECT    1002
#define IDC_BTN_PAIR       1003
#define IDC_STATIC_STATUS  1004
#define IDC_LIST_APPS      1005
#define IDC_COMBO_RES      1006
#define IDC_COMBO_FPS      1007
#define IDC_EDIT_BITRATE   1008
#define IDC_CHK_FULLSCREEN 1009
#define IDC_BTN_LAUNCH     1010
#define IDC_BTN_QUITAPP    1011

#define WM_USER_PAIR_DONE  (WM_USER + 101)

static HINSTANCE g_hInstance;
static HWND g_hMainWnd;
static HWND g_hEditHost;
static HWND g_hBtnConnect;
static HWND g_hBtnPair;
static HWND g_hStaticStatus;
static HWND g_hListApps;
static HWND g_hComboRes;
static HWND g_hComboFps;
static HWND g_hEditBitrate;
static HWND g_hChkFullscreen;
static HWND g_hBtnLaunch;
static HWND g_hBtnQuitApp;

static NvClientIdentity g_Ident;
static NvServerInfo g_ServerInfo;
static bool g_IsStreaming = false;
static HANDLE g_hStreamThread = NULL;
static HANDLE g_hPairThread = NULL;

typedef struct {
    int width;
    int height;
    int fps;
    int bitrate_kbps;
    bool fullscreen;
    int app_id;
    char host[128];
} StreamSettings;

static StreamSettings g_StreamSettings;

typedef struct {
    char host[128];
    int https_port;
    char pin[8];
} PairThreadParams;

static PairThreadParams g_PairParams;

static void log_message(const char* format, ...) {
    char buf[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    OutputDebugStringA(buf);
}

static void cl_stage_starting(int stage) {
    log_message("[Moonlight] Stage starting: %s\n", LiGetStageName(stage));
}

static void cl_stage_complete(int stage) {
    log_message("[Moonlight] Stage complete: %s\n", LiGetStageName(stage));
}

static void cl_stage_failed(int stage, int errorCode) {
    log_message("[Moonlight] Stage failed: %s (error %d)\n", LiGetStageName(stage), errorCode);
    char buf[128];
    snprintf(buf, sizeof(buf), "Stage '%s' (%d) failed (error %d)", LiGetStageName(stage), stage, errorCode);
    MessageBoxA(NULL, buf, "Stream Stage Error", MB_ICONERROR);
}

static void cl_connection_started(void) {
    log_message("[Moonlight] Connection established!\n");
}

static void cl_connection_terminated(int errorCode) {
    log_message("[Moonlight] Connection terminated: %d\n", errorCode);
    if (errorCode != 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Stream terminated unexpectedly (error %d)", errorCode);
        MessageBoxA(NULL, buf, "Stream Terminated", MB_ICONWARNING);
    }
    if (g_VideoContext.hwnd) {
        PostMessage(g_VideoContext.hwnd, WM_CLOSE, 0, 0);
    }
}

static void cl_log_message(const char* format, ...) {
    char buf[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    OutputDebugStringA(buf);
}

static CONNECTION_LISTENER_CALLBACKS g_ConnCallbacks = {
    .stageStarting = cl_stage_starting,
    .stageComplete = cl_stage_complete,
    .stageFailed = cl_stage_failed,
    .connectionStarted = cl_connection_started,
    .connectionTerminated = cl_connection_terminated,
    .logMessage = cl_log_message,
};

static LRESULT CALLBACK StreamWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (input_handler_process_message(hwnd, uMsg, wParam, lParam)) {
        return 0;
    }

    switch (uMsg) {
    case WM_CLOSE:
        g_IsStreaming = false;
        LiStopConnection();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_VideoContext.hwnd = NULL;
        return 0;
    }

    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

static DWORD WINAPI StreamThreadProc(LPVOID lpParam) {
    StreamSettings* settings = (StreamSettings*)lpParam;

    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = StreamWndProc;
    wc.hInstance = g_hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "MoonlightStreamWndClass";
    RegisterClassExA(&wc);

    DWORD style = WS_OVERLAPPEDWINDOW;
    int posX = CW_USEDEFAULT, posY = CW_USEDEFAULT;
    int winW = settings->width;
    int winH = settings->height;

    if (settings->fullscreen) {
        style = WS_POPUP | WS_VISIBLE;
        posX = 0;
        posY = 0;
        winW = GetSystemMetrics(SM_CXSCREEN);
        winH = GetSystemMetrics(SM_CYSCREEN);
    }

    HWND hwndStream = CreateWindowExA(
        0, "MoonlightStreamWndClass", "Moonlight XP Stream",
        style, posX, posY, winW, winH,
        NULL, NULL, g_hInstance, NULL);

    if (!hwndStream) {
        MessageBoxA(g_hMainWnd, "Failed to create streaming window", "Error", MB_ICONERROR);
        g_IsStreaming = false;
        ShowWindow(g_hMainWnd, SW_SHOW);
        return 1;
    }

    g_VideoContext.hwnd = hwndStream;
    g_VideoContext.fullscreen = settings->fullscreen;
    g_VideoContext.quit_requested = false;

    audio_renderer_set_hwnd(hwndStream);
    input_handler_init(hwndStream);

    ShowWindow(hwndStream, SW_SHOW);
    UpdateWindow(hwndStream);

    char rtsp_session_url[256] = {0};
    if (!nv_launch_app(settings->host, g_ServerInfo.https_port, &g_Ident, settings->app_id,
                      settings->width, settings->height, settings->fps, settings->bitrate_kbps,
                      rtsp_session_url, sizeof(rtsp_session_url))) {
        MessageBoxA(g_hMainWnd, "Failed to launch application on server", "Error", MB_ICONERROR);
        DestroyWindow(hwndStream);
        g_IsStreaming = false;
        ShowWindow(g_hMainWnd, SW_SHOW);
        return 1;
    }

    STREAM_CONFIGURATION streamConfig;
    LiInitializeStreamConfiguration(&streamConfig);
    streamConfig.width = settings->width;
    streamConfig.height = settings->height;
    streamConfig.fps = settings->fps;
    streamConfig.bitrate = settings->bitrate_kbps;
    streamConfig.packetSize = 1024;
    streamConfig.streamingRemotely = STREAM_CFG_AUTO;
    streamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    streamConfig.supportedVideoFormats = VIDEO_FORMAT_H264;
    streamConfig.encryptionFlags = ENCFLG_NONE;

    memcpy(streamConfig.remoteInputAesKey, "0123456789ABCDEF", 16);

    SERVER_INFORMATION serverInfo;
    LiInitializeServerInformation(&serverInfo);
    serverInfo.address = settings->host;
    serverInfo.serverInfoAppVersion = "7.1.431.0";
    serverInfo.serverInfoGfeVersion = "3.22.0.32";
    serverInfo.rtspSessionUrl = rtsp_session_url;
    serverInfo.serverCodecModeSupport = SCM_H264 | SCM_HEVC;

    g_IsStreaming = true;

    int res = LiStartConnection(
        &serverInfo, &streamConfig, &g_ConnCallbacks,
        &g_VideoCallbacks, &g_AudioCallbacks,
        NULL, 0, NULL, 0);

    if (res != 0) {
        char errBuf[128];
        snprintf(errBuf, sizeof(errBuf), "Stream connection error: %d", res);
        MessageBoxA(g_hMainWnd, errBuf, "Error", MB_ICONERROR);
    }

    MSG msg;
    while (g_IsStreaming) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_IsStreaming = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        input_handler_poll_gamepads();
        Sleep(1);
    }

    LiStopConnection();
    input_handler_destroy();
    if (g_VideoContext.hwnd) {
        DestroyWindow(g_VideoContext.hwnd);
    }

    ShowWindow(g_hMainWnd, SW_SHOW);
    return 0;
}

static void get_ini_path(char* path, size_t size) {
    GetModuleFileNameA(NULL, path, (DWORD)size);
    char* last_slash = strrchr(path, '\\');
    if (last_slash) {
        *(last_slash + 1) = '\0';
        strncat(path, "moonlight.ini", size - strlen(path) - 1);
    } else {
        snprintf(path, size, "moonlight.ini");
    }
}

static void save_server_ip(const char* ip) {
    char ini[MAX_PATH];
    get_ini_path(ini, sizeof(ini));
    WritePrivateProfileStringA("Moonlight", "ServerIP", ip, ini);
}

static void load_server_ip(char* out_ip, size_t size) {
    char ini[MAX_PATH];
    get_ini_path(ini, sizeof(ini));
    GetPrivateProfileStringA("Moonlight", "ServerIP", "", out_ip, (DWORD)size, ini);
}

static void on_btn_connect(void) {
    char host[128];
    GetWindowTextA(g_hEditHost, host, sizeof(host));
    if (strlen(host) == 0) {
        MessageBoxA(g_hMainWnd, "Please enter a server hostname or IP address.", "Info", MB_OK);
        return;
    }

    save_server_ip(host);

    SetWindowTextA(g_hStaticStatus, "Status: Connecting...");
    UpdateWindow(g_hStaticStatus);

    if (nv_get_server_info(host, 47984, &g_ServerInfo)) {
        // Attempt to fetch app list over HTTPS
        bool app_ok = nv_get_app_list(host, g_ServerInfo.https_port, &g_Ident, &g_ServerInfo);
        if (app_ok && g_ServerInfo.app_count > 0) {
            g_ServerInfo.is_paired = true;
        }

        char status[256];
        if (g_ServerInfo.is_paired) {
            snprintf(status, sizeof(status), "Status: %s (%s) - Paired (%d apps)",
                     g_ServerInfo.hostname, g_ServerInfo.server_version, g_ServerInfo.app_count);
        } else {
            snprintf(status, sizeof(status), "Status: %s (%s) - Not Paired",
                     g_ServerInfo.hostname, g_ServerInfo.server_version);
        }
        SetWindowTextA(g_hStaticStatus, status);

        EnableWindow(g_hBtnPair, !g_ServerInfo.is_paired);

        SendMessage(g_hListApps, LB_RESETCONTENT, 0, 0);
        if (g_ServerInfo.is_paired && g_ServerInfo.app_count > 0) {
            for (int i = 0; i < g_ServerInfo.app_count; i++) {
                int idx = (int)SendMessageA(g_hListApps, LB_ADDSTRING, 0, (LPARAM)g_ServerInfo.apps[i].name);
                SendMessageA(g_hListApps, LB_SETITEMDATA, idx, (LPARAM)g_ServerInfo.apps[i].id);
            }
            SendMessageA(g_hListApps, LB_SETCURSEL, 0, 0);
            EnableWindow(g_hBtnLaunch, TRUE);
            EnableWindow(g_hBtnQuitApp, TRUE);
        } else {
            EnableWindow(g_hBtnLaunch, FALSE);
            EnableWindow(g_hBtnQuitApp, FALSE);
        }
    } else {
        SetWindowTextA(g_hStaticStatus, "Status: Connection failed. Check IP & firewall.");
    }
}

static DWORD WINAPI PairThreadProc(LPVOID lpParam) {
    PairThreadParams* params = (PairThreadParams*)lpParam;
    bool success = nv_pair_server(params->host, params->https_port, &g_Ident, params->pin);
    PostMessageA(g_hMainWnd, WM_USER_PAIR_DONE, success ? 1 : 0, 0);
    return 0;
}

static void on_btn_pair(void) {
    char host[128];
    GetWindowTextA(g_hEditHost, host, sizeof(host));
    if (strlen(host) == 0) {
        MessageBoxA(g_hMainWnd, "Please enter a server IP.", "Info", MB_OK);
        return;
    }

    save_server_ip(host);

    strncpy(g_PairParams.host, host, sizeof(g_PairParams.host));
    g_PairParams.https_port = g_ServerInfo.https_port > 0 ? g_ServerInfo.https_port : 47984;

    int pinNum = rand() % 10000;
    snprintf(g_PairParams.pin, sizeof(g_PairParams.pin), "%04d", pinNum);

    char statusBuf[256];
    snprintf(statusBuf, sizeof(statusBuf), "Pairing... Enter PIN [ %s ] on server!", g_PairParams.pin);
    SetWindowTextA(g_hStaticStatus, statusBuf);

    EnableWindow(g_hBtnPair, FALSE);
    EnableWindow(g_hBtnConnect, FALSE);

    g_hPairThread = CreateThread(NULL, 0, PairThreadProc, &g_PairParams, 0, NULL);
}

static void on_pair_done(bool success) {
    EnableWindow(g_hBtnPair, TRUE);
    EnableWindow(g_hBtnConnect, TRUE);

    if (success) {
        g_ServerInfo.is_paired = true;
        SetWindowTextA(g_hStaticStatus, "Status: Paired! Loading apps...");
        on_btn_connect();
    } else {
        MessageBoxA(g_hMainWnd, "Pairing failed or timed out. Make sure the PIN was submitted on the server.", "Pairing Error", MB_ICONERROR);
        SetWindowTextA(g_hStaticStatus, "Status: Pairing failed.");
    }
}

static void on_btn_launch(void) {
    int sel = (int)SendMessage(g_hListApps, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) {
        MessageBoxA(g_hMainWnd, "Please select an app to stream.", "Info", MB_OK);
        return;
    }

    int appId = (int)SendMessage(g_hListApps, LB_GETITEMDATA, sel, 0);

    GetWindowTextA(g_hEditHost, g_StreamSettings.host, sizeof(g_StreamSettings.host));
    g_StreamSettings.app_id = appId;

    int resSel = (int)SendMessage(g_hComboRes, CB_GETCURSEL, 0, 0);
    switch (resSel) {
    case 0: g_StreamSettings.width = 1280; g_StreamSettings.height = 720; break;
    case 1: g_StreamSettings.width = 1920; g_StreamSettings.height = 1080; break;
    case 2: g_StreamSettings.width = 1024; g_StreamSettings.height = 768; break;
    case 3: g_StreamSettings.width = 800;  g_StreamSettings.height = 600; break;
    default: g_StreamSettings.width = 1280; g_StreamSettings.height = 720; break;
    }

    int fpsSel = (int)SendMessage(g_hComboFps, CB_GETCURSEL, 0, 0);
    g_StreamSettings.fps = (fpsSel == 1) ? 30 : 60;

    char bitrateBuf[32];
    GetWindowTextA(g_hEditBitrate, bitrateBuf, sizeof(bitrateBuf));
    int mbps = atoi(bitrateBuf);
    if (mbps <= 0) mbps = 10;
    g_StreamSettings.bitrate_kbps = mbps * 1000;

    g_StreamSettings.fullscreen = (SendMessage(g_hChkFullscreen, BM_GETCHECK, 0, 0) == BST_CHECKED);

    ShowWindow(g_hMainWnd, SW_HIDE);

    g_hStreamThread = CreateThread(NULL, 0, StreamThreadProc, &g_StreamSettings, 0, NULL);
}

static void on_btn_quit_app(void) {
    char host[128];
    GetWindowTextA(g_hEditHost, host, sizeof(host));
    if (nv_quit_app(host, g_ServerInfo.https_port, &g_Ident)) {
        MessageBoxA(g_hMainWnd, "Sent quit command to active session.", "Quit App", MB_OK);
    }
}

static BOOL CALLBACK SetChildFont(HWND hwndChild, LPARAM lParam) {
    SendMessage(hwndChild, WM_SETFONT, (WPARAM)lParam, TRUE);
    return TRUE;
}

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_CREATE:
        {
            HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

            char saved_ip[128] = {0};
            load_server_ip(saved_ip, sizeof(saved_ip));
            const char* initial_ip = (strlen(saved_ip) > 0) ? saved_ip : "192.168.1.100";

            CreateWindowA("STATIC", "Server IP / Hostname:", WS_CHILD | WS_VISIBLE, 20, 15, 160, 20, hwnd, NULL, g_hInstance, NULL);
            g_hEditHost = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", initial_ip, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 20, 35, 220, 22, hwnd, (HMENU)IDC_EDIT_HOST, g_hInstance, NULL);
            g_hBtnConnect = CreateWindowA("BUTTON", "Connect", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 250, 34, 80, 24, hwnd, (HMENU)IDC_BTN_CONNECT, g_hInstance, NULL);
            g_hBtnPair = CreateWindowA("BUTTON", "Pair", WS_CHILD | WS_VISIBLE, 335, 34, 80, 24, hwnd, (HMENU)IDC_BTN_PAIR, g_hInstance, NULL);
            EnableWindow(g_hBtnPair, FALSE);

            g_hStaticStatus = CreateWindowA("STATIC", "Status: Ready", WS_CHILD | WS_VISIBLE, 20, 65, 400, 20, hwnd, (HMENU)IDC_STATIC_STATUS, g_hInstance, NULL);

            CreateWindowA("STATIC", "Available Applications:", WS_CHILD | WS_VISIBLE, 20, 95, 200, 20, hwnd, NULL, g_hInstance, NULL);
            g_hListApps = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "", WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL | WS_BORDER, 20, 115, 240, 160, hwnd, (HMENU)IDC_LIST_APPS, g_hInstance, NULL);

            CreateWindowA("STATIC", "Resolution:", WS_CHILD | WS_VISIBLE, 280, 115, 100, 18, hwnd, NULL, g_hInstance, NULL);
            g_hComboRes = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 280, 135, 140, 120, hwnd, (HMENU)IDC_COMBO_RES, g_hInstance, NULL);
            SendMessageA(g_hComboRes, CB_ADDSTRING, 0, (LPARAM)"1280 x 720 (720p)");
            SendMessageA(g_hComboRes, CB_ADDSTRING, 0, (LPARAM)"1920 x 1080 (1080p)");
            SendMessageA(g_hComboRes, CB_ADDSTRING, 0, (LPARAM)"1024 x 768 (4:3)");
            SendMessageA(g_hComboRes, CB_ADDSTRING, 0, (LPARAM)"800 x 600 (SVGA)");
            SendMessageA(g_hComboRes, CB_SETCURSEL, 0, 0);

            CreateWindowA("STATIC", "Frame Rate:", WS_CHILD | WS_VISIBLE, 280, 165, 100, 18, hwnd, NULL, g_hInstance, NULL);
            g_hComboFps = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 280, 185, 140, 60, hwnd, (HMENU)IDC_COMBO_FPS, g_hInstance, NULL);
            SendMessageA(g_hComboFps, CB_ADDSTRING, 0, (LPARAM)"60 FPS");
            SendMessageA(g_hComboFps, CB_ADDSTRING, 0, (LPARAM)"30 FPS");
            SendMessageA(g_hComboFps, CB_SETCURSEL, 0, 0);

            CreateWindowA("STATIC", "Bitrate (Mbps):", WS_CHILD | WS_VISIBLE, 280, 215, 100, 18, hwnd, NULL, g_hInstance, NULL);
            g_hEditBitrate = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "10", WS_CHILD | WS_VISIBLE | ES_NUMBER, 280, 235, 60, 22, hwnd, (HMENU)IDC_EDIT_BITRATE, g_hInstance, NULL);

            g_hChkFullscreen = CreateWindowA("BUTTON", "Fullscreen", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 350, 236, 90, 20, hwnd, (HMENU)IDC_CHK_FULLSCREEN, g_hInstance, NULL);

            g_hBtnLaunch = CreateWindowA("BUTTON", "Start Streaming", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 20, 290, 140, 32, hwnd, (HMENU)IDC_BTN_LAUNCH, g_hInstance, NULL);
            g_hBtnQuitApp = CreateWindowA("BUTTON", "Quit Running App", WS_CHILD | WS_VISIBLE, 170, 290, 130, 32, hwnd, (HMENU)IDC_BTN_QUITAPP, g_hInstance, NULL);
            EnableWindow(g_hBtnLaunch, FALSE);
            EnableWindow(g_hBtnQuitApp, FALSE);

            EnumChildWindows(hwnd, SetChildFont, (LPARAM)hFont);
        }
        return 0;

    case WM_USER_PAIR_DONE:
        on_pair_done(wParam != 0);
        return 0;

    case WM_COMMAND:
        {
            int wmId = LOWORD(wParam);
            switch (wmId) {
            case IDC_BTN_CONNECT:
                on_btn_connect();
                break;
            case IDC_BTN_PAIR:
                on_btn_pair();
                break;
            case IDC_BTN_LAUNCH:
                on_btn_launch();
                break;
            case IDC_BTN_QUITAPP:
                on_btn_quit_app();
                break;
            }
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    g_hInstance = hInstance;

    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    psa_crypto_init();

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    srand((unsigned int)time(NULL));

    nv_init_client_identity(&g_Ident, NULL);

    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "MoonlightXPMainWnd";
    RegisterClassExA(&wc);

    g_hMainWnd = CreateWindowExA(
        WS_EX_WINDOWEDGE, "MoonlightXPMainWnd", "Moonlight for Windows XP",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 460, 370,
        NULL, NULL, hInstance, NULL);

    if (!g_hMainWnd) return 1;

    ShowWindow(g_hMainWnd, nCmdShow);
    UpdateWindow(g_hMainWnd);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return (int)msg.wParam;
}
