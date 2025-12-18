#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>

#include <SDL.h>
#include <SDL_image.h>
#include <SDL_opengl.h>

#include "gui.h"
#include "font_win32.h"

#ifdef _MSC_VER
#pragma comment(lib, "opengl32.lib")
#endif

static void clamp_and_center(int* w, int* h, int* x, int* y) {
    SDL_Rect usable = { 0,0,0,0 };
    if (SDL_GetDisplayUsableBounds(0, &usable) == 0) {
        int maxW = usable.w > 80 ? usable.w - 40 : usable.w;
        int maxH = usable.h > 120 ? usable.h - 80 : usable.h;
        if (maxW > 0 && *w > maxW) *w = maxW;
        if (maxH > 0 && *h > maxH) *h = maxH;
    }
    if (*x == 0 && *y == 0) {
        *x = SDL_WINDOWPOS_CENTERED;
        *y = SDL_WINDOWPOS_CENTERED;
    }
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    
    int img_flags = IMG_INIT_PNG;
    if ((IMG_Init(img_flags) & img_flags) != img_flags) {
        fprintf(stderr, "SDL_image initialization failed: %s\n", IMG_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    int w = 1258, h = 983;
    int x = 0, y = 0;
    clamp_and_center(&w, &h, &x, &y);

    SDL_Window* win = SDL_CreateWindow(
        "3Ddraw (GUI repro)",
        x, y, w, h,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE
    );
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetSwapInterval(1);

    /* Init font (Helvetica 12; fallback to Arial) */
    FontWin32* font = font_create_helvetica_12(win);

    GuiState* gui = gui_create();
    gui_set_font(gui, font);
    gui_load_tool_icons(gui, "resources");

    int running = 1;
    int mouse_down = 0;
    int mx = 0, my = 0;

    while (running) {
        int pressed = 0, released = 0;
        int wheel_delta = 0;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            if (e.type == SDL_MOUSEMOTION) { mx = e.motion.x; my = e.motion.y; }
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) { mouse_down = 1; pressed = 1; }
            if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) { mouse_down = 0; released = 1; }
            if (e.type == SDL_MOUSEWHEEL) {
                /* SDL_MOUSEWHEEL: positive y = scroll up (zoom in), negative y = scroll down (zoom out) */
                wheel_delta = e.wheel.y;
            }
            if (e.type == SDL_WINDOWEVENT && (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_RESIZED)) {
                /* nothing special; we query each frame */
            }
        }

        SDL_GetWindowSize(win, &w, &h);

        GuiInput in = { 0 };
        in.mouse_x = mx;
        in.mouse_y = my;
        in.mouse_down = mouse_down;
        in.mouse_pressed = pressed;
        in.mouse_released = released;
        in.wheel_delta = wheel_delta;

        gui_update(gui, &in, w, h);
        gui_draw(gui, w, h);

        SDL_GL_SwapWindow(win);
        SDL_Delay(1);
    }

    gui_destroy(gui);
    font_destroy(font);

    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    IMG_Quit();
    SDL_Quit();
    return 0;
}


