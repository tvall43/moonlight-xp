#include "input_handler.h"
#include "video_renderer.h"
#include "Limelight.h"

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <xinput.h>

static HWND g_StreamHwnd = NULL;
static bool g_MouseCaptured = false;
static POINT g_LastCursorPos = {0, 0};
static bool g_KeysDown[256] = {0};

typedef DWORD (WINAPI *LPFN_XINPUTGETSTATE)(DWORD dwUserIndex, XINPUT_STATE* pState);
static LPFN_XINPUTGETSTATE pfnXInputGetState = NULL;
static HMODULE g_hXInput = NULL;

void input_handler_init(HWND stream_hwnd) {
    g_StreamHwnd = stream_hwnd;

    // Load XInput dynamically (supports XP with DirectX End-User Runtimes)
    g_hXInput = LoadLibraryA("xinput1_3.dll");
    if (!g_hXInput) g_hXInput = LoadLibraryA("xinput9_1_0.dll");
    if (!g_hXInput) g_hXInput = LoadLibraryA("xinput1_4.dll");

    if (g_hXInput) {
        pfnXInputGetState = (LPFN_XINPUTGETSTATE)GetProcAddress(g_hXInput, "XInputGetState");
    }
}

void input_handler_destroy(void) {
    input_handler_set_capture(false);
    if (g_hXInput) {
        FreeLibrary(g_hXInput);
        g_hXInput = NULL;
        pfnXInputGetState = NULL;
    }
}

void input_handler_set_capture(bool capture) {
    g_MouseCaptured = capture;
    if (capture && g_StreamHwnd) {
        SetCapture(g_StreamHwnd);
        RECT rc;
        GetClientRect(g_StreamHwnd, &rc);
        MapWindowPoints(g_StreamHwnd, NULL, (LPPOINT)&rc, 2);
        ClipCursor(&rc);
        ShowCursor(FALSE);
        GetCursorPos(&g_LastCursorPos);
    } else {
        ReleaseCapture();
        ClipCursor(NULL);
        ShowCursor(TRUE);
    }
}

static char get_active_modifiers(void) {
    char mod = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) mod |= MODIFIER_SHIFT;
    if (GetKeyState(VK_CONTROL) & 0x8000) mod |= MODIFIER_CTRL;
    if (GetKeyState(VK_MENU) & 0x8000) mod |= MODIFIER_ALT;
    if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) mod |= MODIFIER_META;
    return mod;
}

bool input_handler_process_message(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_LBUTTONDOWN:
        {
            RECT rc;
            GetClientRect(hwnd, &rc);
            int width = rc.right - rc.left;
            int height = rc.bottom - rc.top;
            if (!g_MouseCaptured && width > 0 && height > 0) {
                short x = (short)LOWORD(lParam);
                short y = (short)HIWORD(lParam);
                LiSendMousePositionEvent(x, y, (short)width, (short)height);
                input_handler_set_capture(true);
            }
            LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT);
        }
        return true;
    case WM_LBUTTONUP:
        LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
        return true;
    case WM_RBUTTONDOWN:
        {
            RECT rc;
            GetClientRect(hwnd, &rc);
            int width = rc.right - rc.left;
            int height = rc.bottom - rc.top;
            if (!g_MouseCaptured && width > 0 && height > 0) {
                short x = (short)LOWORD(lParam);
                short y = (short)HIWORD(lParam);
                LiSendMousePositionEvent(x, y, (short)width, (short)height);
                input_handler_set_capture(true);
            }
            LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_RIGHT);
        }
        return true;
    case WM_RBUTTONUP:
        LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
        return true;
    case WM_MBUTTONDOWN:
        if (!g_MouseCaptured) input_handler_set_capture(true);
        LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_MIDDLE);
        return true;
    case WM_MBUTTONUP:
        LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_MIDDLE);
        return true;
    case WM_XBUTTONDOWN:
        if (HIWORD(wParam) == XBUTTON1) LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_X1);
        else if (HIWORD(wParam) == XBUTTON2) LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_X2);
        return true;
    case WM_XBUTTONUP:
        if (HIWORD(wParam) == XBUTTON1) LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_X1);
        else if (HIWORD(wParam) == XBUTTON2) LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_X2);
        return true;

    case WM_MOUSEMOVE:
        if (g_MouseCaptured) {
            POINT pt;
            GetCursorPos(&pt);
            short dx = (short)(pt.x - g_LastCursorPos.x);
            short dy = (short)(pt.y - g_LastCursorPos.y);

            if (dx != 0 || dy != 0) {
                LiSendMouseMoveEvent(dx, dy);

                // Reset cursor to window center to prevent hitting screen edges
                RECT rc;
                GetClientRect(hwnd, &rc);
                POINT center = { (rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2 };
                ClientToScreen(hwnd, &center);
                SetCursorPos(center.x, center.y);
                g_LastCursorPos = center;
            }
        } else {
            RECT rc;
            GetClientRect(hwnd, &rc);
            int width = rc.right - rc.left;
            int height = rc.bottom - rc.top;
            if (width > 0 && height > 0) {
                short x = (short)LOWORD(lParam);
                short y = (short)HIWORD(lParam);
                LiSendMousePositionEvent(x, y, (short)width, (short)height);
            }
        }
        return true;

    case WM_MOUSEWHEEL:
        {
            short delta = (short)HIWORD(wParam);
            LiSendHighResScrollEvent(delta);
        }
        return true;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        {
            // Shortcut: Ctrl + Alt + Shift + Q to quit stream
            if ((wParam == 'Q' || wParam == 'q') &&
                (GetKeyState(VK_CONTROL) & 0x8000) &&
                (GetKeyState(VK_MENU) & 0x8000) &&
                (GetKeyState(VK_SHIFT) & 0x8000)) {
                g_VideoContext.quit_requested = true;
                PostMessage(hwnd, WM_CLOSE, 0, 0);
                return true;
            }

            // Shortcut: Ctrl + Alt + Shift + Z to toggle mouse capture
            if ((wParam == 'Z' || wParam == 'z') &&
                (GetKeyState(VK_CONTROL) & 0x8000) &&
                (GetKeyState(VK_MENU) & 0x8000) &&
                (GetKeyState(VK_SHIFT) & 0x8000)) {
                input_handler_set_capture(!g_MouseCaptured);
                return true;
            }

            short vkCode = (short)wParam;
            if (vkCode >= 0 && vkCode < 256) {
                g_KeysDown[vkCode] = true;
            }
            char mod = get_active_modifiers();
            LiSendKeyboardEvent(vkCode, KEY_ACTION_DOWN, mod);
        }
        return true;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        {
            short vkCode = (short)wParam;
            if (vkCode >= 0 && vkCode < 256) {
                g_KeysDown[vkCode] = false;
            }
            char mod = get_active_modifiers();
            LiSendKeyboardEvent(vkCode, KEY_ACTION_UP, mod);
        }
        return true;

    case WM_KILLFOCUS:
        if (g_MouseCaptured) {
            input_handler_set_capture(false);
        }
        for (int k = 0; k < 256; k++) {
            if (g_KeysDown[k]) {
                LiSendKeyboardEvent((short)k, KEY_ACTION_UP, 0);
                g_KeysDown[k] = false;
            }
        }
        LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
        LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
        LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_MIDDLE);
        return true;
    }

    return false;
}

void input_handler_poll_gamepads(void) {
    if (!pfnXInputGetState) return;

    for (DWORD i = 0; i < 4; i++) {
        XINPUT_STATE state;
        if (pfnXInputGetState(i, &state) == ERROR_SUCCESS) {
            short buttonFlags = 0;
            WORD wButtons = state.Gamepad.wButtons;

            if (wButtons & XINPUT_GAMEPAD_DPAD_UP) buttonFlags |= 0x0001;
            if (wButtons & XINPUT_GAMEPAD_DPAD_DOWN) buttonFlags |= 0x0002;
            if (wButtons & XINPUT_GAMEPAD_DPAD_LEFT) buttonFlags |= 0x0004;
            if (wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) buttonFlags |= 0x0008;
            if (wButtons & XINPUT_GAMEPAD_START) buttonFlags |= 0x0010;
            if (wButtons & XINPUT_GAMEPAD_BACK) buttonFlags |= 0x0020;
            if (wButtons & XINPUT_GAMEPAD_LEFT_THUMB) buttonFlags |= 0x0040;
            if (wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) buttonFlags |= 0x0080;
            if (wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) buttonFlags |= 0x0100;
            if (wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) buttonFlags |= 0x0200;
            if (wButtons & XINPUT_GAMEPAD_A) buttonFlags |= 0x1000;
            if (wButtons & XINPUT_GAMEPAD_B) buttonFlags |= 0x2000;
            if (wButtons & XINPUT_GAMEPAD_X) buttonFlags |= 0x4000;
            if (wButtons & XINPUT_GAMEPAD_Y) buttonFlags |= 0x8000;

            LiSendControllerEvent(
                buttonFlags,
                (unsigned char)state.Gamepad.bLeftTrigger,
                (unsigned char)state.Gamepad.bRightTrigger,
                state.Gamepad.sThumbLX,
                state.Gamepad.sThumbLY,
                state.Gamepad.sThumbRX,
                state.Gamepad.sThumbRY
            );
        }
    }
}
