#define _CRT_SECURE_NO_WARNINGS

#include "font_win32.h"

#include <SDL3/SDL_opengl.h>

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wingdi.h>
#endif

struct FontWin32 {
    GLuint base;
    int ascent;
    int height;
    int widths[96]; /* ASCII 32..127 */
};

#ifdef _WIN32

static int to_logical_pixels(int physical_pixels, float display_scale) {
    if (display_scale <= 0.0f) {
        display_scale = 1.0f;
    }

    const int logical_pixels = (int)((float)physical_pixels / display_scale + 0.5f);
    return logical_pixels > 0 ? logical_pixels : 1;
}

static void get_widths(HDC hdc, int* out_widths, int fallback, float display_scale) {
    int physical_widths[96] = {0};
    if (!GetCharWidth32A(hdc, 32, 127, physical_widths)) {
        for (int i = 0; i < 96; ++i) {
            out_widths[i] = to_logical_pixels(fallback, display_scale);
        }
        return;
    }

    for (int i = 0; i < 96; ++i) {
        out_widths[i] = to_logical_pixels(physical_widths[i], display_scale);
    }
}

static HFONT create_font(const char* face, int point_size, float display_scale) {
    if (display_scale <= 0.0f) {
        display_scale = 1.0f;
    }

    /* GDI's screen DC reports the system DPI on many per-monitor-aware setups.
       SDL's window display scale tracks the monitor that actually owns the
       window, so use it for both bitmap size and logical metric conversion. */
    const float pixel_height = (float)point_size * (96.0f / 72.0f) * display_scale;
    const int height = -(int)(pixel_height + 0.5f);
    return CreateFontA(height,
                       0,
                       0,
                       0,
                       FW_NORMAL,
                       FALSE,
                       FALSE,
                       FALSE,
                       ANSI_CHARSET,
                       OUT_TT_PRECIS,
                       CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY,
                       FF_DONTCARE,
                       face);
}

#endif

FontWin32* font_create_helvetica(int point_size, float display_scale) {
#ifndef _WIN32
    (void)point_size;
    (void)display_scale;
    return NULL;
#else
    if (point_size <= 0) {
        return NULL;
    }

    /* SDL owns the window and context. WGL exposes the current context's DC,
       so the font layer does not need a window-system handle. */
    HDC hdc = wglGetCurrentDC();
    if (!hdc) {
        return NULL;
    }

    HFONT font = create_font("Helvetica", point_size, display_scale);
    if (!font) {
        font = create_font("Arial", point_size, display_scale);
    }
    if (!font) {
        return NULL;
    }

    HFONT old_font = (HFONT)SelectObject(hdc, font);
    TEXTMETRICA metrics;
    memset(&metrics, 0, sizeof(metrics));
    if (!GetTextMetricsA(hdc, &metrics)) {
        SelectObject(hdc, old_font);
        DeleteObject(font);
        return NULL;
    }

    FontWin32* result = (FontWin32*)calloc(1, sizeof(*result));
    if (!result) {
        SelectObject(hdc, old_font);
        DeleteObject(font);
        return NULL;
    }

    result->base = glGenLists(96);
    result->ascent = to_logical_pixels((int)metrics.tmAscent, display_scale);
    result->height = to_logical_pixels((int)(metrics.tmHeight + metrics.tmExternalLeading),
                                       display_scale);
    get_widths(hdc, result->widths, (int)metrics.tmAveCharWidth, display_scale);

    if (result->base == 0 || !wglUseFontBitmapsA(hdc, 32, 96, result->base)) {
        if (result->base != 0) {
            glDeleteLists(result->base, 96);
        }
        free(result);
        result = NULL;
    }

    SelectObject(hdc, old_font);
    DeleteObject(font);
    return result;
#endif
}

void font_destroy(FontWin32* font) {
    if (!font) {
        return;
    }
    if (font->base != 0) {
        glDeleteLists(font->base, 96);
    }
    free(font);
}

int font_height(const FontWin32* font) {
    return (font && font->height > 0) ? font->height : 16;
}

int font_measure(const FontWin32* font, const char* text) {
    if (!text) {
        return 0;
    }
    if (!font) {
        return (int)strlen(text) * 8;
    }

    int width = 0;
    for (const unsigned char* cursor = (const unsigned char*)text; *cursor; ++cursor) {
        unsigned int character = *cursor;
        if (character < 32 || character > 127) {
            character = '?';
        }
        width += font->widths[character - 32];
    }
    return width;
}

void font_draw(const FontWin32* font, int x, int y, const char* text, uint8_t gray) {
    if (!font || !text) {
        return;
    }

    const float value = (float)gray / 255.0f;
    glColor3f(value, value, value);
    glRasterPos2i(x, y + font->ascent);
    glListBase(font->base - 32);
    glCallLists((GLsizei)strlen(text), GL_UNSIGNED_BYTE, (const GLubyte*)text);
}
