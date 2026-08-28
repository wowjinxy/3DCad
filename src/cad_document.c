#include "cad_document.h"
#include "cad_anm_codec.h"
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
    char* paletteSourcePath;
    uint64_t revision;
    uint64_t nextRevision;
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

static int strings_equal(const char* left, const char* right) {
    if (left == right) return 1;
    if (!left || !right) return 0;
    return strcmp(left, right) == 0;
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
    free(snapshot->paletteSourcePath);
    free(snapshot);
}

static void update_dirty_state(CadDocument* document) {
    if (!document) return;
    document->isDirty = document->revision != document->savedRevision;
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
    snapshot->paletteSourcePath =
        duplicate_string(document->paletteSourcePath);
    if (document->paletteSourcePath && !snapshot->paletteSourcePath) {
        free(snapshot);
        return NULL;
    }
    snapshot->revision = document->revision;
    snapshot->nextRevision = document->nextRevision;
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
           snapshot->paletteValid == document->paletteValid &&
           strings_equal(snapshot->paletteSourcePath,
                         document->paletteSourcePath);
}

static int snapshot_apply(CadDocument* document,
                          const CadDocumentSnapshot* snapshot) {
    char* paletteSourcePath;
    if (!document || !snapshot) return 0;
    paletteSourcePath = duplicate_string(snapshot->paletteSourcePath);
    if (snapshot->paletteSourcePath && !paletteSourcePath) return 0;
    document->core = snapshot->core;
    memcpy(document->colorData, snapshot->colorData,
           sizeof(document->colorData));
    memcpy(document->paletteData, snapshot->paletteData,
           sizeof(document->paletteData));
    document->colorDataSize = snapshot->colorDataSize;
    document->paletteDataSize = snapshot->paletteDataSize;
    memcpy(document->palette, snapshot->palette, sizeof(document->palette));
    document->paletteValid = snapshot->paletteValid;
    free(document->paletteSourcePath);
    document->paletteSourcePath = paletteSourcePath;
    document->revision = snapshot->revision;
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
    free(document->paletteSourcePath);
    document->paletteSourcePath = snapshot->paletteSourcePath;
    snapshot->paletteSourcePath = NULL;
    document->revision = snapshot->revision;
    document->nextRevision = snapshot->nextRevision;
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
    free(document->paletteSourcePath);
    clear_original_native_bytes(document);
    document->sourcePath = document->savePath = NULL;
    document->lastImportPath = document->lastExportPath = NULL;
    document->paletteSourcePath = NULL;
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
    update_dirty_state(document);
    free(document->sourcePath); document->sourcePath = NULL;
    free(document->savePath); document->savePath = NULL;
    free(document->lastImportPath); document->lastImportPath = NULL;
    free(document->lastExportPath); document->lastExportPath = NULL;
    free(document->paletteSourcePath); document->paletteSourcePath = NULL;
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
    free(document->paletteSourcePath); document->paletteSourcePath = NULL;
    clear_original_native_bytes(document);
    document->originalNativeBytes = originalNativeBytes;
    document->originalNativeByteCount = originalNativeByteCount;
    document->sourceFormat = result.format;
    document->revision = 0;
    document->nextRevision = 0;
    document->savedRevision = result.format == CAD_FORMAT_LEGACY_PACKED
        ? CAD_DOCUMENT_NO_SAVED_REVISION : 0;
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
    free(document->paletteSourcePath); document->paletteSourcePath = NULL;
    clear_original_native_bytes(document);
    document->sourceFormat = result.format;
    document->revision = 0;
    document->nextRevision = 0;
    document->savedRevision = CAD_DOCUMENT_NO_SAVED_REVISION;
    update_dirty_state(document);
    CadDocument_ClearHistory(document);
    return result;
}

CadResult CadDocument_ExportAnm(CadDocument* document,
                                const char* utf8Path,
                                CadFormat format) {
    uint8_t* bytes = NULL;
    size_t byteCount = 0;
    CadResult result;
    CadResult writeResult;
    char* pathCopy;
    if (!document || !utf8Path)
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "ANM export requires a path");
    if (document->transactionBefore)
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "Finish or cancel the active edit before exporting");
    if (document->lastImportPath &&
        CadPlatform_PathsEqual(utf8Path, document->lastImportPath))
        return document_error(
            CAD_STATUS_INVALID_ARGUMENT,
            "Choose a different export path; imported source files are never overwritten");
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
    if (document->lastImportPath &&
        CadPlatform_PathsEqual(utf8Path, document->lastImportPath))
        return document_error(
            CAD_STATUS_INVALID_ARGUMENT,
            "Choose a different native Save As path; imported source files are never overwritten");
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
    if (nativeContentChanged)
        document->revision = ++document->nextRevision;
    else
        document->revision = document->transactionBefore->revision;
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

int CadDocument_SetPalette(CadDocument* document,
                           const CadRgba entries[256],
                           const char* utf8SourcePath) {
    char* source;
    if (!document || !entries) return 0;
    source = duplicate_string(utf8SourcePath);
    if (utf8SourcePath && !source) return 0;
    memcpy(document->palette, entries, sizeof(document->palette));
    document->paletteValid = 1;
    free(document->paletteSourcePath);
    document->paletteSourcePath = source;
    return 1;
}

void CadDocument_ClearPalette(CadDocument* document) {
    if (!document) return;
    memset(document->palette, 0, sizeof(document->palette));
    document->paletteValid = 0;
    free(document->paletteSourcePath);
    document->paletteSourcePath = NULL;
}
