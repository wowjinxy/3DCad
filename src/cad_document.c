#include "cad_document.h"

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

static CadDocumentSnapshot* snapshot_create(const CadDocument* document) {
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

static void snapshot_apply(CadDocument* document,
                           const CadDocumentSnapshot* snapshot) {
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
    update_dirty_state(document);
}

static int history_append(CadDocument* document,
                          CadDocumentSnapshot* snapshot) {
    unsigned i;
    if (!document || !snapshot) return 0;
    while (document->historyCount > document->historyCursor + 1) {
        free(document->history[document->historyCount - 1]);
        document->history[--document->historyCount] = NULL;
    }
    if (document->historyCount == CAD_DOCUMENT_HISTORY_LIMIT) {
        free(document->history[0]);
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
    initial = snapshot_create(document);
    if (initial) history_append(document, initial);
}

void CadDocument_Destroy(CadDocument* document) {
    unsigned i;
    if (!document) return;
    for (i = 0; i < document->historyCount; ++i) {
        free(document->history[i]);
        document->history[i] = NULL;
    }
    document->historyCount = 0;
    free(document->transactionBefore);
    document->transactionBefore = NULL;
    free(document->sourcePath);
    free(document->savePath);
    free(document->lastImportPath);
    free(document->lastExportPath);
    free(document->paletteSourcePath);
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
        free(document->history[i]);
        document->history[i] = NULL;
    }
    document->historyCount = 0;
    document->historyCursor = 0;
    free(document->transactionBefore);
    document->transactionBefore = NULL;
    initial = snapshot_create(document);
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
    CadDocument_ClearHistory(document);
}

CadResult CadDocument_Load(CadDocument* document, const char* utf8Path) {
    CadFileData* loaded;
    CadResult result;
    char* source;
    char* save = NULL;
    char* importPath = NULL;
    if (!document || !utf8Path)
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "Document load requires a path");
    loaded = (CadFileData*)malloc(sizeof(*loaded));
    if (!loaded)
        return document_error(CAD_STATUS_OUT_OF_MEMORY,
                              "Could not allocate a temporary document");
    result = CadCodec_LoadPath(utf8Path, CAD_FORMAT_AUTO, loaded);
    if (!CadResult_IsSuccess(&result)) {
        free(loaded);
        return result;
    }
    source = duplicate_string(utf8Path);
    if (result.format == CAD_FORMAT_X11_STREAM)
        save = duplicate_string(utf8Path);
    else
        importPath = duplicate_string(utf8Path);
    if (!source || (result.format == CAD_FORMAT_X11_STREAM && !save) ||
        (result.format == CAD_FORMAT_LEGACY_PACKED && !importPath)) {
        free(source); free(save); free(importPath); free(loaded);
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
    document->sourceFormat = result.format;
    document->revision = 0;
    document->nextRevision = 0;
    document->savedRevision = result.format == CAD_FORMAT_LEGACY_PACKED
        ? CAD_DOCUMENT_NO_SAVED_REVISION : 0;
    update_dirty_state(document);
    CadDocument_ClearHistory(document);
    return result;
}

CadResult CadDocument_Save(CadDocument* document, const char* utf8Path) {
    CadResult result;
    char* source;
    char* save;
    if (!document || !utf8Path)
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "Document save requires a path");
    if (document->transactionBefore)
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "Finish or cancel the active edit before saving");
    source = duplicate_string(utf8Path);
    save = duplicate_string(utf8Path);
    if (!source || !save) {
        free(source); free(save);
        return document_error(CAD_STATUS_OUT_OF_MEMORY,
                              "Could not copy the save path");
    }
    result = CadCodec_SavePathAtomic(utf8Path, &document->core.data,
                                     CAD_FORMAT_X11_STREAM);
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

CadResult CadDocument_BeginEdit(CadDocument* document) {
    if (!document)
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "Cannot begin an edit on a NULL document");
    if (document->transactionBefore)
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "A document edit is already active");
    document->transactionBefore = snapshot_create(document);
    if (!document->transactionBefore)
        return document_error(CAD_STATUS_OUT_OF_MEMORY,
                              "Could not capture the edit snapshot");
    return CadResult_Ok(document->sourceFormat);
}

CadResult CadDocument_CommitEdit(CadDocument* document) {
    CadDocumentSnapshot* current;
    int nativeContentChanged;
    if (!document || !document->transactionBefore)
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "No document edit is active");
    if (snapshot_matches(document->transactionBefore, document)) {
        free(document->transactionBefore);
        document->transactionBefore = NULL;
        update_dirty_state(document);
        return CadResult_Ok(document->sourceFormat);
    }
    nativeContentChanged = memcmp(&document->transactionBefore->core.data,
                                  &document->core.data,
                                  sizeof(document->core.data)) != 0;
    if (nativeContentChanged)
        document->revision = ++document->nextRevision;
    else
        document->revision = document->transactionBefore->revision;
    update_dirty_state(document);
    current = snapshot_create(document);
    if (!current) {
        snapshot_apply(document, document->transactionBefore);
        free(document->transactionBefore);
        document->transactionBefore = NULL;
        return document_error(CAD_STATUS_OUT_OF_MEMORY,
                              "Could not commit the edit snapshot");
    }
    while (document->historyCount > document->historyCursor + 1) {
        free(document->history[document->historyCount - 1]);
        document->history[--document->historyCount] = NULL;
    }
    if (document->historyCount) {
        free(document->history[document->historyCursor]);
        document->history[document->historyCursor] =
            document->transactionBefore;
    } else {
        history_append(document, document->transactionBefore);
    }
    document->transactionBefore = NULL;
    history_append(document, current);
    return CadResult_Ok(document->sourceFormat);
}

void CadDocument_CancelEdit(CadDocument* document) {
    if (!document || !document->transactionBefore) return;
    snapshot_apply(document, document->transactionBefore);
    free(document->transactionBefore);
    document->transactionBefore = NULL;
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
    if (!CadDocument_CanUndo(document))
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "There is no edit to undo");
    document->historyCursor--;
    snapshot_apply(document, document->history[document->historyCursor]);
    return CadResult_Ok(document->sourceFormat);
}

CadResult CadDocument_Redo(CadDocument* document) {
    if (!CadDocument_CanRedo(document))
        return document_error(CAD_STATUS_INVALID_ARGUMENT,
                              "There is no edit to redo");
    document->historyCursor++;
    snapshot_apply(document, document->history[document->historyCursor]);
    return CadResult_Ok(document->sourceFormat);
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
