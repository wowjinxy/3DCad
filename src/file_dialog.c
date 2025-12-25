#define _CRT_SECURE_NO_WARNINGS

#include "file_dialog.h"
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <string.h>
#include <stdio.h>
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

/* Open file dialog - uses Unicode version for proper path handling */
int FileDialog_Open(char* filename_out, int filename_out_size, const char* filter, const char* title) {
    if (!filename_out || filename_out_size < MAX_PATH) return 0;
    
    /* Convert filter to wide string */
    wchar_t wfilter[256] = {0};
    if (filter) {
        MultiByteToWideChar(CP_UTF8, 0, filter, -1, wfilter, sizeof(wfilter) / sizeof(wfilter[0]));
    } else {
        wcscpy(wfilter, L"All Files\0*.*\0\0");
    }
    
    /* Convert title to wide string */
    wchar_t wtitle[256] = {0};
    if (title) {
        MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, sizeof(wtitle) / sizeof(wtitle[0]));
    } else {
        wcscpy(wtitle, L"Open File");
    }
    
    OPENFILENAMEW ofn;
    wchar_t szFile[MAX_PATH] = {0};
    
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = szFile;
    ofn.lpstrFile[0] = L'\0';
    ofn.nMaxFile = sizeof(szFile) / sizeof(szFile[0]);
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
            return 1;
        }
    }
    
    return 0;
}

/* Save file dialog - uses Unicode version for proper path handling */
int FileDialog_Save(char* filename_out, int filename_out_size, const char* filter, const char* title) {
    if (!filename_out || filename_out_size < MAX_PATH) return 0;
    
    /* Convert filter to wide string */
    wchar_t wfilter[256] = {0};
    if (filter) {
        MultiByteToWideChar(CP_UTF8, 0, filter, -1, wfilter, sizeof(wfilter) / sizeof(wfilter[0]));
    } else {
        wcscpy(wfilter, L"All Files\0*.*\0\0");
    }
    
    /* Convert title to wide string */
    wchar_t wtitle[256] = {0};
    if (title) {
        MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, sizeof(wtitle) / sizeof(wtitle[0]));
    } else {
        wcscpy(wtitle, L"Save File");
    }
    
    OPENFILENAMEW ofn;
    wchar_t szFile[MAX_PATH] = {0};
    
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = szFile;
    ofn.lpstrFile[0] = L'\0';
    ofn.nMaxFile = sizeof(szFile) / sizeof(szFile[0]);
    ofn.lpstrFilter = wfilter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    ofn.lpstrTitle = wtitle;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    
    if (GetSaveFileNameW(&ofn)) {
        /* Convert wide string to UTF-8 */
        if (WideToUTF8(szFile, filename_out, filename_out_size)) {
            return 1;
        }
    }
    
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

/* Convert UTF-8 to wide string */
static int UTF8ToWide(const char* utf8, wchar_t* wide, int wide_size) {
    if (!utf8 || !wide || wide_size <= 0) return 0;
    
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, wide_size);
    if (len <= 0) {
        wide[0] = L'\0';
        return 0;
    }
    return 1;
}

/* Folder selection dialog using IFileOpenDialog (faster, modern API) */
int FileDialog_SelectFolder(char* folder_path_out, int folder_path_out_size) {
    if (!folder_path_out || folder_path_out_size < MAX_PATH) return 0;
    
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
            hr = pFileOpen->lpVtbl->Show(pFileOpen, NULL);
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
    }
    
    /* Fallback to old SHBrowseForFolder if IFileOpenDialog fails */
    BROWSEINFOW bi = {0};
    wchar_t szPath[MAX_PATH] = {0};
    wchar_t szDisplayName[MAX_PATH] = {0};
    
    bi.hwndOwner = NULL;
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

