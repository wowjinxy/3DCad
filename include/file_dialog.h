#pragma once

/* ============================================================================
   file_dialog.h
   Native Windows file dialogs
   ============================================================================ */

/* Open file dialog
   Returns 1 if user selected a file, 0 if cancelled
   filename_out must be at least MAX_PATH characters */
int FileDialog_Open(char* filename_out, int filename_out_size, const char* filter, const char* title);

/* Save file dialog
   Returns 1 if user selected a file, 0 if cancelled
   filename_out must be at least MAX_PATH characters */
int FileDialog_Save(char* filename_out, int filename_out_size, const char* filter, const char* title);

/* Convenience functions for CAD files */
int FileDialog_OpenCAD(char* filename_out, int filename_out_size);
int FileDialog_SaveCAD(char* filename_out, int filename_out_size);

/* Folder selection dialog
   Returns 1 if user selected a folder, 0 if cancelled
   folder_path_out must be at least MAX_PATH characters */
int FileDialog_SelectFolder(char* folder_path_out, int folder_path_out_size);


