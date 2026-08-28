/* Simple CLI converter: .cad -> .txt
 * Usage: cad23dg1 <input.cad> [output.txt]
 */
// A little CLI frontend so I can use the existing components to convert Iwamoto 3D-CAD files to Fundoshi-Kun format - Sunlit

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include "cad_file.h"
#include "cad_export_3dg1.h"
#include "cad_core.h"

static char* copy_string(const char* value) {
    size_t size;
    char* result;
    if (!value) return NULL;
    size = strlen(value) + 1;
    result = (char*)malloc(size);
    if (result) memcpy(result, value, size);
    return result;
}

static char* default_output_path(const char* input) {
    const char* slash;
    const char* backslash;
    const char* separator;
    const char* dot;
    size_t stemLength;
    char* output;
    if (!input) return NULL;
    slash = strrchr(input, '/');
    backslash = strrchr(input, '\\');
    separator = slash;
    if (backslash && (!separator || backslash > separator)) separator = backslash;
    dot = strrchr(input, '.');
    if (dot && separator && dot < separator + 1) dot = NULL;
    if (dot && separator && dot == separator + 1) dot = NULL;
    if (dot && !separator && dot == input) dot = NULL;
    stemLength = dot ? (size_t)(dot - input) : strlen(input);
    output = (char*)malloc(stemLength + sizeof(".txt"));
    if (!output) return NULL;
    memcpy(output, input, stemLength);
    memcpy(output + stemLength, ".txt", sizeof(".txt"));
    return output;
}

static int converter_main(int argc, char** argv) {
    CadCore core;
    const char* inpath;
    char* outpath;
    int exitCode = 0;
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input.cad> [output.txt]\n", argc > 0 ? argv[0] : "cad23dg1");
        return 1;
    }

    inpath = argv[1];
    outpath = argc >= 3 ? copy_string(argv[2]) : default_output_path(inpath);
    if (!outpath) {
        fprintf(stderr, "Could not allocate the output path\n");
        return 1;
    }

    CadCore_Init(&core);
    if (!CadCore_LoadFile(&core, inpath)) {
        fprintf(stderr, "Failed to load CAD file '%s'\n", inpath);
        exitCode = 2;
    } else if (!CadExport_3DG1(&core, outpath)) {
        fprintf(stderr, "Failed to export Fundoshi-Kun file '%s'\n", outpath);
        exitCode = 3;
    }
    CadCore_Destroy(&core);
    free(outpath);
    return exitCode;
}

#ifdef _WIN32
static char* wide_to_utf8(const wchar_t* value) {
    int size;
    char* result;
    if (!value) return NULL;
    size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                               NULL, 0, NULL, NULL);
    if (size <= 0) return NULL;
    result = (char*)malloc((size_t)size);
    if (!result) return NULL;
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                             result, size, NULL, NULL)) {
        free(result);
        return NULL;
    }
    return result;
}
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    LPWSTR* wideArgv;
    char** utf8Argv;
    int wideArgc = 0;
    int i;
    int result;
    (void)argc;
    (void)argv;
    wideArgv = CommandLineToArgvW(GetCommandLineW(), &wideArgc);
    if (!wideArgv) {
        fprintf(stderr, "Could not read the Unicode command line\n");
        return 1;
    }
    utf8Argv = (char**)calloc((size_t)wideArgc, sizeof(*utf8Argv));
    if (!utf8Argv) {
        LocalFree(wideArgv);
        return 1;
    }
    for (i = 0; i < wideArgc; ++i) {
        utf8Argv[i] = wide_to_utf8(wideArgv[i]);
        if (!utf8Argv[i]) break;
    }
    if (i != wideArgc) {
        fprintf(stderr, "Could not convert the Unicode command line to UTF-8\n");
        result = 1;
    } else {
        result = converter_main(wideArgc, utf8Argv);
    }
    while (i-- > 0) free(utf8Argv[i]);
    free(utf8Argv);
    LocalFree(wideArgv);
    return result;
#else
    return converter_main(argc, argv);
#endif
}
