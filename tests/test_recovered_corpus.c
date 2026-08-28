#define _CRT_SECURE_NO_WARNINGS

#include "cad_codec.h"
#include "platform_fs.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

static int expected_count_matches(const char* variable, unsigned actual)
{
    const char* text = getenv(variable);
    char* end = NULL;
    unsigned long expected;
    if (!text || !text[0]) return 1;
    expected = strtoul(text, &end, 10);
    if (!end || *end || expected > UINT_MAX) {
        fprintf(stderr, "Invalid %s expectation: %s\n", variable, text);
        return 0;
    }
    if (actual != (unsigned)expected) {
        fprintf(stderr, "%s expected %lu, found %u\n",
                variable, expected, actual);
        return 0;
    }
    return 1;
}

static int expected_cad_counts_match(unsigned total, unsigned x11,
                                     unsigned legacy)
{
    return expected_count_matches("THREEDCAD_EXPECT_CAD_TOTAL", total) &&
           expected_count_matches("THREEDCAD_EXPECT_CAD_X11", x11) &&
           expected_count_matches("THREEDCAD_EXPECT_CAD_LEGACY", legacy);
}

static CadResult decode_path(const char* path, CadFileData* data)
{
    uint8_t* bytes = NULL;
    size_t size = 0;
    CadResult result = CadPlatform_ReadFile(
        path, CAD_PLATFORM_DEFAULT_FILE_LIMIT, &bytes, &size);
    if (CadResult_IsSuccess(&result))
        result = CadCodec_Decode(bytes, size, CAD_FORMAT_AUTO, data);
    CadPlatform_Free(bytes);
    return result;
}

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

typedef struct CorpusCounts {
    unsigned total;
    unsigned x11;
    unsigned legacy;
    unsigned failed;
} CorpusCounts;

static int wide_to_utf8(const wchar_t* value, char** output)
{
    int size;
    char* buffer;
    if (!value || !output) return 0;
    size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, NULL, 0, NULL, NULL);
    if (size <= 0) return 0;
    buffer = (char*)malloc((size_t)size);
    if (!buffer) return 0;
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                            buffer, size, NULL, NULL) <= 0) {
        free(buffer);
        return 0;
    }
    *output = buffer;
    return 1;
}

#if !defined(_MSC_VER)
static int utf8_to_wide(const char* value, wchar_t** output)
{
    int size;
    wchar_t* buffer;
    if (!value || !output) return 0;
    size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, NULL, 0);
    if (size <= 0) return 0;
    buffer = (wchar_t*)malloc((size_t)size * sizeof(wchar_t));
    if (!buffer) return 0;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, buffer, size) <= 0) {
        free(buffer);
        return 0;
    }
    *output = buffer;
    return 1;
}
#endif

static int has_cad_extension(const wchar_t* name)
{
    size_t length = wcslen(name);
    if (length < 4) return 0;
    return name[length - 4] == L'.' &&
           towlower(name[length - 3]) == L'c' &&
           towlower(name[length - 2]) == L'a' &&
           towlower(name[length - 1]) == L'd';
}

static void validate_file(const wchar_t* path, CorpusCounts* counts)
{
    CadFileData* data;
    CadResult result;
    char* utf8 = NULL;
    if (!wide_to_utf8(path, &utf8)) {
        ++counts->failed;
        return;
    }
    data = (CadFileData*)malloc(sizeof(*data));
    if (!data) {
        free(utf8);
        ++counts->failed;
        return;
    }
    result = decode_path(utf8, data);
    ++counts->total;
    if (!CadResult_IsSuccess(&result)) {
        const char* message = result.diagnosticCount ? result.diagnostics[0].message : CadStatus_Name(result.status);
        fprintf(stderr, "FAIL: %s: %s\n", utf8, message);
        ++counts->failed;
    } else if (result.format == CAD_FORMAT_LEGACY_PACKED) {
        ++counts->legacy;
    } else {
        ++counts->x11;
    }
    free(data);
    free(utf8);
}

static void walk_directory(const wchar_t* directory, CorpusCounts* counts)
{
    WIN32_FIND_DATAW entry;
    HANDLE search;
    size_t length = wcslen(directory);
    wchar_t* pattern = (wchar_t*)malloc((length + 3) * sizeof(wchar_t));
    if (!pattern) {
        ++counts->failed;
        return;
    }
    wcscpy(pattern, directory);
    if (length && directory[length - 1] != L'\\' && directory[length - 1] != L'/') wcscat(pattern, L"\\");
    wcscat(pattern, L"*");
    search = FindFirstFileW(pattern, &entry);
    free(pattern);
    if (search == INVALID_HANDLE_VALUE) {
        ++counts->failed;
        return;
    }
    do {
        wchar_t* child;
        size_t child_length;
        if (!wcscmp(entry.cFileName, L".") || !wcscmp(entry.cFileName, L"..")) continue;
        child_length = length + wcslen(entry.cFileName) + 2;
        child = (wchar_t*)malloc(child_length * sizeof(wchar_t));
        if (!child) {
            ++counts->failed;
            continue;
        }
        wcscpy(child, directory);
        if (length && directory[length - 1] != L'\\' && directory[length - 1] != L'/') wcscat(child, L"\\");
        wcscat(child, entry.cFileName);
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) walk_directory(child, counts);
        } else if (has_cad_extension(entry.cFileName)) {
            validate_file(child, counts);
        }
        free(child);
    } while (FindNextFileW(search, &entry));
    FindClose(search);
}

#if defined(_MSC_VER)
int wmain(int argc, wchar_t** argv)
{
    CorpusCounts counts = { 0, 0, 0, 0 };
    if (argc != 2) {
        fwprintf(stderr, L"usage: %ls <recovered-data-root>\n", argv[0]);
        return 2;
    }
    walk_directory(argv[1], &counts);
    printf("CAD corpus: %u total, %u X11 stream, %u legacy packed, %u failed\n",
           counts.total, counts.x11, counts.legacy, counts.failed);
    return counts.total && !counts.failed &&
           expected_cad_counts_match(counts.total, counts.x11, counts.legacy)
               ? 0 : 1;
}
#else
int main(int argc, char** argv)
{
    wchar_t* root = NULL;
    CorpusCounts counts = { 0, 0, 0, 0 };
    if (argc != 2 || !utf8_to_wide(argv[1], &root)) return 2;
    walk_directory(root, &counts);
    free(root);
    printf("CAD corpus: %u total, %u X11 stream, %u legacy packed, %u failed\n",
           counts.total, counts.x11, counts.legacy, counts.failed);
    return counts.total && !counts.failed &&
           expected_cad_counts_match(counts.total, counts.x11, counts.legacy)
               ? 0 : 1;
}
#endif

#else

#include <dirent.h>
#include <sys/stat.h>

typedef struct CorpusCounts {
    unsigned total, x11, legacy, failed;
} CorpusCounts;

static int has_cad_extension(const char* name)
{
    size_t length = strlen(name);
    return length >= 4 && name[length - 4] == '.' &&
           tolower((unsigned char)name[length - 3]) == 'c' &&
           tolower((unsigned char)name[length - 2]) == 'a' &&
           tolower((unsigned char)name[length - 1]) == 'd';
}

static void validate_file(const char* path, CorpusCounts* counts)
{
    CadFileData* data = (CadFileData*)malloc(sizeof(*data));
    CadResult result;
    if (!data) { ++counts->failed; return; }
    result = decode_path(path, data);
    ++counts->total;
    if (!CadResult_IsSuccess(&result)) {
        fprintf(stderr, "FAIL: %s\n", path);
        ++counts->failed;
    } else if (result.format == CAD_FORMAT_LEGACY_PACKED) ++counts->legacy;
    else ++counts->x11;
    free(data);
}

static void walk_directory(const char* directory, CorpusCounts* counts)
{
    DIR* stream = opendir(directory);
    struct dirent* entry;
    if (!stream) { ++counts->failed; return; }
    while ((entry = readdir(stream)) != NULL) {
        char* child;
        struct stat status;
        size_t size;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        size = strlen(directory) + strlen(entry->d_name) + 2;
        child = (char*)malloc(size);
        if (!child) { ++counts->failed; continue; }
        snprintf(child, size, "%s/%s", directory, entry->d_name);
        if (!stat(child, &status)) {
            if (S_ISDIR(status.st_mode)) walk_directory(child, counts);
            else if (S_ISREG(status.st_mode) && has_cad_extension(entry->d_name)) validate_file(child, counts);
        }
        free(child);
    }
    closedir(stream);
}

int main(int argc, char** argv)
{
    CorpusCounts counts = { 0, 0, 0, 0 };
    if (argc != 2) return 2;
    walk_directory(argv[1], &counts);
    printf("CAD corpus: %u total, %u X11 stream, %u legacy packed, %u failed\n",
           counts.total, counts.x11, counts.legacy, counts.failed);
    return counts.total && !counts.failed &&
           expected_cad_counts_match(counts.total, counts.x11, counts.legacy)
               ? 0 : 1;
}
#endif
