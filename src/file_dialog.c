#define _CRT_SECURE_NO_WARNINGS

#include "file_dialog.h"
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#ifndef CP_UTF8
#define CP_UTF8 65001
#endif

/* Convert wide string to UTF-8 */
static int WideToUTF8(const wchar_t* wide, char* utf8, int utf8_size) {
    if (!wide || !utf8 || utf8_size <= 0) return 0;
    
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, utf8_size, NULL, NULL);
    if (len <= 0) {
        utf8[0] = '\0';
        return 0;
    }
    return 1;
}

/* OPENFILENAME filters are UTF-8 multi-strings terminated by two NULs.
   MultiByteToWideChar(..., -1, ...) stops at the first segment, so convert
   each segment independently and preserve the final empty segment. */
static int MultiStringToWide(const char* utf8, wchar_t* wide, int wide_count) {
    int written = 0;
    const char* segment = utf8;

    if (!utf8 || !wide || wide_count < 2) return 0;
    while (*segment) {
        int segment_length = (int)strlen(segment) + 1;
        int converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                            segment, segment_length,
                                            wide + written, wide_count - written);
        if (converted <= 0 || written + converted >= wide_count) return 0;
        written += converted;
        segment += segment_length;
    }
    wide[written++] = L'\0';
    return written;
}

static int Utf8ToWide(const char* utf8, wchar_t* wide, int wide_count) {
    if (!utf8 || !wide || wide_count <= 0) return 0;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1,
                               wide, wide_count) > 0;
}

static int DefaultExtensionFromFilter(const char* filter,
                                      wchar_t* extension,
                                      int extension_count) {
    const char* pattern;
    const char* end;
    char utf8Extension[32];
    size_t length;
    if (!filter || !extension || extension_count <= 0) return 0;
    pattern = filter + strlen(filter) + 1;
    if (pattern[0] != '*' || pattern[1] != '.' || pattern[2] == '*' ||
        pattern[2] == '\0') return 0;
    end = pattern + 2;
    while (*end && *end != ';' && *end != '*' && *end != '?') end++;
    length = (size_t)(end - (pattern + 2));
    if (!length || length >= sizeof(utf8Extension)) return 0;
    memcpy(utf8Extension, pattern + 2, length);
    utf8Extension[length] = '\0';
    return Utf8ToWide(utf8Extension, extension, extension_count);
}

/* Open file dialog - uses Unicode version for proper path handling */
int FileDialog_Open(char* filename_out, int filename_out_size, const char* filter, const char* title) {
    if (!filename_out || filename_out_size <= 1) return 0;
    filename_out[0] = '\0';
    
    /* Convert filter to wide string */
    wchar_t wfilter[256] = {0};
    if (filter) {
        if (!MultiStringToWide(filter, wfilter, (int)(sizeof(wfilter) / sizeof(wfilter[0])))) {
            return 0;
        }
    } else {
        if (!MultiStringToWide("All Files\0*.*\0\0", wfilter,
                               (int)(sizeof(wfilter) / sizeof(wfilter[0])))) return 0;
    }
    
    /* Convert title to wide string */
    wchar_t wtitle[256] = {0};
    if (title) {
        if (!Utf8ToWide(title, wtitle, (int)(sizeof(wtitle) / sizeof(wtitle[0])))) return 0;
    } else {
        wcscpy(wtitle, L"Open File");
    }
    
    OPENFILENAMEW ofn;
    wchar_t* szFile = (wchar_t*)calloc(32768u, sizeof(wchar_t));
    if (!szFile) return 0;
    
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFile = szFile;
    ofn.lpstrFile[0] = L'\0';
    ofn.nMaxFile = 32768;
    ofn.lpstrFilter = wfilter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    ofn.lpstrTitle = wtitle;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    
    if (GetOpenFileNameW(&ofn)) {
        /* Convert wide string to UTF-8 */
        if (WideToUTF8(szFile, filename_out, filename_out_size)) {
            free(szFile);
            return 1;
        }
    }
    free(szFile);
    return 0;
}

/* Save file dialog - uses Unicode version for proper path handling */
int FileDialog_Save(char* filename_out, int filename_out_size, const char* filter, const char* title) {
    if (!filename_out || filename_out_size <= 1) return 0;
    filename_out[0] = '\0';
    
    /* Convert filter to wide string */
    wchar_t wfilter[256] = {0};
    if (filter) {
        if (!MultiStringToWide(filter, wfilter, (int)(sizeof(wfilter) / sizeof(wfilter[0])))) {
            return 0;
        }
    } else {
        if (!MultiStringToWide("All Files\0*.*\0\0", wfilter,
                               (int)(sizeof(wfilter) / sizeof(wfilter[0])))) return 0;
    }
    
    /* Convert title to wide string */
    wchar_t wtitle[256] = {0};
    if (title) {
        if (!Utf8ToWide(title, wtitle, (int)(sizeof(wtitle) / sizeof(wtitle[0])))) return 0;
    } else {
        wcscpy(wtitle, L"Save File");
    }
    
    OPENFILENAMEW ofn;
    wchar_t wdefaultExtension[32] = {0};
    wchar_t* szFile = (wchar_t*)calloc(32768u, sizeof(wchar_t));
    if (!szFile) return 0;
    
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFile = szFile;
    ofn.lpstrFile[0] = L'\0';
    ofn.nMaxFile = 32768;
    ofn.lpstrFilter = wfilter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    ofn.lpstrTitle = wtitle;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = DefaultExtensionFromFilter(
        filter, wdefaultExtension,
        (int)(sizeof(wdefaultExtension) / sizeof(wdefaultExtension[0])))
        ? wdefaultExtension : NULL;
    
    if (GetSaveFileNameW(&ofn)) {
        /* Convert wide string to UTF-8 */
        if (WideToUTF8(szFile, filename_out, filename_out_size)) {
            free(szFile);
            return 1;
        }
    }
    free(szFile);
    return 0;
}

/* Convenience function for opening CAD files */
int FileDialog_OpenCAD(char* filename_out, int filename_out_size) {
    const char* filter = "CAD Files\0*.cad\0All Files\0*.*\0\0";
    return FileDialog_Open(filename_out, filename_out_size, filter, "Open CAD File");
}

/* Convenience function for saving CAD files */
int FileDialog_SaveCAD(char* filename_out, int filename_out_size) {
    const char* filter = "CAD Files\0*.cad\0All Files\0*.*\0\0";
    return FileDialog_Save(filename_out, filename_out_size, filter, "Save CAD File");
}

/* Folder selection dialog using IFileOpenDialog (faster, modern API) */
int FileDialog_SelectFolder(char* folder_path_out, int folder_path_out_size) {
    if (!folder_path_out || folder_path_out_size <= 1) return 0;
    folder_path_out[0] = '\0';
    
    /* Initialize COM if needed (required for IFileOpenDialog) */
    BOOL com_initialized = FALSE;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (SUCCEEDED(hr)) {
        com_initialized = TRUE;
    } else if (hr == RPC_E_CHANGED_MODE) {
        /* COM already initialized with different mode - try anyway */
    }
    
    /* Try modern IFileOpenDialog first (Windows Vista+) */
    IFileOpenDialog* pFileOpen = NULL;
    hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_ALL,
                         &IID_IFileOpenDialog, (void**)&pFileOpen);
    
    if (SUCCEEDED(hr)) {
        /* Set options to pick folders */
        DWORD dwOptions;
        hr = pFileOpen->lpVtbl->GetOptions(pFileOpen, &dwOptions);
        if (SUCCEEDED(hr)) {
            hr = pFileOpen->lpVtbl->SetOptions(pFileOpen, dwOptions | FOS_PICKFOLDERS);
        }
        
        /* Set title */
        if (SUCCEEDED(hr)) {
            wchar_t* title = L"Select Folder Containing ASM Files";
            pFileOpen->lpVtbl->SetTitle(pFileOpen, title);
        }
        
        /* Show dialog */
        if (SUCCEEDED(hr)) {
            hr = pFileOpen->lpVtbl->Show(pFileOpen, GetActiveWindow());
        }
        
        /* Get result */
        if (SUCCEEDED(hr)) {
            IShellItem* pItem = NULL;
            hr = pFileOpen->lpVtbl->GetResult(pFileOpen, &pItem);
            if (SUCCEEDED(hr)) {
                PWSTR pszFilePath = NULL;
                hr = pItem->lpVtbl->GetDisplayName(pItem, SIGDN_FILESYSPATH, &pszFilePath);
                if (SUCCEEDED(hr)) {
                    if (WideToUTF8(pszFilePath, folder_path_out, folder_path_out_size)) {
                        CoTaskMemFree(pszFilePath);
                        pItem->lpVtbl->Release(pItem);
                        pFileOpen->lpVtbl->Release(pFileOpen);
                        if (com_initialized) {
                            CoUninitialize();
                        }
                        return 1;
                    }
                    CoTaskMemFree(pszFilePath);
                }
                pItem->lpVtbl->Release(pItem);
            }
        }
        
        pFileOpen->lpVtbl->Release(pFileOpen);
        /* The modern dialog was available.  Cancellation and operational
           failures both end this request; do not surprise the user by
           opening a second legacy picker after Cancel. */
        if (com_initialized) CoUninitialize();
        return 0;
    }
    
    /* Fallback to old SHBrowseForFolder if IFileOpenDialog fails */
    BROWSEINFOW bi = {0};
    wchar_t szPath[MAX_PATH] = {0};
    wchar_t szDisplayName[MAX_PATH] = {0};
    
    bi.hwndOwner = GetActiveWindow();
    bi.pidlRoot = NULL;
    bi.pszDisplayName = szDisplayName;
    bi.lpszTitle = L"Select Folder Containing ASM Files";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpfn = NULL;
    bi.lParam = 0;
    bi.iImage = 0;
    
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl != NULL) {
        if (SHGetPathFromIDListW(pidl, szPath)) {
            if (WideToUTF8(szPath, folder_path_out, folder_path_out_size)) {
                CoTaskMemFree(pidl);
                if (com_initialized) {
                    CoUninitialize();
                }
                return 1;
            }
        }
        CoTaskMemFree(pidl);
    }
    
    if (com_initialized) {
        CoUninitialize();
    }
    
    return 0;
}

