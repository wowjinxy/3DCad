#include "cad_document.h"
#include "cad_anm_codec.h"
#include "cad_palette.h"
#include "platform_fs.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

struct CadDocumentSnapshot {
    CadCore core;
    uint8_t colorData[CAD_COLOR_DATA_SIZE];
    uint8_t paletteData[CAD_PALETTE_DATA_SIZE];
    size_t colorDataSize;
    size_t paletteDataSize;
    CadRgba palette[256];
    int paletteValid;
    uint64_t revision;
    uint64_t nextRevision;
    uint64_t colRevision;
    uint64_t nextColRevision;
    uint64_t palRevision;
    uint64_t nextPalRevision;
    /* Paths are retained only so a cancelled/rejected edit can restore
       resource associations. Undo/redo intentionally applies content and
       revisions without rewinding the user's current save destinations. */
    char* colSourcePath;
    char* colSavePath;
    char* palSourcePath;
    char* palSavePath;
    char label[CAD_DOCUMENT_HISTORY_LABEL_CAPACITY];
};

#define CAD_DOCUMENT_NO_SAVED_REVISION UINT64_MAX

static CadResult document_error(CadStatus status, const char* message) {
    CadResult result = CadResult_Ok(CAD_FORMAT_AUTO);
    result.status = status;
    result.errorCount = 1;
    result.diagnosticCount = 1;
    result.diagnostics[0].severity = CAD_DIAGNOSTIC_ERROR;
    result.diagnostics[0].code = status;
    result.diagnostics[0].recordTag = -1;
    result.diagnostics[0].recordIndex = -1;
    if (message) {
        size_t length = strlen(message);
        if (length >= sizeof(result.diagnostics[0].message))
            length = sizeof(result.diagnostics[0].message) - 1;
        memcpy(result.diagnostics[0].message, message, length);
        result.diagnostics[0].message[length] = '\0';
    }
    return result;
}

static char* duplicate_string(const char* value) {
    char* copy;
    size_t length;
    if (!value) return NULL;
    length = strlen(value);
    copy = (char*)malloc(length + 1);
    if (copy) memcpy(copy, value, length + 1);
    return copy;
}

static int replace_string(char** destination, const char* value) {
    char* copy = duplicate_string(value);
    if (value && !copy) return 0;
    free(*destination);
    *destination = copy;
    return 1;
}

static int path_matches(const char* candidate, const char* ownedPath) {
    return candidate && ownedPath &&
           CadPlatform_PathsEqual(candidate, ownedPath);
}

static int path_matches_palette_resource(const CadDocument* document,
                                         const char* candidate) {
    return document &&
           (path_matches(candidate, document->colSourcePath) ||
            path_matches(candidate, document->colSavePath) ||
            path_matches(candidate, document->palSourcePath) ||
            path_matches(candidate, document->palSavePath));
}

static int path_matches_model_resource(const CadDocument* document,
                                       const char* candidate) {
    return document &&
           (path_matches(candidate, document->sourcePath) ||
            path_matches(candidate, document->savePath) ||
            path_matches(candidate, document->lastImportPath) ||
            path_matches(candidate, document->lastExportPath));
}

static void clear_original_native_bytes(CadDocument* document) {
    if (!document) return;
    CadPlatform_Free(document->originalNativeBytes);
    document->originalNativeBytes = NULL;
    document->originalNativeByteCount = 0;
}

static int document_matches_original_native(const CadDocument* document) {
    CadFileData* decoded;
    CadResult result;
    int matches = 0;
    if (!document || !document->originalNativeBytes ||
        !document->originalNativeByteCount) return 0;
    decoded = (CadFileData*)malloc(sizeof(*decoded));
    if (!decoded) return -1;
    result = CadCodec_Decode(document->originalNativeBytes,
                             document->originalNativeByteCount,
                             CAD_FORMAT_X11_STREAM, decoded);
    if (CadResult_IsSuccess(&result)) {
        int index;
        /* Selection is session/editor state.  It may legitimately change
           while the model revision stays untouched, and must not force an
           otherwise byte-exact native save through the canonical encoder. */
        for (index = 0; index < CAD_MAX_OBJECTS; ++index)
            decoded->objects[index].selectFlag =
                document->core.data.objects[index].selectFlag;
        for (index = 0; index < CAD_MAX_POLYGONS; ++index)
            decoded->polygons[index].selectFlag =
                document->core.data.polygons[index].selectFlag;
        for (index = 0; index < CAD_MAX_POINTS; ++index)
            decoded->points[index].selectFlag =
                document->core.data.points[index].selectFlag;
        for (index = 0; index < CAD_MAX_ANIMATION_POINTS; ++index)
            decoded->animationPoints[index].selectFlag =
                document->core.data.animationPoints[index].selectFlag;
        matches = memcmp(decoded, &document->core.data, sizeof(*decoded)) == 0;
    }
    free(decoded);
    return matches;
}

static void snapshot_destroy(CadDocumentSnapshot* snapshot) {
    if (!snapshot) return;
    free(snapshot->colSourcePath);
    free(snapshot->colSavePath);
    free(snapshot->palSourcePath);
    free(snapshot->palSavePath);
    free(snapshot);
}

static void update_dirty_state(CadDocument* document) {
    if (!document) return;
    document->isDirty = document->revision != document->savedRevision;
    /* An undo may return to the state before a resource was created/opened.
       Absence is not a savable modified resource and must never trigger a
       Save prompt that cannot succeed. */
    document->colDirty = document->paletteValid &&
        document->colorDataSize == CAD_COL_FILE_SIZE &&
        document->colRevision != document->colSavedRevision;
    document->palDirty = document->paletteDataSize == CAD_PAL_FILE_SIZE &&
        document->palRevision != document->palSavedRevision;
    document->core.isDirty = document->isDirty;
}

static int core_state_matches(const CadCore* left, const CadCore* right) {
    const size_t dirtyOffset = offsetof(CadCore, isDirty);
    const size_t afterDirty = dirtyOffset + sizeof(left->isDirty);
    const unsigned char* leftBytes;
    const unsigned char* rightBytes;

    if (!left || !right) return 0;
    leftBytes = (const unsigned char*)left;
    rightBytes = (const unsigned char*)right;

    /* Compare the representation in place so this hot path never puts two
       ~350 KiB CadCore copies on the 1 MiB default Windows stack.  isDirty is
       the only ignored field because CadDocument derives it from revisions;
       the trailing comparison also covers fields appended to CadCore later. */
    return memcmp(leftBytes, rightBytes, dirtyOffset) == 0 &&
           memcmp(leftBytes + afterDirty, rightBytes + afterDirty,
                  sizeof(*left) - afterDirty) == 0;
}

static void copy_history_label(char destination[CAD_DOCUMENT_HISTORY_LABEL_CAPACITY],
                               const char* label) {
    size_t length = label ? strlen(label) : 0;
    if (length >= CAD_DOCUMENT_HISTORY_LABEL_CAPACITY)
        length = CAD_DOCUMENT_HISTORY_LABEL_CAPACITY - 1;
    if (length) memcpy(destination, label, length);
    destination[length] = '\0';
}

static const char* current_history_label(const CadDocument* document) {
    if (!document || !document->historyCount ||
        document->historyCursor >= document->historyCount ||
        !document->history[document->historyCursor]) return "";
    return document->history[document->historyCursor]->label;
}

static CadDocumentSnapshot* snapshot_create(const CadDocument* document,
                                            const char* label) {
    CadDocumentSnapshot* snapshot;
    if (!document) return NULL;
    snapshot = (CadDocumentSnapshot*)malloc(sizeof(*snapshot));
    if (!snapshot) return NULL;
    snapshot->core = document->core;
    memcpy(snapshot->colorData, document->colorData,
           sizeof(snapshot->colorData));
    memcpy(snapshot->paletteData, document->paletteData,
           sizeof(snapshot->paletteData));
    snapshot->colorDataSize = document->colorDataSize;
    snapshot->paletteDataSize = document->paletteDataSize;
    memcpy(snapshot->palette, document->palette, sizeof(snapshot->palette));
    snapshot->paletteValid = document->paletteValid;
    snapshot->revision = document->revision;
    snapshot->nextRevision = document->nextRevision;
    snapshot->colRevision = document->colRevision;
    snapshot->nextColRevision = document->nextColRevision;
    snapshot->palRevision = document->palRevision;
    snapshot->nextPalRevision = document->nextPalRevision;
    snapshot->colSourcePath = duplicate_string(document->colSourcePath);
    snapshot->colSavePath = duplicate_string(document->colSavePath);
    snapshot->palSourcePath = duplicate_string(document->palSourcePath);
    snapshot->palSavePath = duplicate_string(document->palSavePath);
    if ((document->colSourcePath && !snapshot->colSourcePath) ||
        (document->colSavePath && !snapshot->colSavePath) ||
        (document->palSourcePath && !snapshot->palSourcePath) ||
        (document->palSavePath && !snapshot->palSavePath)) {
        snapshot_destroy(snapshot);
        return NULL;
    }
    copy_history_label(snapshot->label, label);
    return snapshot;
}

static int snapshot_matches(const CadDocumentSnapshot* snapshot,
                            const CadDocument* document) {
    return snapshot && document &&
           core_state_matches(&snapshot->core, &document->core) &&
           memcmp(snapshot->colorData, document->colorData,
                  sizeof(snapshot->colorData)) == 0 &&
           memcmp(snapshot->paletteData, document->paletteData,
                  sizeof(snapshot->paletteData)) == 0 &&
           memcmp(snapshot->palette, document->palette,
                  sizeof(snapshot->palette)) == 0 &&
           snapshot->colorDataSize == document->colorDataSize &&
           snapshot->paletteDataSize == document->paletteDataSize &&
           snapshot->paletteValid == document->paletteValid;
}

static int snapshot_apply(CadDocument* document,
                          const CadDocumentSnapshot* snapshot) {
    if (!document || !snapshot) return 0;
    document->core = snapshot->core;
    memcpy(document->colorData, snapshot->colorData,
           sizeof(document->colorData));
    memcpy(document->paletteData, snapshot->paletteData,
           sizeof(document->paletteData));
    document->colorDataSize = snapshot->colorDataSize;
    document->paletteDataSize = snapshot->paletteDataSize;
    memcpy(document->palette, snapshot->palette, sizeof(document->palette));
    document->paletteValid = snapshot->paletteValid;
    document->revision = snapshot->revision;
    document->colRevision = snapshot->colRevision;
    document->palRevision = snapshot->palRevision;
    update_dirty_state(document);
    return 1;
}

/* Undo and redo deliberately retain the document's monotonic revision
   allocator.  A cancelled or rejected transaction is different: it must
   restore every bit of revision bookkeeping that existed before the edit. */
static void snapshot_rollback(CadDocument* document,
                               CadDocumentSnapshot* snapshot) {
    if (!document || !snapshot) return;
    document->core = snapshot->core;
    memcpy(document->colorData, snapshot->colorData,
           sizeof(document->colorData));
    memcpy(document->paletteData, snapshot->paletteData,
           sizeof(document->paletteData));
    document->colorDataSize = snapshot->colorDataSize;
    document->paletteDataSize = snapshot->paletteDataSize;
    memcpy(document->palette, snapshot->palette, sizeof(document->palette));
    document->paletteValid = snapshot->paletteValid;
    document->revision = snapshot->revision;
    document->nextRevision = snapshot->nextRevision;
    document->colRevision = snapshot->colRevision;
    document->nextColRevision = snapshot->nextColRevision;
    document->palRevision = snapshot->palRevision;
    document->nextPalRevision = snapshot->nextPalRevision;
    free(document->colSourcePath);
    free(document->colSavePath);
    free(document->palSourcePath);
    free(document->palSavePath);
    document->colSourcePath = snapshot->colSourcePath;
    document->colSavePath = snapshot->colSavePath;
    document->palSourcePath = snapshot->palSourcePath;
    document->palSavePath = snapshot->palSavePath;
    snapshot->colSourcePath = NULL;
    snapshot->colSavePath = NULL;
    snapshot->palSourcePath = NULL;
    snapshot->palSavePath = NULL;
    update_dirty_state(document);
}

static int history_append(CadDocument* document,
                          CadDocumentSnapshot* snapshot) {
    unsigned i;
    if (!document || !snapshot) return 0;
    while (document->historyCount > document->historyCursor + 1) {
        snapshot_destroy(document->history[document->historyCount - 1]);
        document->history[--document->historyCount] = NULL;
    }
    if (document->historyCount == CAD_DOCUMENT_HISTORY_LIMIT) {
        snapshot_destroy(document->history[0]);
        for (i = 1; i < document->historyCount; ++i)
            document->history[i - 1] = document->history[i];
        document->history[document->historyCount - 1] = NULL;
        document->historyCount--;
        if (document->historyCursor) document->historyCursor--;
    }
    document->history[document->historyCount++] = snapshot;
    document->historyCursor = document->historyCount - 1;
    return 1;
}

void CadDocument_Init(CadDocument* document) {
    CadDocumentSnapshot* initial;
    if (!document) return;
    memset(document, 0, sizeof(*document));
    CadCore_Init(&document->core);
    document->sourceFormat = CAD_FORMAT_AUTO;
    document->revision = 0;
    document->savedRevision = 0;
    document->nextRevision = 0;
    document->colRevision = 0;
    document->colSavedRevision = 0;
    document->nextColRevision = 0;
    document->palRevision = 0;
    document->palSavedRevision = 0;
    document->nextPalRevision = 0;
    initial = snapshot_create(document, "");
    if (initial) history_append(document, initial);
}

void CadDocument_Destroy(CadDocument* document) {
    unsigned i;
    if (!document) return;
    for (i = 0; i < document->historyCount; ++i) {
        snapshot_destroy(document->history[i]);
        document->history[i] = NULL;
    }
    document->historyCount = 0;
    snapshot_destroy(document->transactionBefore);
    document->transactionBefore = NULL;
    document->transactionLabel[0] = '\0';
    free(document->sourcePath);
    free(document->savePath);
    free(document->lastImportPath);
    free(document->lastExportPath);
    free(document->colSourcePath);
    free(document->colSavePath);
    free(document->palSourcePath);
    free(document->palSavePath);
    clear_original_native_bytes(document);
    document->sourcePath = document->savePath = NULL;
    document->lastImportPath = document->lastExportPath = NULL;
    document->colSourcePath = document->colSavePath = NULL;
    document->palSourcePath = document->palSavePath = NULL;
    CadCore_Destroy(&document->core);
    memset(document, 0, sizeof(*document));
}

void CadDocument_ClearHistory(CadDocument* document) {
    unsigned i;
    CadDocumentSnapshot* initial;
    if (!document) return;
    for (i = 0; i < document->historyCount; ++i) {
        snapshot_destroy(document->history[i]);
        document->history[i] = NULL;
    }
    document->historyCount = 0;
    document->historyCursor = 0;
    snapshot_destroy(document->transactionBefore);
    document->transactionBefore = NULL;
    document->transactionLabel[0] = '\0';
    initial = snapshot_create(document, "");
    if (initial) history_append(document, initial);
}

void CadDocument_New(CadDocument* document) {
    if (!document) return;
    CadCore_Clear(&document->core);
    memset(document->colorData, 0, sizeof(document->colorData));
    memset(document->paletteData, 0, sizeof(document->paletteData));
    document->colorDataSize = 0;
    document->paletteDataSize = 0;
    memset(document->palette, 0, sizeof(document->palette));
    document->paletteValid = 0;
    document->sourceFormat = CAD_FORMAT_AUTO;
    document->revision = 0;
    document->savedRevision = 0;
    document->nextRevision = 0;
    document->colRevision = 0;
    document->colSavedRevision = 0;
    document->nextColRevision = 0;
    document->palRevision = 0;
    document->palSavedRevision = 0;
    document->nextPalRevision = 0;
    update_dirty_state(document);
    free(document->sourcePath); document->sourcePath = NULL;
    free(document->savePath); document->savePath = NULL;
    free(document->lastImportPath); document->lastImportPath = NULL;
    free(document->lastExportPath); document->lastExportPath = NULL;
    free(document->colSourcePath); document->colSourcePath = NULL;
    free(document->colSavePath); document->colSavePath = NULL;
    free(document->palSourcePath); document->palSourcePath = NULL;
    free(document->palSavePath); document->palSavePath = NULL;
    clear_original_native_bytes(document);
    CadDocument_ClearHistory(document);
}

void CadDocument_MakeUnnamed(CadDocument* document) {
    if (!document) return;
    free(document->sourcePath); document->sourcePath = NULL;
    free(document->savePath); document->savePath = NULL;
    free(document->lastImportPath); document->lastImportPath = NULL;
    free(document->lastExportPath); document->lastExportPath = NULL;
    clear_original_native_bytes(document);
    document->sourceFormat = CAD_FORMAT_AUTO;
    document->savedRevision = CAD_DOCUMENT_NO_SAVED_REVISION;
    update_dirty_state(document);
}

CadResult CadDocument_Load(CadDocument* document, const char* utf8Path) {
    CadFileData* loaded;
    uint8_t* bytes = NULL;
    size_t byteCount = 0;
    CadResult result;
    char* source;
    char* save = NULL;
    char* importPath = NULL;
    uint8_t* originalNativeBytes = NULL;
    size_t originalNativeByteCount = 0;
    if (!document || !utf8Path)
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "Document load requires a path");
    if (document->transactionBefore)
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "Finish or cancel the active edit before loading");
    loaded = (CadFileData*)malloc(sizeof(*loaded));
    if (!loaded)
        return document_error(CAD_STATUS_OUT_OF_MEMORY,
                              "Could not allocate a temporary document");
    result = CadPlatform_ReadFile(utf8Path, CAD_PLATFORM_DEFAULT_FILE_LIMIT,
                                  &bytes, &byteCount);
    if (CadResult_IsSuccess(&result))
        result = CadCodec_Decode(bytes, byteCount, CAD_FORMAT_AUTO, loaded);
    if (!CadResult_IsSuccess(&result)) {
        CadPlatform_Free(bytes);
        free(loaded);
        return result;
    }
    if (result.format == CAD_FORMAT_X11_STREAM) {
        originalNativeBytes = bytes;
        originalNativeByteCount = byteCount;
        bytes = NULL;
    }
    CadPlatform_Free(bytes);
    source = duplicate_string(utf8Path);
    if (result.format == CAD_FORMAT_X11_STREAM)
        save = duplicate_string(utf8Path);
    else
        importPath = duplicate_string(utf8Path);
    if (!source || (result.format == CAD_FORMAT_X11_STREAM && !save) ||
        (result.format == CAD_FORMAT_LEGACY_PACKED && !importPath)) {
        free(source); free(save); free(importPath); free(loaded);
        CadPlatform_Free(originalNativeBytes);
        return document_error(CAD_STATUS_OUT_OF_MEMORY,
                              "Could not copy the document path");
    }

    CadCore_Clear(&document->core);
    document->core.data = *loaded;
    CadCore_RebuildDerivedState(&document->core);
    free(loaded);
    memset(document->colorData, 0, sizeof(document->colorData));
    memset(document->paletteData, 0, sizeof(document->paletteData));
    memset(document->palette, 0, sizeof(document->palette));
    document->colorDataSize = 0;
    document->paletteDataSize = 0;
    document->paletteValid = 0;
    free(document->sourcePath); document->sourcePath = source;
    free(document->savePath); document->savePath = save;
    free(document->lastImportPath); document->lastImportPath = importPath;
    free(document->lastExportPath); document->lastExportPath = NULL;
    free(document->colSourcePath); document->colSourcePath = NULL;
    free(document->colSavePath); document->colSavePath = NULL;
    free(document->palSourcePath); document->palSourcePath = NULL;
    free(document->palSavePath); document->palSavePath = NULL;
    clear_original_native_bytes(document);
    document->originalNativeBytes = originalNativeBytes;
    document->originalNativeByteCount = originalNativeByteCount;
    document->sourceFormat = result.format;
    document->revision = 0;
    document->nextRevision = 0;
    document->savedRevision = result.format == CAD_FORMAT_LEGACY_PACKED
        ? CAD_DOCUMENT_NO_SAVED_REVISION : 0;
    document->colRevision = 0;
    document->colSavedRevision = 0;
    document->nextColRevision = 0;
    document->palRevision = 0;
    document->palSavedRevision = 0;
    document->nextPalRevision = 0;
    update_dirty_state(document);
    CadDocument_ClearHistory(document);
    return result;
}

CadResult CadDocument_ImportAnm(CadDocument* document,
                                const char* utf8Path) {
    CadFileData* imported;
    uint8_t* bytes = NULL;
    size_t byteCount = 0;
    CadResult result;
    char* source;
    char* importPath;
    if (!document || !utf8Path)
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "ANM import requires a path");
    if (document->transactionBefore)
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "Finish or cancel the active edit before importing ANM");
    imported = (CadFileData*)malloc(sizeof(*imported));
    if (!imported)
        return document_error(CAD_STATUS_OUT_OF_MEMORY,
                              "Could not allocate a temporary ANM document");
    result = CadPlatform_ReadFile(utf8Path, CAD_PLATFORM_DEFAULT_FILE_LIMIT,
                                  &bytes, &byteCount);
    if (CadResult_IsSuccess(&result))
        result = CadAnmCodec_Decode(bytes, byteCount, CAD_FORMAT_AUTO,
                                    imported);
    CadPlatform_Free(bytes);
    if (!CadResult_IsSuccess(&result)) {
        free(imported);
        return result;
    }
    source = duplicate_string(utf8Path);
    importPath = duplicate_string(utf8Path);
    if (!source || !importPath) {
        free(source);
        free(importPath);
        free(imported);
        return document_error(CAD_STATUS_OUT_OF_MEMORY,
                              "Could not copy the ANM import path");
    }

    CadCore_Clear(&document->core);
    document->core.data = *imported;
    CadCore_RebuildDerivedState(&document->core);
    free(imported);
    memset(document->colorData, 0, sizeof(document->colorData));
    memset(document->paletteData, 0, sizeof(document->paletteData));
    memset(document->palette, 0, sizeof(document->palette));
    document->colorDataSize = 0;
    document->paletteDataSize = 0;
    document->paletteValid = 0;
    free(document->sourcePath); document->sourcePath = source;
    free(document->savePath); document->savePath = NULL;
    free(document->lastImportPath); document->lastImportPath = importPath;
    free(document->lastExportPath); document->lastExportPath = NULL;
    free(document->colSourcePath); document->colSourcePath = NULL;
    free(document->colSavePath); document->colSavePath = NULL;
    free(document->palSourcePath); document->palSourcePath = NULL;
    free(document->palSavePath); document->palSavePath = NULL;
    clear_original_native_bytes(document);
    document->sourceFormat = result.format;
    document->revision = 0;
    document->nextRevision = 0;
    document->savedRevision = CAD_DOCUMENT_NO_SAVED_REVISION;
    document->colRevision = 0;
    document->colSavedRevision = 0;
    document->nextColRevision = 0;
    document->palRevision = 0;
    document->palSavedRevision = 0;
    document->nextPalRevision = 0;
    update_dirty_state(document);
    CadDocument_ClearHistory(document);
    return result;
}

CadResult CadDocument_ValidateExportPath(const CadDocument* document,
                                         const char* utf8Path) {
    if (!document || !utf8Path)
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "Export requires a document and path");
    if (document->transactionBefore)
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "Finish or cancel the active edit before exporting");
    if (path_matches(utf8Path, document->lastImportPath) ||
        path_matches(utf8Path, document->sourcePath) ||
        path_matches(utf8Path, document->savePath) ||
        path_matches_palette_resource(document, utf8Path))
        return document_error(
            CAD_STATUS_INVALID_ARGUMENT,
            "Choose a different export path; open source and palette resources are never overwritten");
    return CadResult_Ok(CAD_FORMAT_AUTO);
}

CadResult CadDocument_ExportAnm(CadDocument* document,
                                const char* utf8Path,
                                CadFormat format) {
    uint8_t* bytes = NULL;
    size_t byteCount = 0;
    CadResult result;
    CadResult writeResult;
    char* pathCopy;
    result = CadDocument_ValidateExportPath(document, utf8Path);
    if (!CadResult_IsSuccess(&result)) return result;
    result = CadAnmCodec_Encode(&document->core.data, format,
                                &bytes, &byteCount);
    if (!CadResult_IsSuccess(&result)) return result;
    pathCopy = duplicate_string(utf8Path);
    if (!pathCopy) {
        CadCodec_FreeBuffer(bytes);
        return document_error(CAD_STATUS_OUT_OF_MEMORY,
                              "Could not copy the ANM export path");
    }
    writeResult = CadPlatform_WriteFileAtomic(utf8Path, bytes, byteCount);
    CadCodec_FreeBuffer(bytes);
    if (!CadResult_IsSuccess(&writeResult)) {
        free(pathCopy);
        return writeResult;
    }
    free(document->lastExportPath);
    document->lastExportPath = pathCopy;
    result.bytesConsumed = byteCount;
    return result;
}

CadResult CadDocument_Save(CadDocument* document, const char* utf8Path) {
    CadResult result;
    CadResult writeResult;
    uint8_t* bytes = NULL;
    size_t byteCount = 0;
    char* source;
    char* save;
    int encoded = 0;
    int originalMatch;
    if (!document || !utf8Path)
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "Document save requires a path");
    if (document->transactionBefore)
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "Finish or cancel the active edit before saving");
    if (path_matches(utf8Path, document->lastImportPath) ||
        path_matches(utf8Path, document->lastExportPath) ||
        path_matches_palette_resource(document, utf8Path))
        return document_error(
            CAD_STATUS_INVALID_ARGUMENT,
            "Choose a different native Save As path; import, export, and palette resources are never overwritten");
    source = duplicate_string(utf8Path);
    save = duplicate_string(utf8Path);
    if (!source || !save) {
        free(source); free(save);
        return document_error(CAD_STATUS_OUT_OF_MEMORY,
                              "Could not copy the save path");
    }
    originalMatch = document_matches_original_native(document);
    if (originalMatch < 0) {
        free(source); free(save);
        return document_error(CAD_STATUS_OUT_OF_MEMORY,
                              "Could not compare the native source before saving");
    }
    if (originalMatch) {
        result = CadCodec_Validate(&document->core.data);
        result.format = CAD_FORMAT_X11_STREAM;
        bytes = document->originalNativeBytes;
        byteCount = document->originalNativeByteCount;
    } else {
        result = CadCodec_Encode(&document->core.data, CAD_FORMAT_X11_STREAM,
                                 &bytes, &byteCount);
        encoded = 1;
    }
    if (CadResult_IsSuccess(&result)) {
        writeResult = CadPlatform_WriteFileAtomic(utf8Path, bytes, byteCount);
        if (!CadResult_IsSuccess(&writeResult)) result = writeResult;
        else result.bytesConsumed = byteCount;
    }
    if (encoded) CadCodec_FreeBuffer(bytes);
    if (!CadResult_IsSuccess(&result)) {
        free(source); free(save);
        return result;
    }
    free(document->sourcePath); document->sourcePath = source;
    free(document->savePath); document->savePath = save;
    document->sourceFormat = CAD_FORMAT_X11_STREAM;
    document->savedRevision = document->revision;
    update_dirty_state(document);
    return result;
}

CadResult CadDocument_SaveCurrent(CadDocument* document) {
    if (!document || !document->savePath)
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "Document has no native save path; Save As is required");
    return CadDocument_Save(document, document->savePath);
}

void CadDocument_MarkDirty(CadDocument* document) {
    if (!document) return;
    if (!document->isDirty) document->revision = ++document->nextRevision;
    update_dirty_state(document);
}

int CadDocument_HasAnimation(const CadDocument* document) {
    int i;
    if (!document) return 0;
    for (i = 0; i < CAD_MAX_ANIMATION_INDICES; ++i)
        if (document->core.data.animationIndices[i].flags) return 1;
    for (i = 0; i < CAD_MAX_ANIMATION_POINTS; ++i)
        if (document->core.data.animationPoints[i].flags) return 1;
    for (i = 0; i < CAD_MAX_POLYGONS; ++i)
        if (document->core.data.polygons[i].flags &&
            document->core.data.polygons[i].animation != -1) return 1;
    return 0;
}

CadResult CadDocument_BeginEditNamed(CadDocument* document,
                                     const char* label) {
    if (!document)
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "Cannot begin an edit on a NULL document");
    if (document->transactionBefore)
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "A document edit is already active");
    document->transactionBefore = snapshot_create(
        document, current_history_label(document));
    if (!document->transactionBefore)
        return document_error(CAD_STATUS_OUT_OF_MEMORY,
                              "Could not capture the edit snapshot");
    copy_history_label(document->transactionLabel,
                       label && label[0] ? label : "Edit");
    return CadResult_Ok(document->sourceFormat);
}

CadResult CadDocument_BeginEdit(CadDocument* document) {
    return CadDocument_BeginEditNamed(document, "Edit");
}

CadResult CadDocument_ApplyEdit(CadDocument* document, const char* label,
                                CadDocumentEditCallback callback,
                                void* userData) {
    CadResult result;
    if (!callback)
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "A composable document edit requires a callback");
    result = CadDocument_BeginEditNamed(document, label);
    if (!CadResult_IsSuccess(&result)) return result;
    result = callback(document, userData);
    if (!CadResult_IsSuccess(&result)) {
        CadDocument_CancelEdit(document);
        return result;
    }
    return CadDocument_CommitEdit(document);
}

CadResult CadDocument_CommitEdit(CadDocument* document) {
    CadDocumentSnapshot* current;
    CadResult validation;
    int nativeContentChanged;
    int colContentChanged;
    int palContentChanged;
    if (!document || !document->transactionBefore)
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "No document edit is active");
    if (snapshot_matches(document->transactionBefore, document)) {
        snapshot_destroy(document->transactionBefore);
        document->transactionBefore = NULL;
        document->transactionLabel[0] = '\0';
        update_dirty_state(document);
        return CadResult_Ok(document->sourceFormat);
    }
    validation = CadCodec_Validate(&document->core.data);
    if (!CadResult_IsSuccess(&validation)) {
        snapshot_rollback(document, document->transactionBefore);
        snapshot_destroy(document->transactionBefore);
        document->transactionBefore = NULL;
        document->transactionLabel[0] = '\0';
        return validation;
    }
    nativeContentChanged = memcmp(&document->transactionBefore->core.data,
                                  &document->core.data,
                                  sizeof(document->core.data)) != 0;
    colContentChanged =
        document->transactionBefore->colorDataSize !=
            document->colorDataSize ||
        document->transactionBefore->paletteValid !=
            document->paletteValid ||
        memcmp(document->transactionBefore->colorData,
               document->colorData, sizeof(document->colorData)) != 0 ||
        memcmp(document->transactionBefore->palette,
               document->palette, sizeof(document->palette)) != 0;
    palContentChanged =
        document->transactionBefore->paletteDataSize !=
            document->paletteDataSize ||
        memcmp(document->transactionBefore->paletteData,
               document->paletteData,
               sizeof(document->paletteData)) != 0;
    if (nativeContentChanged)
        document->revision = ++document->nextRevision;
    else
        document->revision = document->transactionBefore->revision;
    if (colContentChanged)
        document->colRevision = ++document->nextColRevision;
    else
        document->colRevision = document->transactionBefore->colRevision;
    if (palContentChanged)
        document->palRevision = ++document->nextPalRevision;
    else
        document->palRevision = document->transactionBefore->palRevision;
    update_dirty_state(document);
    current = snapshot_create(document, document->transactionLabel);
    if (!current) {
        snapshot_rollback(document, document->transactionBefore);
        snapshot_destroy(document->transactionBefore);
        document->transactionBefore = NULL;
        document->transactionLabel[0] = '\0';
        return document_error(CAD_STATUS_OUT_OF_MEMORY,
                              "Could not commit the edit snapshot");
    }
    while (document->historyCount > document->historyCursor + 1) {
        snapshot_destroy(document->history[document->historyCount - 1]);
        document->history[--document->historyCount] = NULL;
    }
    if (document->historyCount) {
        snapshot_destroy(document->history[document->historyCursor]);
        document->history[document->historyCursor] =
            document->transactionBefore;
    } else {
        history_append(document, document->transactionBefore);
    }
    document->transactionBefore = NULL;
    document->transactionLabel[0] = '\0';
    history_append(document, current);
    validation.format = document->sourceFormat;
    return validation;
}

void CadDocument_CancelEdit(CadDocument* document) {
    if (!document || !document->transactionBefore) return;
    snapshot_rollback(document, document->transactionBefore);
    snapshot_destroy(document->transactionBefore);
    document->transactionBefore = NULL;
    document->transactionLabel[0] = '\0';
}

int CadDocument_CanUndo(const CadDocument* document) {
    return document && !document->transactionBefore &&
           document->historyCount && document->historyCursor > 0;
}

int CadDocument_CanRedo(const CadDocument* document) {
    return document && !document->transactionBefore &&
           document->historyCursor + 1 < document->historyCount;
}

CadResult CadDocument_Undo(CadDocument* document) {
    unsigned target;
    if (!CadDocument_CanUndo(document))
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "There is no edit to undo");
    target = document->historyCursor - 1;
    if (!snapshot_apply(document, document->history[target]))
        return document_error(CAD_STATUS_OUT_OF_MEMORY,
                              "Could not restore the undo snapshot");
    document->historyCursor = target;
    return CadResult_Ok(document->sourceFormat);
}

CadResult CadDocument_Redo(CadDocument* document) {
    unsigned target;
    if (!CadDocument_CanRedo(document))
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "There is no edit to redo");
    target = document->historyCursor + 1;
    if (!snapshot_apply(document, document->history[target]))
        return document_error(CAD_STATUS_OUT_OF_MEMORY,
                              "Could not restore the redo snapshot");
    document->historyCursor = target;
    return CadResult_Ok(document->sourceFormat);
}

const char* CadDocument_GetUndoLabel(const CadDocument* document) {
    if (!CadDocument_CanUndo(document)) return NULL;
    return document->history[document->historyCursor]->label;
}

const char* CadDocument_GetRedoLabel(const CadDocument* document) {
    if (!CadDocument_CanRedo(document)) return NULL;
    return document->history[document->historyCursor + 1]->label;
}

int CadDocument_SetLastImportPath(CadDocument* document,
                                  const char* utf8Path) {
    return document && replace_string(&document->lastImportPath, utf8Path);
}

int CadDocument_SetLastExportPath(CadDocument* document,
                                  const char* utf8Path) {
    return document && replace_string(&document->lastExportPath, utf8Path);
}

static void document_install_col(CadDocument* document,
                                 const CadPaletteFile* palette) {
    unsigned index;
    if (!document || !palette) return;
    memset(document->colorData, 0, sizeof(document->colorData));
    for (index = 0; index < CAD_PALETTE_ENTRY_COUNT; ++index) {
        const uint16_t word = palette->colWords[index];
        const CadPaletteRgba8 rgba = CadPalette_Bgr555ToRgba8(word);
        document->colorData[index * 2u] = (uint8_t)(word & 0xffu);
        document->colorData[index * 2u + 1u] = (uint8_t)(word >> 8);
        document->palette[index].r = rgba.r;
        document->palette[index].g = rgba.g;
        document->palette[index].b = rgba.b;
        document->palette[index].a = rgba.a;
    }
    document->colorDataSize = CAD_COL_FILE_SIZE;
    document->paletteValid = 1;
}

static void document_install_pal(CadDocument* document,
                                 const CadPaletteFile* palette) {
    unsigned recordIndex;
    if (!document || !palette) return;
    memset(document->paletteData, 0, sizeof(document->paletteData));
    for (recordIndex = 0; recordIndex < CAD_PAL_RECORD_COUNT;
         ++recordIndex) {
        const CadPalRecord* record = &palette->palRecords[recordIndex];
        const size_t descriptor = (size_t)recordIndex * 2u;
        const size_t payload = CAD_PAL_DESCRIPTOR_SIZE +
            (size_t)recordIndex * CAD_PAL_INDEX_COUNT;
        document->paletteData[descriptor] = record->type;
        document->paletteData[descriptor + 1u] = (uint8_t)(
            ((record->paletteNumber - 1u) << 4) |
            (record->colorCount - 1u));
        memcpy(document->paletteData + payload, record->indices,
               CAD_PAL_INDEX_COUNT);
    }
    document->paletteDataSize = CAD_PAL_FILE_SIZE;
}

static CadResult document_extract_palette(const CadDocument* document,
                                          CadPaletteFormat format,
                                          CadPaletteFile* output) {
    if (!document || !output)
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "Palette extraction requires a document and output");
    if (format == CAD_PALETTE_FORMAT_COL) {
        if (document->colorDataSize != CAD_COL_FILE_SIZE)
            return document_error(CAD_STATUS_INVALID_ARGUMENT,
                                  "No COL palette is loaded or created");
        return CadPalette_Decode(document->colorData,
                                 document->colorDataSize,
                                 CAD_PALETTE_FORMAT_COL, output);
    }
    if (format == CAD_PALETTE_FORMAT_PAL) {
        if (document->paletteDataSize != CAD_PAL_FILE_SIZE)
            return document_error(CAD_STATUS_INVALID_ARGUMENT,
                                  "No PAL material map is loaded or created");
        return CadPalette_Decode(document->paletteData,
                                 document->paletteDataSize,
                                 CAD_PALETTE_FORMAT_PAL, output);
    }
    return document_error(CAD_STATUS_UNSUPPORTED_FORMAT,
                          "Choose COL or PAL for this palette operation");
}

CadResult CadDocument_NewPalette(CadDocument* document,
                                 CadPaletteFormat format) {
    CadPaletteFile palette = {0};
    CadResult result;
    if (!document)
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "Palette creation requires a document");
    result = CadPalette_Create(format, &palette);
    if (!CadResult_IsSuccess(&result)) return result;
    format = palette.format;
    if (format == CAD_PALETTE_FORMAT_COL) {
        /* A compact 3-3-2 RGB cube gives a new editor immediate, useful
           coverage while remaining deterministic and fully BGR555. */
        unsigned index;
        for (index = 0; index < CAD_PALETTE_ENTRY_COUNT; ++index) {
            const unsigned red = ((index >> 5) & 7u) * 31u / 7u;
            const unsigned green = ((index >> 2) & 7u) * 31u / 7u;
            const unsigned blue = (index & 3u) * 31u / 3u;
            palette.colWords[index] = (uint16_t)(
                red | (green << 5) | (blue << 10));
        }
    }
    result = CadDocument_BeginEditNamed(
        document, format == CAD_PALETTE_FORMAT_COL
                      ? "New COL Palette" : "New PAL Material Map");
    if (!CadResult_IsSuccess(&result)) return result;
    if (format == CAD_PALETTE_FORMAT_COL)
        document_install_col(document, &palette);
    else
        document_install_pal(document, &palette);
    result = CadDocument_CommitEdit(document);
    if (!CadResult_IsSuccess(&result)) return result;
    if (format == CAD_PALETTE_FORMAT_COL) {
        free(document->colSourcePath); document->colSourcePath = NULL;
        free(document->colSavePath); document->colSavePath = NULL;
        document->colSavedRevision = CAD_DOCUMENT_NO_SAVED_REVISION;
    } else {
        free(document->palSourcePath); document->palSourcePath = NULL;
        free(document->palSavePath); document->palSavePath = NULL;
        document->palSavedRevision = CAD_DOCUMENT_NO_SAVED_REVISION;
    }
    update_dirty_state(document);
    return result;
}

CadResult CadDocument_OpenPalette(CadDocument* document,
                                  const char* utf8Path,
                                  CadPaletteFormat format) {
    CadPaletteFile palette = {0};
    CadResult result;
    CadResult commitResult;
    uint8_t* bytes = NULL;
    size_t size = 0;
    char* source;
    char* save;
    if (!document || !utf8Path)
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "Palette open requires a document and path");
    if (document->transactionBefore)
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "Finish or cancel the active edit before opening a palette");
    if (path_matches_model_resource(document, utf8Path))
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "A model, import, or export path cannot also be opened as a palette resource");
    result = CadPlatform_ReadFile(utf8Path, CAD_PLATFORM_DEFAULT_FILE_LIMIT,
                                  &bytes, &size);
    if (CadResult_IsSuccess(&result)) {
        const CadPaletteFormat requestedFormat = format;
        result = CadPalette_Decode(bytes, size, CAD_PALETTE_FORMAT_AUTO,
                                   &palette);
        if (CadResult_IsSuccess(&result) &&
            requestedFormat != CAD_PALETTE_FORMAT_AUTO &&
            palette.format != requestedFormat) {
            result = document_error(
                CAD_STATUS_UNSUPPORTED_FORMAT,
                requestedFormat == CAD_PALETTE_FORMAT_COL
                    ? "The selected file is a PAL material map, not a COL color palette"
                    : "The selected file is a COL color palette, not a PAL material map");
        }
    }
    CadPlatform_Free(bytes);
    if (!CadResult_IsSuccess(&result)) return result;
    format = palette.format;
    if ((format == CAD_PALETTE_FORMAT_COL &&
         (path_matches(utf8Path, document->palSourcePath) ||
          path_matches(utf8Path, document->palSavePath))) ||
        (format == CAD_PALETTE_FORMAT_PAL &&
         (path_matches(utf8Path, document->colSourcePath) ||
          path_matches(utf8Path, document->colSavePath))))
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "COL and PAL resources must use different paths");
    source = duplicate_string(utf8Path);
    save = duplicate_string(utf8Path);
    if (!source || !save) {
        free(source); free(save);
        return document_error(CAD_STATUS_OUT_OF_MEMORY,
                              "Could not retain the palette path");
    }
    commitResult = CadDocument_BeginEditNamed(
        document, format == CAD_PALETTE_FORMAT_COL
                      ? "Open COL Palette" : "Open PAL Material Map");
    if (CadResult_IsSuccess(&commitResult)) {
        if (format == CAD_PALETTE_FORMAT_COL)
            document_install_col(document, &palette);
        else
            document_install_pal(document, &palette);
        commitResult = CadDocument_CommitEdit(document);
    }
    if (!CadResult_IsSuccess(&commitResult)) {
        free(source); free(save);
        return commitResult;
    }
    if (format == CAD_PALETTE_FORMAT_COL) {
        free(document->colSourcePath); document->colSourcePath = source;
        free(document->colSavePath); document->colSavePath = save;
        document->colSavedRevision = document->colRevision;
    } else {
        free(document->palSourcePath); document->palSourcePath = source;
        free(document->palSavePath); document->palSavePath = save;
        document->palSavedRevision = document->palRevision;
    }
    update_dirty_state(document);
    return result;
}

CadResult CadDocument_SavePalette(CadDocument* document,
                                  const char* utf8Path,
                                  CadPaletteFormat format) {
    CadPaletteFile palette = {0};
    CadResult result;
    CadResult writeResult;
    uint8_t* bytes = NULL;
    size_t size = 0;
    char* source;
    char* save;
    if (!document || !utf8Path)
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "Palette save requires a document and path");
    if (document->transactionBefore)
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "Finish or cancel the active edit before saving a palette");
    if (format != CAD_PALETTE_FORMAT_COL &&
        format != CAD_PALETTE_FORMAT_PAL)
        return document_error(CAD_STATUS_UNSUPPORTED_FORMAT,
                              "Palette save requires COL or PAL");
    if (path_matches_model_resource(document, utf8Path))
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "Choose a different palette path; model, import, and export resources are never overwritten");
    if (format == CAD_PALETTE_FORMAT_COL &&
        ((document->palSourcePath &&
          CadPlatform_PathsEqual(utf8Path, document->palSourcePath)) ||
         (document->palSavePath &&
          CadPlatform_PathsEqual(utf8Path, document->palSavePath))))
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "A COL save cannot overwrite the PAL resource");
    if (format == CAD_PALETTE_FORMAT_PAL &&
        ((document->colSourcePath &&
          CadPlatform_PathsEqual(utf8Path, document->colSourcePath)) ||
         (document->colSavePath &&
          CadPlatform_PathsEqual(utf8Path, document->colSavePath))))
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "A PAL save cannot overwrite the COL resource");
    result = document_extract_palette(document, format, &palette);
    if (CadResult_IsSuccess(&result))
        result = CadPalette_Encode(&palette, format, &bytes, &size);
    if (!CadResult_IsSuccess(&result)) return result;
    source = duplicate_string(utf8Path);
    save = duplicate_string(utf8Path);
    if (!source || !save) {
        free(source); free(save);
        CadPalette_FreeBuffer(bytes);
        return document_error(CAD_STATUS_OUT_OF_MEMORY,
                              "Could not retain the palette save path");
    }
    writeResult = CadPlatform_WriteFileAtomic(utf8Path, bytes, size);
    CadPalette_FreeBuffer(bytes);
    if (!CadResult_IsSuccess(&writeResult)) {
        free(source); free(save);
        return writeResult;
    }
    if (format == CAD_PALETTE_FORMAT_COL) {
        free(document->colSourcePath); document->colSourcePath = source;
        free(document->colSavePath); document->colSavePath = save;
        document->colSavedRevision = document->colRevision;
    } else {
        free(document->palSourcePath); document->palSourcePath = source;
        free(document->palSavePath); document->palSavePath = save;
        document->palSavedRevision = document->palRevision;
    }
    update_dirty_state(document);
    result.bytesConsumed = size;
    return result;
}

CadResult CadDocument_SavePaletteCurrent(CadDocument* document,
                                         CadPaletteFormat format) {
    const char* path;
    if (!document)
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "Palette save requires a document");
    path = format == CAD_PALETTE_FORMAT_COL ? document->colSavePath
         : format == CAD_PALETTE_FORMAT_PAL ? document->palSavePath : NULL;
    if (!path)
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "This palette resource requires Save As");
    return CadDocument_SavePalette(document, path, format);
}

int CadDocument_HasPalette(const CadDocument* document,
                           CadPaletteFormat format) {
    if (!document) return 0;
    if (format == CAD_PALETTE_FORMAT_COL)
        return document->paletteValid &&
               document->colorDataSize == CAD_COL_FILE_SIZE;
    if (format == CAD_PALETTE_FORMAT_PAL)
        return document->paletteDataSize == CAD_PAL_FILE_SIZE;
    if (format == CAD_PALETTE_FORMAT_AUTO)
        return CadDocument_HasPalette(document, CAD_PALETTE_FORMAT_COL) ||
               CadDocument_HasPalette(document, CAD_PALETTE_FORMAT_PAL);
    return 0;
}

int CadDocument_HasUnsavedPaletteChanges(const CadDocument* document) {
    return document && (document->colDirty || document->palDirty);
}

uint16_t CadDocument_GetColWord(const CadDocument* document, unsigned index) {
    if (!CadDocument_HasPalette(document, CAD_PALETTE_FORMAT_COL) ||
        index >= CAD_PALETTE_ENTRY_COUNT) return 0;
    return (uint16_t)((uint16_t)document->colorData[index * 2u] |
                      ((uint16_t)document->colorData[index * 2u + 1u] << 8));
}

CadResult CadDocument_SetColWord(CadDocument* document, unsigned index,
                                 uint16_t bgr555) {
    CadResult result;
    CadPaletteRgba8 rgba;
    if (!CadDocument_HasPalette(document, CAD_PALETTE_FORMAT_COL))
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "Create or open a COL palette before editing colors");
    if (index >= CAD_PALETTE_ENTRY_COUNT)
        return document_error(CAD_STATUS_INDEX_OUT_OF_RANGE,
                              "COL color index is out of range");
    result = CadDocument_BeginEditNamed(document, "Edit COL Color");
    if (!CadResult_IsSuccess(&result)) return result;
    document->colorData[index * 2u] = (uint8_t)(bgr555 & 0xffu);
    document->colorData[index * 2u + 1u] = (uint8_t)(bgr555 >> 8);
    rgba = CadPalette_Bgr555ToRgba8(bgr555);
    document->palette[index].r = rgba.r;
    document->palette[index].g = rgba.g;
    document->palette[index].b = rgba.b;
    document->palette[index].a = rgba.a;
    return CadDocument_CommitEdit(document);
}

int CadDocument_GetPalDescriptor(const CadDocument* document, unsigned index,
                                 uint8_t* type, uint8_t* paletteNumber,
                                 uint8_t* colorCount) {
    uint8_t packed;
    if (!CadDocument_HasPalette(document, CAD_PALETTE_FORMAT_PAL) ||
        index >= CAD_PAL_RECORD_COUNT) return 0;
    packed = document->paletteData[index * 2u + 1u];
    if (type) *type = document->paletteData[index * 2u];
    if (paletteNumber) *paletteNumber = (uint8_t)((packed >> 4) + 1u);
    if (colorCount) *colorCount = (uint8_t)((packed & 0x0fu) + 1u);
    return 1;
}

CadResult CadDocument_SetPalDescriptor(CadDocument* document, unsigned index,
                                       uint8_t type, uint8_t paletteNumber,
                                       uint8_t colorCount) {
    CadResult result;
    if (!CadDocument_HasPalette(document, CAD_PALETTE_FORMAT_PAL))
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "Create or open a PAL map before editing materials");
    if (index >= CAD_PAL_RECORD_COUNT)
        return document_error(CAD_STATUS_INDEX_OUT_OF_RANGE,
                              "PAL material index is out of range");
    if (paletteNumber < 1 || paletteNumber > 16 ||
        colorCount < 1 || colorCount > 16)
        return document_error(CAD_STATUS_INVALID_NUMBER,
                              "PAL palette number and color count must be 1-16");
    result = CadDocument_BeginEditNamed(document, "Edit PAL Material");
    if (!CadResult_IsSuccess(&result)) return result;
    document->paletteData[index * 2u] = type;
    document->paletteData[index * 2u + 1u] = (uint8_t)(
        ((paletteNumber - 1u) << 4) | (colorCount - 1u));
    return CadDocument_CommitEdit(document);
}

int CadDocument_GetPalSample(const CadDocument* document, unsigned material,
                             unsigned sample, uint8_t* colorIndex) {
    size_t offset;
    if (!CadDocument_HasPalette(document, CAD_PALETTE_FORMAT_PAL) ||
        material >= CAD_PAL_RECORD_COUNT || sample >= CAD_PAL_INDEX_COUNT)
        return 0;
    offset = CAD_PAL_DESCRIPTOR_SIZE +
             (size_t)material * CAD_PAL_INDEX_COUNT + sample;
    if (colorIndex) *colorIndex = document->paletteData[offset];
    return 1;
}

CadResult CadDocument_SetPalSample(CadDocument* document, unsigned material,
                                   unsigned sample, uint8_t colorIndex) {
    CadResult result;
    size_t offset;
    if (!CadDocument_HasPalette(document, CAD_PALETTE_FORMAT_PAL))
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "Create or open a PAL map before editing samples");
    if (material >= CAD_PAL_RECORD_COUNT || sample >= CAD_PAL_INDEX_COUNT)
        return document_error(CAD_STATUS_INDEX_OUT_OF_RANGE,
                              "PAL material or sample index is out of range");
    result = CadDocument_BeginEditNamed(document, "Edit PAL Sample");
    if (!CadResult_IsSuccess(&result)) return result;
    offset = CAD_PAL_DESCRIPTOR_SIZE +
             (size_t)material * CAD_PAL_INDEX_COUNT + sample;
    document->paletteData[offset] = colorIndex;
    return CadDocument_CommitEdit(document);
}

int CadDocument_ResolvePaletteColor(const CadDocument* document,
                                    unsigned material, unsigned sample,
                                    CadRgba* output) {
    unsigned colorIndex = material;
    uint8_t mapped;
    if (!output || material >= CAD_PALETTE_ENTRY_COUNT ||
        !CadDocument_HasPalette(document, CAD_PALETTE_FORMAT_COL)) return 0;
    if (CadDocument_HasPalette(document, CAD_PALETTE_FORMAT_PAL) &&
        CadDocument_GetPalSample(document, material,
                                 sample % CAD_PAL_INDEX_COUNT, &mapped))
        colorIndex = mapped;
    *output = document->palette[colorIndex];
    return 1;
}

int CadDocument_SetPalette(CadDocument* document,
                           const CadRgba entries[256],
                           const char* utf8SourcePath) {
    CadPaletteFile palette;
    CadResult result;
    char* source;
    char* save;
    int ownsTransaction;
    unsigned index;
    if (!document || !entries) return 0;
    source = duplicate_string(utf8SourcePath);
    save = duplicate_string(utf8SourcePath);
    if (utf8SourcePath && (!source || !save)) {
        free(source); free(save);
        return 0;
    }
    memset(&palette, 0, sizeof(palette));
    palette.format = CAD_PALETTE_FORMAT_COL;
    for (index = 0; index < CAD_PALETTE_ENTRY_COUNT; ++index) {
        CadPaletteRgba8 rgba = { entries[index].r, entries[index].g,
                                 entries[index].b, entries[index].a };
        palette.colWords[index] = CadPalette_Rgba8ToBgr555(rgba);
    }
    ownsTransaction = document->transactionBefore == NULL;
    if (ownsTransaction) {
        result = CadDocument_BeginEditNamed(document, "Install COL Palette");
        if (!CadResult_IsSuccess(&result)) {
            free(source); free(save);
            return 0;
        }
    }
    /* Install from the quantized words so the preview always matches what a
       save/reopen cycle will display. */
    document_install_col(document, &palette);
    free(document->colSourcePath);
    free(document->colSavePath);
    document->colSourcePath = source;
    document->colSavePath = save;
    if (ownsTransaction) {
        result = CadDocument_CommitEdit(document);
        if (!CadResult_IsSuccess(&result)) return 0;
    }
    return 1;
}

void CadDocument_ClearPalette(CadDocument* document) {
    CadResult result;
    int ownsTransaction;
    if (!document) return;
    ownsTransaction = document->transactionBefore == NULL;
    if (ownsTransaction) {
        result = CadDocument_BeginEditNamed(document, "Clear COL Palette");
        if (!CadResult_IsSuccess(&result)) return;
    }
    memset(document->colorData, 0, sizeof(document->colorData));
    document->colorDataSize = 0;
    memset(document->palette, 0, sizeof(document->palette));
    document->paletteValid = 0;
    free(document->colSourcePath); document->colSourcePath = NULL;
    free(document->colSavePath); document->colSavePath = NULL;
    if (ownsTransaction) (void)CadDocument_CommitEdit(document);
}
