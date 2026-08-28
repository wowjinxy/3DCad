#pragma once

#include <stdint.h>

typedef struct FontWin32 FontWin32;

/* Creates a bitmap font in the current OpenGL context using
   wglUseFontBitmaps. Helvetica falls back to Arial when unavailable.
   display_scale converts GDI's physical-pixel metrics to GUI coordinates. */
FontWin32* font_create_helvetica(int point_size, float display_scale);
void font_destroy(FontWin32* f);

int font_height(const FontWin32* f);
int font_measure(const FontWin32* f, const char* text);
void font_draw(const FontWin32* f, int x, int y, const char* text, uint8_t gray);



