#pragma once

#include "cad_codec.h"
#include "cad_core.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAD_DOCUMENT_HISTORY_LIMIT 64
#define CAD_COLOR_DATA_SIZE 0x200
#define CAD_PALETTE_DATA_SIZE 0x8200

typedef struct CadDocumentSnapshot CadDocumentSnapshot;

typedef struct CadRgba {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
} CadRgba;

typedef struct CadDocument {
    CadCore core;

    /* sourcePath identifies what was opened/imported.  savePath is non-NULL
       only for a named later-X11 document. */
    char* sourcePath;
    char* savePath;
    char* lastImportPath;
    char* lastExportPath;
    char* paletteSourcePath;
    CadFormat sourceFormat;
    int isDirty;

    /* History snapshots carry stable content revisions.  savedRevision names
       the exact native CAD state last written, so undoing away from a save is
       dirty and redoing back to it is clean.  nextRevision is monotonic for
       the lifetime of the currently loaded document. */
    uint64_t revision;
    uint64_t savedRevision;
    uint64_t nextRevision;

    uint8_t colorData[CAD_COLOR_DATA_SIZE];
    uint8_t paletteData[CAD_PALETTE_DATA_SIZE];
    size_t colorDataSize;
    size_t paletteDataSize;
    CadRgba palette[256];
    int paletteValid;

    CadDocumentSnapshot* history[CAD_DOCUMENT_HISTORY_LIMIT];
    unsigned historyCount;
    unsigned historyCursor;
    CadDocumentSnapshot* transactionBefore;
} CadDocument;

void CadDocument_Init(CadDocument* document);
void CadDocument_Destroy(CadDocument* document);
void CadDocument_New(CadDocument* document);

/* Transactional replacement.  Legacy packed inputs remain unnamed and dirty
   so a later-X11 Save As is required; their path is retained as import origin. */
CadResult CadDocument_Load(CadDocument* document, const char* utf8Path);
CadResult CadDocument_Save(CadDocument* document, const char* utf8Path);
CadResult CadDocument_SaveCurrent(CadDocument* document);

void CadDocument_MarkDirty(CadDocument* document);
int CadDocument_HasAnimation(const CadDocument* document);

/* Begin/commit brackets one user gesture (including a whole pointer drag).
   Cancel restores the exact pre-gesture document. */
CadResult CadDocument_BeginEdit(CadDocument* document);
CadResult CadDocument_CommitEdit(CadDocument* document);
void CadDocument_CancelEdit(CadDocument* document);
int CadDocument_CanUndo(const CadDocument* document);
int CadDocument_CanRedo(const CadDocument* document);
CadResult CadDocument_Undo(CadDocument* document);
CadResult CadDocument_Redo(CadDocument* document);
void CadDocument_ClearHistory(CadDocument* document);

/* Paths are copied, never borrowed. */
int CadDocument_SetLastImportPath(CadDocument* document, const char* utf8Path);
int CadDocument_SetLastExportPath(CadDocument* document, const char* utf8Path);

/* Integration point for .COL/.PAL parsers: parse externally, then install all
   256 entries transactionally.  Polygon colors remain full uint8 indices. */
int CadDocument_SetPalette(CadDocument* document,
                           const CadRgba entries[256],
                           const char* utf8SourcePath);
void CadDocument_ClearPalette(CadDocument* document);

#ifdef __cplusplus
}
#endif
