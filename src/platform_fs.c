#define _CRT_SECURE_NO_WARNINGS
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "platform_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <wchar.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

static CadResult fs_error(CadStatus status, const char* message) {
    CadResult result = CadResult_Ok(CAD_FORMAT_AUTO);
    result.status = status;
    result.errorCount = 1;
    result.diagnosticCount = 1;
    result.diagnostics[0].severity = CAD_DIAGNOSTIC_ERROR;
    result.diagnostics[0].code = status;
    result.diagnostics[0].recordTag = -1;
    result.diagnostics[0].recordIndex = -1;
    if (message) {
        snprintf(result.diagnostics[0].message,
                 sizeof(result.diagnostics[0].message), "%s", message);
    }
    return result;
}

static int valid_path(const char* path) {
    size_t length;
    if (!path || !path[0]) return 0;
    length = strlen(path);
    return length < CAD_PLATFORM_PATH_BYTE_LIMIT;
}

#ifdef _WIN32
static wchar_t* path_to_wide(const char* path) {
    wchar_t* wide;
    int count;
    if (!valid_path(path)) return NULL;
    count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                path, -1, NULL, 0);
    if (count <= 0 || count > 32767) return NULL;
    wide = (wchar_t*)malloc((size_t)count * sizeof(*wide));
    if (!wide) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                             path, -1, wide, count)) {
        free(wide);
        return NULL;
    }
    return wide;
}

static FILE* open_file(const char* path, const wchar_t* mode) {
    wchar_t* wide = path_to_wide(path);
    FILE* file = wide ? _wfopen(wide, mode) : NULL;
    free(wide);
    return file;
}

static int delete_file(const char* path) {
    wchar_t* wide = path_to_wide(path);
    int ok = wide && DeleteFileW(wide);
    free(wide);
    return ok;
}

static int replace_file(const char* temporary, const char* destination) {
    wchar_t* wideTemporary = path_to_wide(temporary);
    wchar_t* wideDestination = path_to_wide(destination);
    int ok = wideTemporary && wideDestination &&
             MoveFileExW(wideTemporary, wideDestination,
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    free(wideTemporary);
    free(wideDestination);
    return ok;
}

static wchar_t* absolute_wide_path(const char* path) {
    wchar_t* wide = path_to_wide(path);
    wchar_t* absolute;
    DWORD required;
    if (!wide) return NULL;
    required = GetFullPathNameW(wide, 0, NULL, NULL);
    if (!required || required > 32767) {
        free(wide);
        return NULL;
    }
    absolute = (wchar_t*)malloc((size_t)required * sizeof(*absolute));
    if (!absolute || !GetFullPathNameW(wide, required, absolute, NULL)) {
        free(absolute);
        absolute = NULL;
    }
    free(wide);
    return absolute;
}
#else
static FILE* open_file(const char* path, const char* mode) {
    return valid_path(path) ? fopen(path, mode) : NULL;
}
static int delete_file(const char* path) { return remove(path) == 0; }
static int replace_file(const char* temporary, const char* destination) {
    return rename(temporary, destination) == 0;
}
#endif

int CadPlatform_PathsEqual(const char* firstUtf8Path,
                           const char* secondUtf8Path) {
    if (!valid_path(firstUtf8Path) || !valid_path(secondUtf8Path)) return 0;
#ifdef _WIN32
    wchar_t* first = absolute_wide_path(firstUtf8Path);
    wchar_t* second = absolute_wide_path(secondUtf8Path);
    HANDLE firstHandle = INVALID_HANDLE_VALUE;
    HANDLE secondHandle = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION firstInfo;
    BY_HANDLE_FILE_INFORMATION secondInfo;
    int equal = 0;
    if (!first || !second) goto done;
    firstHandle = CreateFileW(first, 0,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              NULL);
    secondHandle = CreateFileW(second, 0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE |
                                   FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                               NULL);
    if (firstHandle != INVALID_HANDLE_VALUE &&
        secondHandle != INVALID_HANDLE_VALUE &&
        GetFileInformationByHandle(firstHandle, &firstInfo) &&
        GetFileInformationByHandle(secondHandle, &secondInfo)) {
        equal = firstInfo.dwVolumeSerialNumber == secondInfo.dwVolumeSerialNumber &&
                firstInfo.nFileIndexHigh == secondInfo.nFileIndexHigh &&
                firstInfo.nFileIndexLow == secondInfo.nFileIndexLow;
    }
    if (!equal) equal = _wcsicmp(first, second) == 0;
done:
    if (firstHandle != INVALID_HANDLE_VALUE) CloseHandle(firstHandle);
    if (secondHandle != INVALID_HANDLE_VALUE) CloseHandle(secondHandle);
    free(first);
    free(second);
    return equal;
#else
    struct stat firstInfo;
    struct stat secondInfo;
    char* first;
    char* second;
    int equal;
    if (stat(firstUtf8Path, &firstInfo) == 0 &&
        stat(secondUtf8Path, &secondInfo) == 0 &&
        firstInfo.st_dev == secondInfo.st_dev &&
        firstInfo.st_ino == secondInfo.st_ino)
        return 1;
    first = realpath(firstUtf8Path, NULL);
    second = realpath(secondUtf8Path, NULL);
    equal = first && second ? strcmp(first, second) == 0
                            : strcmp(firstUtf8Path, secondUtf8Path) == 0;
    free(first);
    free(second);
    return equal;
#endif
}

CadResult CadPlatform_ReadFile(const char* utf8Path, size_t sizeLimit,
                               uint8_t** outputBytes, size_t* outputSize) {
    FILE* file;
    long length;
    uint8_t* bytes;
    if (!outputBytes || !outputSize || !valid_path(utf8Path))
        return fs_error(CAD_STATUS_INVALID_ARGUMENT,
                        "Read requires a valid bounded UTF-8 path and outputs");
    *outputBytes = NULL;
    *outputSize = 0;
    if (!sizeLimit) sizeLimit = CAD_PLATFORM_DEFAULT_FILE_LIMIT;
#ifdef _WIN32
    file = open_file(utf8Path, L"rb");
#else
    file = open_file(utf8Path, "rb");
#endif
    if (!file)
        return fs_error(CAD_STATUS_IO_ERROR, "Could not open file for reading");
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return fs_error(CAD_STATUS_IO_ERROR, "Could not determine file size");
    }
    if ((size_t)length > sizeLimit) {
        fclose(file);
        return fs_error(CAD_STATUS_INDEX_OUT_OF_RANGE,
                        "File exceeds the configured read limit");
    }
    bytes = (uint8_t*)malloc(length ? (size_t)length : 1u);
    if (!bytes) {
        fclose(file);
        return fs_error(CAD_STATUS_OUT_OF_MEMORY,
                        "Could not allocate the bounded file buffer");
    }
    if ((length && fread(bytes, 1, (size_t)length, file) != (size_t)length) ||
        fclose(file) != 0) {
        free(bytes);
        return fs_error(CAD_STATUS_IO_ERROR,
                        "Could not read the complete file");
    }
    *outputBytes = bytes;
    *outputSize = (size_t)length;
    return CadResult_Ok(CAD_FORMAT_AUTO);
}

static char* temporary_path(const char* path) {
#ifdef _WIN32
    static volatile LONG counter;
#else
    static unsigned counter;
#endif
    char suffix[64];
    char* result;
    size_t length;
#ifdef _WIN32
    unsigned process = (unsigned)GetCurrentProcessId();
    unsigned serial = (unsigned)InterlockedIncrement(&counter);
#else
    unsigned process = (unsigned)getpid();
    unsigned serial = ++counter;
#endif
    snprintf(suffix, sizeof(suffix), ".tmp-%u-%u", process, serial);
    length = strlen(path);
    if (length + strlen(suffix) >= CAD_PLATFORM_PATH_BYTE_LIMIT) return NULL;
    result = (char*)malloc(length + strlen(suffix) + 1);
    if (!result) return NULL;
    memcpy(result, path, length);
    strcpy(result + length, suffix);
    return result;
}

CadResult CadPlatform_WriteFileAtomic(const char* utf8Path,
                                      const uint8_t* bytes, size_t size) {
    char* temporary;
    FILE* file;
    CadResult result;
    if (!valid_path(utf8Path) || (!bytes && size))
        return fs_error(CAD_STATUS_INVALID_ARGUMENT,
                        "Write requires a valid bounded UTF-8 path and buffer");
    temporary = temporary_path(utf8Path);
    if (!temporary)
        return fs_error(CAD_STATUS_OUT_OF_MEMORY,
                        "Could not allocate the atomic-save path");
#ifdef _WIN32
    file = open_file(temporary, L"wb");
#else
    file = open_file(temporary, "wb");
#endif
    if (!file || (size && fwrite(bytes, 1, size, file) != size) ||
        fflush(file) != 0
#ifdef _WIN32
        || _commit(_fileno(file)) != 0
#else
        || fsync(fileno(file)) != 0
#endif
    ) {
        if (file) fclose(file);
        delete_file(temporary);
        free(temporary);
        return fs_error(CAD_STATUS_IO_ERROR,
                        "Could not write and flush the temporary file");
    }
    if (fclose(file) != 0 || !replace_file(temporary, utf8Path)) {
        delete_file(temporary);
        free(temporary);
        return fs_error(CAD_STATUS_IO_ERROR,
                        "Could not atomically replace the destination file");
    }
    free(temporary);
    result = CadResult_Ok(CAD_FORMAT_AUTO);
    result.bytesConsumed = size;
    return result;
}

void CadPlatform_Free(void* allocation) { free(allocation); }
