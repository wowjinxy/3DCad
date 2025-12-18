#pragma once

#include <stdint.h>

typedef struct FontWin32 FontWin32;

typedef struct GuiInput {
    int mouse_x;
    int mouse_y;
    int mouse_down;      /* current */
    int mouse_pressed;   /* went down this frame */
    int mouse_released;  /* went up this frame */
    int wheel_delta;     /* mouse wheel scroll delta (positive = zoom in, negative = zoom out) */
} GuiInput;

typedef struct GuiState GuiState;

GuiState* gui_create(void);
void gui_destroy(GuiState* g);

void gui_set_font(GuiState* g, FontWin32* font);
void gui_load_tool_icons(GuiState* g, const char* resource_path);
void gui_update(GuiState* g, const GuiInput* in, int win_w, int win_h);
void gui_draw(GuiState* g, int win_w, int win_h);


