#pragma once

/* SDL3-backed native dialogs. Paths are returned as UTF-8 on desktop
   platforms. These calls are modal to preserve the editor's transactional
   command flow while SDL services its asynchronous platform dialog. */

typedef struct SDL_Window SDL_Window;

enum {
    FILE_DIALOG_CONFIRM_CANCEL = 0,
    FILE_DIALOG_CONFIRM_SAVE = 1,
    FILE_DIALOG_CONFIRM_DISCARD = 2
};

void FileDialog_SetParent(SDL_Window* window);

int FileDialog_Open(char* filename_out, int filename_out_size,
                    const char* filter, const char* title);
int FileDialog_Save(char* filename_out, int filename_out_size,
                    const char* filter, const char* title);
int FileDialog_OpenCAD(char* filename_out, int filename_out_size);
int FileDialog_SaveCAD(char* filename_out, int filename_out_size);
int FileDialog_SelectFolder(char* folder_path_out, int folder_path_out_size);

int FileDialog_ConfirmSaveDiscard(const char* title, const char* message);
int FileDialog_ConfirmContinue(const char* title, const char* message);
