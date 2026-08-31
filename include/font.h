#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Font Font;

/* Creates the editor's dependency-free 8x16 logical-pixel bitmap font.
   point_size is accepted for API compatibility; the embedded face has one
   fixed logical size. display_scale controls only its framebuffer density. */
Font* font_create_default(int point_size, float display_scale);
void font_destroy(Font* font);

int font_height(const Font* font);
int font_measure(const Font* font, const char* text);
void font_draw(const Font* font, int x, int y, const char* text, uint8_t gray);

#ifdef __cplusplus
}
#endif
