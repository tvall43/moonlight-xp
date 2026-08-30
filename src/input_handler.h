#ifndef INPUT_HANDLER_H
#define INPUT_HANDLER_H

#include <windows.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void input_handler_init(HWND stream_hwnd);
void input_handler_destroy(void);

// Process Win32 window messages for input
bool input_handler_process_message(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

// Poll gamepads (called regularly from render / stream loop)
void input_handler_poll_gamepads(void);

// Enable / disable cursor clipping and relative capture
void input_handler_set_capture(bool capture);

#ifdef __cplusplus
}
#endif

#endif // INPUT_HANDLER_H
