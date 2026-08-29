#pragma once

#include <stdint.h>

typedef struct FontWin32 FontWin32;

typedef struct GuiInput {
    int mouse_x;
    int mouse_y;
    int mouse_down;      /* current (left button) */
    int mouse_pressed;   /* went down this frame (left button) */
    int mouse_released;  /* went up this frame (left button) */
    int mouse_right_down;      /* current (right button) */
    int mouse_right_pressed;   /* went down this frame (right button) */
    int mouse_right_released;  /* went up this frame (right button) */
    int mouse_right_dragged;   /* exceeded the click threshold since press */
    int mouse_right_gesture_x; /* current/release position even on a routed press */
    int mouse_right_gesture_y;
    int mouse_middle_down;
    int mouse_middle_pressed;
    int mouse_middle_released;
    int wheel_delta;     /* mouse wheel scroll delta (positive = zoom in, negative = zoom out) */
    unsigned modifiers;  /* GUI_MOD_* state for pointer gestures */
} GuiInput;

enum {
    GUI_KEY_ESCAPE = 0x100,
    GUI_KEY_DELETE,
    GUI_KEY_BACKSPACE,
    GUI_KEY_ENTER
};

enum {
    GUI_MOD_CTRL = 1u << 0,
    GUI_MOD_SHIFT = 1u << 1,
    GUI_MOD_ALT = 1u << 2
};

typedef struct GuiState GuiState;

typedef enum GuiCommand {
    GUI_COMMAND_NONE = 0,
    GUI_COMMAND_QUIT
} GuiCommand;

GuiState* gui_create(void);
void gui_destroy(GuiState* g);
GuiCommand gui_take_command(GuiState* g);
int gui_request_quit(GuiState* g);
/* Releases every editor-owned pointer/text capture.  Active transactional
   gestures are rolled back, making this safe for native window focus loss. */
void gui_cancel_input(GuiState* g);
void gui_handle_key(GuiState* g, int key, unsigned modifiers, int pressed);
const char* gui_window_title(GuiState* g);

void gui_set_font(GuiState* g, FontWin32* font);
void gui_load_tool_icons(GuiState* g, const char* resource_path);
void gui_load_anim_icons(GuiState* g, const char* resource_path);
void gui_update(GuiState* g, const GuiInput* in, int win_w, int win_h);
void gui_draw(GuiState* g, const GuiInput* in, int win_w, int win_h, int fb_w, int fb_h);


