#define SDL_MAIN_USE_CALLBACKS 1

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_opengl.h>

#include <stdbool.h>
#include <string.h>

#include "font_win32.h"
#include "gui.h"

#define APP_NAME "3DCad"
#define APP_VERSION "0.2.0"
#define APP_IDENTIFIER "io.github.wowjinxy.3dcad"
#define DEFAULT_WINDOW_WIDTH 1258
#define DEFAULT_WINDOW_HEIGHT 983
#define DEFAULT_FONT_SIZE 12

typedef struct AppState {
    SDL_Window* window;
    SDL_GLContext gl_context;
    GuiState* gui;
    FontWin32* font;
    GuiInput input;
    float pending_wheel;
    bool vsync_enabled;
    bool font_refresh_pending;
    char current_title[1024];
} AppState;

static SDL_AppResult fail_sdl(const char* operation) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s failed: %s", operation, SDL_GetError());
    return SDL_APP_FAILURE;
}

static bool set_gl_attribute(SDL_GLAttr attribute, int value, const char* name) {
    if (SDL_GL_SetAttribute(attribute, value)) {
        return true;
    }

    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                 "Unable to set OpenGL attribute %s: %s",
                 name,
                 SDL_GetError());
    return false;
}

static bool configure_gl(void) {
    return set_gl_attribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1, "context major version") &&
           set_gl_attribute(SDL_GL_CONTEXT_MINOR_VERSION, 1, "context minor version") &&
           set_gl_attribute(SDL_GL_DOUBLEBUFFER, 1, "double buffering") &&
           set_gl_attribute(SDL_GL_DEPTH_SIZE, 24, "depth buffer size") &&
           set_gl_attribute(SDL_GL_STENCIL_SIZE, 8, "stencil buffer size");
}

static void clamp_initial_window_size(int* width, int* height) {
    SDL_Rect usable_bounds;
    SDL_DisplayID display = SDL_GetPrimaryDisplay();

    if (display == 0 || !SDL_GetDisplayUsableBounds(display, &usable_bounds)) {
        return;
    }

    /* Leave a small margin around the initial window on compact displays. */
    const int max_width = usable_bounds.w > 80 ? usable_bounds.w - 40 : usable_bounds.w;
    const int max_height = usable_bounds.h > 120 ? usable_bounds.h - 80 : usable_bounds.h;

    if (max_width > 0 && *width > max_width) {
        *width = max_width;
    }
    if (max_height > 0 && *height > max_height) {
        *height = max_height;
    }
}

static float get_window_pixel_density(SDL_Window* window) {
    const float pixel_density = SDL_GetWindowPixelDensity(window);
    if (pixel_density > 0.0f) {
        return pixel_density;
    }

    int window_width = 0;
    int pixel_width = 0;
    if (SDL_GetWindowSize(window, &window_width, NULL) &&
        SDL_GetWindowSizeInPixels(window, &pixel_width, NULL) &&
        window_width > 0 && pixel_width > 0) {
        return (float)pixel_width / (float)window_width;
    }

    return 1.0f;
}

static float get_window_display_scale(SDL_Window* window) {
    const float display_scale = SDL_GetWindowDisplayScale(window);
    return display_scale > 0.0f ? display_scale : 1.0f;
}

static bool set_initial_window_size(SDL_Window* window) {
    const float pixel_density = get_window_pixel_density(window);
    const float display_scale = get_window_display_scale(window);
    int width = (int)((float)DEFAULT_WINDOW_WIDTH * display_scale / pixel_density + 0.5f);
    int height = (int)((float)DEFAULT_WINDOW_HEIGHT * display_scale / pixel_density + 0.5f);

    clamp_initial_window_size(&width, &height);
    return SDL_SetWindowSize(window, width, height);
}

static void refresh_font(AppState* app) {
    if (!app || !app->window || !app->gui) {
        return;
    }

    const float display_scale = get_window_display_scale(app->window);
    FontWin32* replacement = font_create_helvetica(DEFAULT_FONT_SIZE, display_scale);
    if (!replacement) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to create the UI font; text rendering will be unavailable");
        return;
    }

    gui_set_font(app->gui, replacement);
    font_destroy(app->font);
    app->font = replacement;
}

static void load_resources(AppState* app) {
    char resource_path[1024];
    const char* base_path = SDL_GetBasePath();
    const int path_length = base_path
        ? SDL_snprintf(resource_path, sizeof(resource_path), "%sresources", base_path)
        : -1;

    if (path_length > 0 && path_length < (int)sizeof(resource_path)) {
        gui_load_tool_icons(app->gui, resource_path);
        gui_load_anim_icons(app->gui, resource_path);
        return;
    }

    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Unable to resolve the executable path; loading resources from the working directory");
    gui_load_tool_icons(app->gui, "resources");
    gui_load_anim_icons(app->gui, "resources");
}

static void verify_depth_buffer(void) {
    GLint depth_bits = 0;
    glGetIntegerv(GL_DEPTH_BITS, &depth_bits);
    if (depth_bits == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "The OpenGL context has no depth buffer; 3D depth testing will be disabled");
    }
}

static void clear_transient_input(GuiInput* input) {
    input->mouse_pressed = 0;
    input->mouse_released = 0;
    input->mouse_right_pressed = 0;
    input->mouse_right_released = 0;
    input->mouse_middle_pressed = 0;
    input->mouse_middle_released = 0;
    input->wheel_delta = 0;
}

static unsigned gui_modifiers(SDL_Keymod modifiers) {
    unsigned result = 0;
    if (modifiers & SDL_KMOD_CTRL) result |= GUI_MOD_CTRL;
    if (modifiers & SDL_KMOD_SHIFT) result |= GUI_MOD_SHIFT;
    if (modifiers & SDL_KMOD_ALT) result |= GUI_MOD_ALT;
    return result;
}

static int gui_keycode(SDL_Keycode key) {
    switch (key) {
    case SDLK_ESCAPE: return GUI_KEY_ESCAPE;
    case SDLK_DELETE: return GUI_KEY_DELETE;
    case SDLK_BACKSPACE: return GUI_KEY_BACKSPACE;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return GUI_KEY_ENTER;
    case SDLK_KP_0: return '0';
    case SDLK_KP_1: return '1';
    case SDLK_KP_2: return '2';
    case SDLK_KP_3: return '3';
    case SDLK_KP_4: return '4';
    case SDLK_KP_5: return '5';
    case SDLK_KP_6: return '6';
    case SDLK_KP_7: return '7';
    case SDLK_KP_8: return '8';
    case SDLK_KP_9: return '9';
    case SDLK_KP_PERIOD:
    case SDLK_KP_DECIMAL: return '.';
    case SDLK_KP_PLUS: return '+';
    case SDLK_KP_MINUS: return '-';
    case SDLK_KP_MULTIPLY: return '*';
    case SDLK_KP_DIVIDE: return '/';
    default:
        if (key >= 0 && key <= 0x7f) return (int)key;
        return 0;
    }
}

static void refresh_window_title(AppState* app) {
    const char* title;
    if (!app || !app->window || !app->gui) return;
    title = gui_window_title(app->gui);
    if (!title || !*title) title = APP_NAME;
    if (strcmp(title, app->current_title) != 0) {
        SDL_strlcpy(app->current_title, title, sizeof(app->current_title));
        if (!SDL_SetWindowTitle(app->window, app->current_title)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Unable to update the document title: %s", SDL_GetError());
        }
    }
}

SDL_AppResult SDLCALL SDL_AppInit(void** appstate, int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    AppState* app = (AppState*)SDL_calloc(1, sizeof(*app));
    if (!app) {
        return fail_sdl("application state allocation");
    }
    *appstate = app;

    if (!SDL_SetAppMetadata(APP_NAME, APP_VERSION, APP_IDENTIFIER)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to set application metadata: %s",
                    SDL_GetError());
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return fail_sdl("SDL initialization");
    }
    if (!configure_gl()) {
        return SDL_APP_FAILURE;
    }

    const SDL_WindowFlags flags = SDL_WINDOW_OPENGL |
                                  SDL_WINDOW_RESIZABLE |
                                  SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                  SDL_WINDOW_HIDDEN;
    app->window = SDL_CreateWindow(APP_NAME,
                                   DEFAULT_WINDOW_WIDTH,
                                   DEFAULT_WINDOW_HEIGHT,
                                   flags);
    if (!app->window) {
        return fail_sdl("window creation");
    }

    if (!set_initial_window_size(app->window)) {
        return fail_sdl("setting the initial window size");
    }
    if (!SDL_SetWindowPosition(app->window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to center the window: %s",
                    SDL_GetError());
    }

    app->gl_context = SDL_GL_CreateContext(app->window);
    if (!app->gl_context) {
        return fail_sdl("OpenGL context creation");
    }
    if (!SDL_GL_MakeCurrent(app->window, app->gl_context)) {
        return fail_sdl("making the OpenGL context current");
    }

    app->vsync_enabled = SDL_GL_SetSwapInterval(1);
    if (!app->vsync_enabled) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to enable vertical sync: %s",
                    SDL_GetError());
    }

    verify_depth_buffer();

    app->gui = gui_create();
    if (!app->gui) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Unable to allocate GUI state");
        return SDL_APP_FAILURE;
    }

    refresh_font(app);
    load_resources(app);

    if (!SDL_ShowWindow(app->window)) {
        return fail_sdl("showing the window");
    }

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDLCALL SDL_AppEvent(void* appstate, SDL_Event* event) {
    AppState* app = (AppState*)appstate;
    if (!app || !event) {
        return SDL_APP_FAILURE;
    }

    switch (event->type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        return gui_request_quit(app->gui) ? SDL_APP_SUCCESS : SDL_APP_CONTINUE;

    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        if (event->type == SDL_EVENT_KEY_DOWN && event->key.repeat) break;
        int key = gui_keycode(event->key.key);
        if (key) {
            gui_handle_key(app->gui, key, gui_modifiers(event->key.mod),
                           event->type == SDL_EVENT_KEY_DOWN);
        }
        break;
    }

    case SDL_EVENT_MOUSE_MOTION:
        app->input.mouse_x = (int)event->motion.x;
        app->input.mouse_y = (int)event->motion.y;
        break;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        app->input.mouse_x = (int)event->button.x;
        app->input.mouse_y = (int)event->button.y;
        if (event->button.button == SDL_BUTTON_LEFT) {
            app->input.mouse_down = 1;
            app->input.mouse_pressed = 1;
        } else if (event->button.button == SDL_BUTTON_RIGHT) {
            app->input.mouse_right_down = 1;
            app->input.mouse_right_pressed = 1;
        } else if (event->button.button == SDL_BUTTON_MIDDLE) {
            app->input.mouse_middle_down = 1;
            app->input.mouse_middle_pressed = 1;
        }
        break;

    case SDL_EVENT_MOUSE_BUTTON_UP:
        app->input.mouse_x = (int)event->button.x;
        app->input.mouse_y = (int)event->button.y;
        if (event->button.button == SDL_BUTTON_LEFT) {
            app->input.mouse_down = 0;
            app->input.mouse_released = 1;
        } else if (event->button.button == SDL_BUTTON_RIGHT) {
            app->input.mouse_right_down = 0;
            app->input.mouse_right_released = 1;
        } else if (event->button.button == SDL_BUTTON_MIDDLE) {
            app->input.mouse_middle_down = 0;
            app->input.mouse_middle_released = 1;
        }
        break;

    case SDL_EVENT_MOUSE_WHEEL: {
        float wheel_y = event->wheel.y;
        if (event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
            wheel_y = -wheel_y;
        }
        app->pending_wheel += wheel_y;
        break;
    }

    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        app->font_refresh_pending = true;
        break;

    default:
        break;
    }

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDLCALL SDL_AppIterate(void* appstate) {
    AppState* app = (AppState*)appstate;
    if (!app || !app->window || !app->gui) {
        return SDL_APP_FAILURE;
    }

    if (app->font_refresh_pending) {
        refresh_font(app);
        app->font_refresh_pending = false;
    }

    float mouse_x = 0.0f;
    float mouse_y = 0.0f;
    const SDL_MouseButtonFlags mouse_buttons = SDL_GetMouseState(&mouse_x, &mouse_y);
    const bool left_down = (mouse_buttons & SDL_BUTTON_LMASK) != 0;
    const bool right_down = (mouse_buttons & SDL_BUTTON_RMASK) != 0;
    const bool middle_down = (mouse_buttons & SDL_BUTTON_MMASK) != 0;

    int window_width = 0;
    int window_height = 0;
    int pixel_width = 0;
    int pixel_height = 0;
    if (!SDL_GetWindowSize(app->window, &window_width, &window_height)) {
        return fail_sdl("querying the window size");
    }
    if (!SDL_GetWindowSizeInPixels(app->window, &pixel_width, &pixel_height)) {
        return fail_sdl("querying the window pixel size");
    }

    if (window_width <= 0 || window_height <= 0 || pixel_width <= 0 || pixel_height <= 0 ||
        (SDL_GetWindowFlags(app->window) & SDL_WINDOW_MINIMIZED) != 0) {
        clear_transient_input(&app->input);
        SDL_Delay(10);
        return SDL_APP_CONTINUE;
    }

    const float display_scale = get_window_display_scale(app->window);
    const int gui_width = (int)((float)pixel_width / display_scale + 0.5f);
    const int gui_height = (int)((float)pixel_height / display_scale + 0.5f);
    const float mouse_scale_x = (float)gui_width / (float)window_width;
    const float mouse_scale_y = (float)gui_height / (float)window_height;

    app->input.mouse_x = (int)(mouse_x * mouse_scale_x);
    app->input.mouse_y = (int)(mouse_y * mouse_scale_y);
    if (left_down != (app->input.mouse_down != 0)) {
        app->input.mouse_pressed |= left_down;
        app->input.mouse_released |= !left_down;
        app->input.mouse_down = left_down;
    }
    if (right_down != (app->input.mouse_right_down != 0)) {
        app->input.mouse_right_pressed |= right_down;
        app->input.mouse_right_released |= !right_down;
        app->input.mouse_right_down = right_down;
    }
    if (middle_down != (app->input.mouse_middle_down != 0)) {
        app->input.mouse_middle_pressed |= middle_down;
        app->input.mouse_middle_released |= !middle_down;
        app->input.mouse_middle_down = middle_down;
    }
    app->input.modifiers = gui_modifiers(SDL_GetModState());

    /* Keep sub-step touchpad deltas until they add up to a complete GUI step. */
    app->input.wheel_delta = (int)app->pending_wheel;
    app->pending_wheel -= (float)app->input.wheel_delta;

    gui_update(app->gui, &app->input, gui_width, gui_height);
    refresh_window_title(app);
    if (gui_take_command(app->gui) == GUI_COMMAND_QUIT) {
        return SDL_APP_SUCCESS;
    }
    gui_draw(app->gui,
             &app->input,
             gui_width,
             gui_height,
             pixel_width,
             pixel_height);

    if (!SDL_GL_SwapWindow(app->window)) {
        return fail_sdl("swapping the OpenGL window");
    }

    clear_transient_input(&app->input);
    if (!app->vsync_enabled) {
        SDL_Delay(1);
    }
    return SDL_APP_CONTINUE;
}

void SDLCALL SDL_AppQuit(void* appstate, SDL_AppResult result) {
    (void)result;

    AppState* app = (AppState*)appstate;
    if (!app) {
        return;
    }

    if (app->window && app->gl_context) {
        SDL_GL_MakeCurrent(app->window, app->gl_context);
    }

    gui_destroy(app->gui);
    font_destroy(app->font);

    if (app->gl_context) {
        SDL_GL_DestroyContext(app->gl_context);
    }
    if (app->window) {
        SDL_DestroyWindow(app->window);
    }

    SDL_free(app);
}
