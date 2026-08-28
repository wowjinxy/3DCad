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
    int wheel_delta;     /* mouse wheel scroll delta (positive = zoom in, negative = zoom out) */
} GuiInput;

typedef struct GuiState GuiState;

typedef enum GuiCommand {
    GUI_COMMAND_NONE = 0,
    GUI_COMMAND_QUIT
} GuiCommand;

GuiState* gui_create(void);
void gui_destroy(GuiState* g);
GuiCommand gui_take_command(GuiState* g);

void gui_set_font(GuiState* g, FontWin32* font);
void gui_load_tool_icons(GuiState* g, const char* resource_path);
void gui_load_anim_icons(GuiState* g, const char* resource_path);
void gui_update(GuiState* g, const GuiInput* in, int win_w, int win_h);
void gui_draw(GuiState* g, const GuiInput* in, int win_w, int win_h, int fb_w, int fb_h);


