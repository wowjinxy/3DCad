#include "cad_document.h"
#include "platform_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", \
                __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

int main(void) {
    static const uint8_t source[] =
        "3DAN\r\n3\r\n2\r\n"
        "0 0 0\r\n1 0 0\r\n0 1 0\r\n"
        "0 0 1\r\n1 0 1\r\n0 1 1\r\n"
        "3 0 1 2 9\r\n\x1a";
    static const uint8_t broken[] = "3DAN\n3\n2\n0 0";
    const char* importPath = "threedcad-animation-import.anm";
    const char* brokenPath = "threedcad-animation-broken.anm";
    const char* nativePath = "threedcad-animation-native.cad";
    const char* exportPath = "threedcad-animation-export.anm";
    CadDocument* document = (CadDocument*)malloc(sizeof(*document));
    CadFileData* before = (CadFileData*)malloc(sizeof(*before));
    uint8_t* reread = NULL;
    size_t rereadSize = 0;
    CadResult result;

    CHECK(document != NULL && before != NULL);
    if (!document || !before) goto done;
    result = CadPlatform_WriteFileAtomic(importPath, source,
                                         sizeof(source) - 1);
    CHECK(CadResult_IsSuccess(&result));
    result = CadPlatform_WriteFileAtomic(brokenPath, broken,
                                         sizeof(broken) - 1);
    CHECK(CadResult_IsSuccess(&result));
    CadDocument_Init(document);
    result = CadDocument_ImportAnm(document, importPath);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(result.format == CAD_FORMAT_ANM_3DAN);
    CHECK(document->sourceFormat == CAD_FORMAT_ANM_3DAN);
    CHECK(document->isDirty);
    CHECK(document->savePath == NULL);
    CHECK(document->lastImportPath != NULL);
    CHECK(document->core.data.animationIndexCount == 1);

    {
        uint64_t revision = document->revision;
        uint64_t savedRevision = document->savedRevision;
        result = CadDocument_Save(
            document, "missing-threedcad-document-dir/native.cad");
        CHECK(!CadResult_IsSuccess(&result));
        CHECK(document->revision == revision);
        CHECK(document->savedRevision == savedRevision);
        CHECK(document->sourceFormat == CAD_FORMAT_ANM_3DAN);
        CHECK(document->sourcePath &&
              strcmp(document->sourcePath, importPath) == 0);
        CHECK(document->savePath == NULL);
        CHECK(document->isDirty);
        result = CadDocument_ExportAnm(
            document, "missing-threedcad-document-dir/export.anm",
            CAD_FORMAT_ANM_3DAN);
        CHECK(!CadResult_IsSuccess(&result));
        CHECK(document->revision == revision);
        CHECK(document->savedRevision == savedRevision);
        CHECK(document->lastExportPath == NULL);
        CHECK(document->sourceFormat == CAD_FORMAT_ANM_3DAN);
        CHECK(document->isDirty);
    }

    result = CadDocument_Save(document, importPath);
    CHECK(!CadResult_IsSuccess(&result));
    result = CadDocument_ExportAnm(document, importPath,
                                   CAD_FORMAT_ANM_3DAN);
    CHECK(!CadResult_IsSuccess(&result));
    result = CadDocument_SaveCurrent(document);
    CHECK(!CadResult_IsSuccess(&result));
    result = CadPlatform_ReadFile(importPath, sizeof(source),
                                  &reread, &rereadSize);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(rereadSize == sizeof(source) - 1);
    CHECK(reread && memcmp(reread, source, rereadSize) == 0);
    CadPlatform_Free(reread);
    reread = NULL;

    *before = document->core.data;
    result = CadDocument_ImportAnm(document, brokenPath);
    CHECK(!CadResult_IsSuccess(&result));
    CHECK(memcmp(before, &document->core.data, sizeof(*before)) == 0);
    CHECK(document->sourceFormat == CAD_FORMAT_ANM_3DAN);

    result = CadDocument_ExportAnm(document, exportPath,
                                   CAD_FORMAT_ANM_3DGI);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(document->lastExportPath != NULL);
    CHECK(document->isDirty);
    result = CadPlatform_ReadFile(exportPath,
                                  CAD_PLATFORM_DEFAULT_FILE_LIMIT,
                                  &reread, &rereadSize);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(rereadSize >= 4 && memcmp(reread, "3DGI", 4) == 0);
    CadPlatform_Free(reread);
    reread = NULL;

    result = CadDocument_Save(document, nativePath);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(!document->isDirty);
    CHECK(document->sourceFormat == CAD_FORMAT_X11_STREAM);
    CHECK(document->savePath != NULL);
    CHECK(document->lastImportPath != NULL);
    CadDocument_Destroy(document);

done:
    CadPlatform_Free(reread);
    free(document);
    free(before);
    remove(importPath);
    remove(brokenPath);
    remove(nativePath);
    remove(exportPath);
    if (failures) {
        fprintf(stderr, "%d ANM document test(s) failed\n", failures);
        return 1;
    }
    puts("All ANM document tests passed.");
    return 0;
}
