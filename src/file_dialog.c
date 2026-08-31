#define _CRT_SECURE_NO_WARNINGS

#include "file_dialog.h"

#include <SDL3/SDL.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define FILE_DIALOG_MAX_FILTERS 8
#define FILE_DIALOG_PATTERN_CAPACITY 128

typedef struct FileDialogWaitState {
    SDL_Mutex* mutex;
    char* selected_path;
    char* error_message;
    int selected_filter;
    bool done;
} FileDialogWaitState;

static SDL_Window* dialog_parent;

void FileDialog_SetParent(SDL_Window* window) {
    dialog_parent = window;
}

static void SDLCALL file_dialog_callback(void* userdata,
                                         const char* const* filelist,
                                         int filter) {
    FileDialogWaitState* state = (FileDialogWaitState*)userdata;
    char* path = NULL;
    char* error = NULL;

    if (!state) return;
    if (!filelist) {
        const char* message = SDL_GetError();
        error = SDL_strdup(message && message[0]
                               ? message : "The native file dialog failed");
    } else if (filelist[0]) {
        path = SDL_strdup(filelist[0]);
        if (!path) error = SDL_strdup("Not enough memory to copy the selected path");
    }

    SDL_LockMutex(state->mutex);
    state->selected_path = path;
    state->error_message = error;
    state->selected_filter = filter;
    state->done = true;
    SDL_UnlockMutex(state->mutex);
}

static int convert_filter_pattern(const char* windows_pattern,
                                  char* pattern_out,
                                  size_t pattern_capacity) {
    const char* cursor;
    size_t written = 0;

    if (!windows_pattern || !pattern_out || pattern_capacity < 2) return 0;
    if (strcmp(windows_pattern, "*.*") == 0 ||
        strcmp(windows_pattern, "*") == 0) {
        pattern_out[0] = '*';
        pattern_out[1] = '\0';
        return 1;
    }

    cursor = windows_pattern;
    while (*cursor) {
        const char* end = strchr(cursor, ';');
        size_t length = end ? (size_t)(end - cursor) : strlen(cursor);

        if (length >= 2 && cursor[0] == '*' && cursor[1] == '.') {
            cursor += 2;
            length -= 2;
        } else if (length >= 1 && cursor[0] == '.') {
            cursor += 1;
            length -= 1;
        }
        if (!length || written + length + (written ? 1u : 0u) + 1u >
                           pattern_capacity)
            return 0;
        if (written) pattern_out[written++] = ';';
        memcpy(pattern_out + written, cursor, length);
        written += length;
        pattern_out[written] = '\0';

        if (!end) break;
        cursor = end + 1;
    }
    return written != 0;
}

static int parse_filters(const char* windows_filters,
                         SDL_DialogFileFilter* filters,
                         char patterns[][FILE_DIALOG_PATTERN_CAPACITY]) {
    const char* cursor = windows_filters;
    int count = 0;

    if (!cursor || !cursor[0]) {
        filters[0].name = "All files";
        filters[0].pattern = "*";
        return 1;
    }

    while (*cursor && count < FILE_DIALOG_MAX_FILTERS) {
        const char* name = cursor;
        const char* windows_pattern;
        cursor += strlen(cursor) + 1;
        if (!*cursor) break;
        windows_pattern = cursor;
        cursor += strlen(cursor) + 1;

        if (!convert_filter_pattern(windows_pattern, patterns[count],
                                    FILE_DIALOG_PATTERN_CAPACITY))
            return 0;
        filters[count].name = name;
        filters[count].pattern = patterns[count];
        ++count;
    }
    return count;
}

static int path_has_extension(const char* path) {
    const char* leaf;
    const char* slash;
    const char* backslash;
    if (!path) return 0;
    slash = strrchr(path, '/');
    backslash = strrchr(path, '\\');
    leaf = slash && backslash
               ? (slash > backslash ? slash + 1 : backslash + 1)
               : (slash ? slash + 1 : (backslash ? backslash + 1 : path));
    return strchr(leaf, '.') != NULL;
}

static int append_default_extension(char* path, size_t path_capacity,
                                    const char* pattern) {
    const char* end;
    size_t path_length;
    size_t extension_length;
    if (!path || !pattern || pattern[0] == '*' || path_has_extension(path))
        return 1;
    end = strchr(pattern, ';');
    extension_length = end ? (size_t)(end - pattern) : strlen(pattern);
    path_length = strlen(path);
    if (!extension_length ||
        path_length + 1u + extension_length + 1u > path_capacity)
        return 0;
    path[path_length++] = '.';
    memcpy(path + path_length, pattern, extension_length);
    path[path_length + extension_length] = '\0';
    return 1;
}

static int run_dialog(SDL_FileDialogType type,
                      char* path_out,
                      int path_out_size,
                      const char* windows_filters,
                      const char* title) {
    SDL_DialogFileFilter filters[FILE_DIALOG_MAX_FILTERS];
    char patterns[FILE_DIALOG_MAX_FILTERS][FILE_DIALOG_PATTERN_CAPACITY];
    FileDialogWaitState state;
    SDL_PropertiesID properties;
    int filter_count = 0;
    int result = 0;

    if (!path_out || path_out_size <= 1) return 0;
    path_out[0] = '\0';
    memset(&state, 0, sizeof(state));
    state.selected_filter = -1;
    state.mutex = SDL_CreateMutex();
    if (!state.mutex) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to create file-dialog synchronization: %s",
                    SDL_GetError());
        return 0;
    }

    if (type != SDL_FILEDIALOG_OPENFOLDER) {
        filter_count = parse_filters(windows_filters, filters, patterns);
        if (filter_count <= 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Unable to translate a file-dialog filter");
            SDL_DestroyMutex(state.mutex);
            return 0;
        }
    }

    properties = SDL_CreateProperties();
    if (!properties) {
        SDL_DestroyMutex(state.mutex);
        return 0;
    }
    if (dialog_parent)
        SDL_SetPointerProperty(properties,
                               SDL_PROP_FILE_DIALOG_WINDOW_POINTER,
                               dialog_parent);
    if (title && title[0])
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING,
                              title);
    if (filter_count > 0) {
        SDL_SetPointerProperty(properties,
                               SDL_PROP_FILE_DIALOG_FILTERS_POINTER, filters);
        SDL_SetNumberProperty(properties,
                              SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER,
                              filter_count);
    }

    SDL_ShowFileDialogWithProperties(type, file_dialog_callback, &state,
                                     properties);

    /* SDL's native dialogs are asynchronous and may complete on any thread.
       The editor's document transactions are deliberately modal, so keep the
       existing API while pumping SDL for portal-backed dialogs. PumpEvents
       does not remove queued application events. */
    for (;;) {
        bool done;
        SDL_LockMutex(state.mutex);
        done = state.done;
        SDL_UnlockMutex(state.mutex);
        if (done) break;
        SDL_PumpEvents();
        SDL_Delay(10);
    }

    SDL_LockMutex(state.mutex);
    if (state.error_message) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "File dialog failed: %s",
                    state.error_message);
    } else if (state.selected_path) {
        const size_t length = strlen(state.selected_path);
        if (length + 1u <= (size_t)path_out_size) {
            memcpy(path_out, state.selected_path, length + 1u);
            result = 1;
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Selected path is too long for the editor");
        }
    }
    if (result && type == SDL_FILEDIALOG_SAVEFILE && filter_count > 0) {
        int selected = state.selected_filter;
        if (selected < 0 || selected >= filter_count) selected = 0;
        if (!append_default_extension(path_out, (size_t)path_out_size,
                                      filters[selected].pattern)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Selected path is too long after adding its extension");
            path_out[0] = '\0';
            result = 0;
        }
    }
    SDL_UnlockMutex(state.mutex);

    SDL_free(state.selected_path);
    SDL_free(state.error_message);
    SDL_DestroyProperties(properties);
    SDL_DestroyMutex(state.mutex);
    return result;
}

int FileDialog_Open(char* filename_out, int filename_out_size,
                    const char* filter, const char* title) {
    return run_dialog(SDL_FILEDIALOG_OPENFILE, filename_out,
                      filename_out_size, filter,
                      title ? title : "Open File");
}

int FileDialog_Save(char* filename_out, int filename_out_size,
                    const char* filter, const char* title) {
    return run_dialog(SDL_FILEDIALOG_SAVEFILE, filename_out,
                      filename_out_size, filter,
                      title ? title : "Save File");
}

int FileDialog_OpenCAD(char* filename_out, int filename_out_size) {
    static const char filter[] =
        "CAD Files\0*.cad\0All Files\0*.*\0\0";
    return FileDialog_Open(filename_out, filename_out_size, filter,
                           "Open CAD File");
}

int FileDialog_SaveCAD(char* filename_out, int filename_out_size) {
    static const char filter[] =
        "CAD Files\0*.cad\0All Files\0*.*\0\0";
    return FileDialog_Save(filename_out, filename_out_size, filter,
                           "Save CAD File");
}

int FileDialog_SelectFolder(char* folder_path_out, int folder_path_out_size) {
    return run_dialog(SDL_FILEDIALOG_OPENFOLDER, folder_path_out,
                      folder_path_out_size, NULL,
                      "Select Folder Containing ASM Files");
}

int FileDialog_ConfirmSaveDiscard(const char* title, const char* message) {
    enum { CHOICE_CANCEL = 0, CHOICE_SAVE = 1, CHOICE_DISCARD = 2 };
    const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, CHOICE_SAVE, "Save" },
        { 0, CHOICE_DISCARD, "Discard" },
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, CHOICE_CANCEL, "Cancel" }
    };
    const SDL_MessageBoxData data = {
        SDL_MESSAGEBOX_WARNING,
        dialog_parent,
        title ? title : "3DCad - Unsaved changes",
        message ? message : "Save changes before continuing?",
        (int)(sizeof(buttons) / sizeof(buttons[0])),
        buttons,
        NULL
    };
    int choice = CHOICE_CANCEL;
    if (!SDL_ShowMessageBox(&data, &choice)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to show confirmation dialog: %s", SDL_GetError());
        return FILE_DIALOG_CONFIRM_CANCEL;
    }
    if (choice == CHOICE_SAVE) return FILE_DIALOG_CONFIRM_SAVE;
    if (choice == CHOICE_DISCARD) return FILE_DIALOG_CONFIRM_DISCARD;
    return FILE_DIALOG_CONFIRM_CANCEL;
}

int FileDialog_ConfirmContinue(const char* title, const char* message) {
    enum { CHOICE_CANCEL = 0, CHOICE_CONTINUE = 1 };
    const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT,
          CHOICE_CONTINUE, "Continue" },
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, CHOICE_CANCEL, "Cancel" }
    };
    const SDL_MessageBoxData data = {
        SDL_MESSAGEBOX_WARNING,
        dialog_parent,
        title ? title : "3DCad - Warning",
        message ? message : "Continue?",
        (int)(sizeof(buttons) / sizeof(buttons[0])),
        buttons,
        NULL
    };
    int choice = CHOICE_CANCEL;
    if (!SDL_ShowMessageBox(&data, &choice)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to show warning dialog: %s", SDL_GetError());
        return 0;
    }
    return choice == CHOICE_CONTINUE;
}
