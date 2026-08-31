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
static bool g_SyntheticRecenter = false;
static bool g_KeysDown[256] = {0};

typedef DWORD (WINAPI *LPFN_XINPUTGETSTATE)(DWORD dwUserIndex, XINPUT_STATE* pState);
static LPFN_XINPUTGETSTATE pfnXInputGetState = NULL;
static HMODULE g_hXInput = NULL;

void input_handler_init(HWND stream_hwnd) {
    g_StreamHwnd = stream_hwnd;
    memset(g_KeysDown, 0, sizeof(g_KeysDown));
    g_SyntheticRecenter = false;

    // Load XInput dynamically (supports XP with DirectX End-User Runtimes)
    g_hXInput = LoadLibraryA("xinput1_3.dll");
    if (!g_hXInput) g_hXInput = LoadLibraryA("xinput9_1_0.dll");
    if (!g_hXInput) g_hXInput = LoadLibraryA("xinput1_4.dll");

    if (g_hXInput) {
        pfnXInputGetState = (LPFN_XINPUTGETSTATE)GetProcAddress(g_hXInput, "XInputGetState");
    }
}

void input_handler_release_all_keys(void) {
    for (int k = 0; k < 256; k++) {
        if (g_KeysDown[k]) {
            LiSendKeyboardEvent((short)k, KEY_ACTION_UP, 0);
            g_KeysDown[k] = false;
        }
    }
    LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
    LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
    LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_MIDDLE);
}

void input_handler_destroy(void) {
    input_handler_set_capture(false);
    input_handler_release_all_keys();
    if (g_hXInput) {
        FreeLibrary(g_hXInput);
        g_hXInput = NULL;
        pfnXInputGetState = NULL;
    }
}

void input_handler_set_capture(bool capture) {
    g_MouseCaptured = capture;
    if (capture && g_StreamHwnd) {
        SetFocus(g_StreamHwnd);
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
        input_handler_release_all_keys();
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
    case WM_SETFOCUS:
        SetFocus(hwnd);
        return true;

    case WM_LBUTTONDOWN:
        {
            SetFocus(hwnd);
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
            SetFocus(hwnd);
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
        SetFocus(hwnd);
        if (!g_MouseCaptured) input_handler_set_capture(true);
        LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_MIDDLE);
        return true;

    case WM_MBUTTONUP:
        LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_MIDDLE);
        return true;

    case WM_XBUTTONDOWN:
        SetFocus(hwnd);
        if (HIWORD(wParam) == XBUTTON1) LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_X1);
        else if (HIWORD(wParam) == XBUTTON2) LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_X2);
        return true;

    case WM_XBUTTONUP:
        if (HIWORD(wParam) == XBUTTON1) LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_X1);
        else if (HIWORD(wParam) == XBUTTON2) LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_X2);
        return true;

    case WM_MOUSEMOVE:
        {
            if (g_SyntheticRecenter) {
                g_SyntheticRecenter = false;
                return true;
            }

            POINT pt;
            GetCursorPos(&pt);

            if (g_MouseCaptured) {
                short dx = (short)(pt.x - g_LastCursorPos.x);
                short dy = (short)(pt.y - g_LastCursorPos.y);
                g_LastCursorPos = pt;

                if (dx != 0 || dy != 0) {
                    LiSendMouseMoveEvent(dx, dy);
                }

                RECT rc;
                GetClientRect(hwnd, &rc);
                POINT center = { (rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2 };
                ClientToScreen(hwnd, &center);

                // Recenter cursor when drifting from window center
                if (abs(pt.x - center.x) > 50 || abs(pt.y - center.y) > 50) {
                    g_SyntheticRecenter = true;
                    g_LastCursorPos = center;
                    SetCursorPos(center.x, center.y);
                }
            } else {
                g_LastCursorPos = pt;
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
            // Drop auto-repeated key events (bit 30 of lParam is 1 if key was already down)
            if (lParam & (1 << 30)) {
                return true;
            }

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

            short vk = (short)(wParam & 0xFF);
            g_KeysDown[vk] = true;

            char mod = get_active_modifiers();
            LiSendKeyboardEvent(vk, KEY_ACTION_DOWN, mod);
        }
        return true;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        {
            short vk = (short)(wParam & 0xFF);
            g_KeysDown[vk] = false;

            char mod = get_active_modifiers();
            LiSendKeyboardEvent(vk, KEY_ACTION_UP, mod);
        }
        return true;

    case WM_KILLFOCUS:
        if (g_MouseCaptured) {
            input_handler_set_capture(false);
        } else {
            input_handler_release_all_keys();
        }
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
                state.Gamepad.bLeftTrigger,
                state.Gamepad.bRightTrigger,
                state.Gamepad.sThumbLX,
                state.Gamepad.sThumbLY,
                state.Gamepad.sThumbRX,
                state.Gamepad.sThumbRY);
        }
    }
}
