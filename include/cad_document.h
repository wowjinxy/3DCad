#pragma once

#include "cad_codec.h"
#include "cad_core.h"
#include "cad_palette.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAD_DOCUMENT_HISTORY_LIMIT 64
#define CAD_DOCUMENT_HISTORY_LABEL_CAPACITY 64
#define CAD_COLOR_DATA_SIZE 0x200
#define CAD_PALETTE_DATA_SIZE 0x8200

typedef struct CadDocumentSnapshot CadDocumentSnapshot;

typedef struct CadRgba {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
} CadRgba;

struct CadDocument;
typedef CadResult (*CadDocumentEditCallback)(struct CadDocument* document,
                                             void* userData);

typedef struct CadDocument {
    CadCore core;

    /* sourcePath identifies what was opened/imported.  savePath is non-NULL
       only for a named later-X11 document. */
    char* sourcePath;
    char* savePath;
    char* lastImportPath;
    char* lastExportPath;
    /* The recovered color resources are separate files.  COL is the actual
       256-entry BGR555 hardware palette; PAL is a 256-material lookup map.
       Neither resource is serialized by native CAD saves, so each keeps its
       own source/save association and dirty revision. */
    char* colSourcePath;
    char* colSavePath;
    char* palSourcePath;
    char* palSavePath;
    /* Retained only for a validated native X11 load.  An unchanged document
       can therefore be saved byte-for-byte, including recovered fields that
       have no editable in-memory representation. */
    uint8_t* originalNativeBytes;
    size_t originalNativeByteCount;
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
    int colDirty;
    int palDirty;
    uint64_t colRevision;
    uint64_t colSavedRevision;
    uint64_t nextColRevision;
    uint64_t palRevision;
    uint64_t palSavedRevision;
    uint64_t nextPalRevision;

    CadDocumentSnapshot* history[CAD_DOCUMENT_HISTORY_LIMIT];
    unsigned historyCount;
    unsigned historyCursor;
    CadDocumentSnapshot* transactionBefore;
    char transactionLabel[CAD_DOCUMENT_HISTORY_LABEL_CAPACITY];
} CadDocument;

void CadDocument_Init(CadDocument* document);
void CadDocument_Destroy(CadDocument* document);
void CadDocument_New(CadDocument* document);
/* Retains the current content and history, but detaches it from model source,
   import/export, and save destinations.  The result always requires a native
   Save As and remains dirty across undo/redo. */
void CadDocument_MakeUnnamed(CadDocument* document);

/* Transactional replacement.  Legacy packed inputs remain unnamed and dirty
   so a later-X11 Save As is required; their path is retained as import origin. */
CadResult CadDocument_Load(CadDocument* document, const char* utf8Path);
CadResult CadDocument_ImportAnm(CadDocument* document,
                                const char* utf8Path);
CadResult CadDocument_ExportAnm(CadDocument* document,
                                const char* utf8Path,
                                CadFormat format);
/* Rejects an export target that aliases the native source/save path, the
   current import source, or an attached COL/PAL resource.  A previous export
   path remains writable so repeated exports work normally. */
CadResult CadDocument_ValidateExportPath(const CadDocument* document,
                                         const char* utf8Path);
CadResult CadDocument_Save(CadDocument* document, const char* utf8Path);
CadResult CadDocument_SaveCurrent(CadDocument* document);

void CadDocument_MarkDirty(CadDocument* document);
int CadDocument_HasAnimation(const CadDocument* document);

/* Begin/commit brackets one user gesture (including a whole pointer drag).
   Cancel restores the exact pre-gesture document. */
CadResult CadDocument_BeginEdit(CadDocument* document);
CadResult CadDocument_BeginEditNamed(CadDocument* document,
                                     const char* label);
/* Runs any number of core mutations as one named, validated history entry.
   Callback failure or final validation failure restores the exact snapshot. */
CadResult CadDocument_ApplyEdit(CadDocument* document, const char* label,
                                CadDocumentEditCallback callback,
                                void* userData);
CadResult CadDocument_CommitEdit(CadDocument* document);
void CadDocument_CancelEdit(CadDocument* document);
int CadDocument_CanUndo(const CadDocument* document);
int CadDocument_CanRedo(const CadDocument* document);
CadResult CadDocument_Undo(CadDocument* document);
CadResult CadDocument_Redo(CadDocument* document);
const char* CadDocument_GetUndoLabel(const CadDocument* document);
const char* CadDocument_GetRedoLabel(const CadDocument* document);
void CadDocument_ClearHistory(CadDocument* document);

/* Paths are copied, never borrowed. */
int CadDocument_SetLastImportPath(CadDocument* document, const char* utf8Path);
int CadDocument_SetLastExportPath(CadDocument* document, const char* utf8Path);

/* Independent recovered color-resource lifecycle.  Open is transactional;
   New creates a useful deterministic COL table or the recovered PAL defaults.
   Save writes only the requested resource and never changes native CAD dirty
   state.  Palette edits remain part of the document's named undo timeline. */
CadResult CadDocument_NewPalette(CadDocument* document,
                                 CadPaletteFormat format);
CadResult CadDocument_OpenPalette(CadDocument* document,
                                  const char* utf8Path,
                                  CadPaletteFormat format);
CadResult CadDocument_SavePalette(CadDocument* document,
                                  const char* utf8Path,
                                  CadPaletteFormat format);
CadResult CadDocument_SavePaletteCurrent(CadDocument* document,
                                         CadPaletteFormat format);
int CadDocument_HasPalette(const CadDocument* document,
                           CadPaletteFormat format);
int CadDocument_HasUnsavedPaletteChanges(const CadDocument* document);

/* COL words remain authoritative so untouched bit 15 round-trips exactly. */
uint16_t CadDocument_GetColWord(const CadDocument* document, unsigned index);
CadResult CadDocument_SetColWord(CadDocument* document, unsigned index,
                                 uint16_t bgr555);

/* PAL descriptors expose the recovered semantics without inventing runtime
   interpolation rules.  Payload samples are raw COL indices (0-255). */
int CadDocument_GetPalDescriptor(const CadDocument* document, unsigned index,
                                 uint8_t* type, uint8_t* paletteNumber,
                                 uint8_t* colorCount);
CadResult CadDocument_SetPalDescriptor(CadDocument* document, unsigned index,
                                       uint8_t type, uint8_t paletteNumber,
                                       uint8_t colorCount);
int CadDocument_GetPalSample(const CadDocument* document, unsigned material,
                             unsigned sample, uint8_t* colorIndex);
CadResult CadDocument_SetPalSample(CadDocument* document, unsigned material,
                                   unsigned sample, uint8_t colorIndex);
int CadDocument_ResolvePaletteColor(const CadDocument* document,
                                    unsigned material, unsigned sample,
                                    CadRgba* output);

/* Legacy integration point retained for callers that already decoded COL.
   Values are quantized through BGR555 and installed as one undoable edit;
   polygon colors remain full uint8 indices. */
int CadDocument_SetPalette(CadDocument* document,
                           const CadRgba entries[256],
                           const char* utf8SourcePath);
void CadDocument_ClearPalette(CadDocument* document);

#ifdef __cplusplus
}
#endif
