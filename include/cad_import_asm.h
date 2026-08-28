#pragma once

/* Portable importer for the recovered Star Fox shape-assembly dialect.
   Filesystem enumeration and document replacement deliberately live outside
   this module: callers provide immutable text buffers, inspect the sorted
   catalog, decode into a temporary model, and explicitly replace a document. */

#include "cad_codec.h"
#include "cad_core.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    CAD_ASM_MAX_CATALOG_SHAPES = 4096,
    CAD_ASM_MAX_CONSTANTS = 4096,
    CAD_ASM_MAX_SOURCE_POINTS = 8192,
    CAD_ASM_NAME_CAPACITY = 128,
    CAD_ASM_SOURCE_NAME_CAPACITY = 260,
    CAD_ASM_MAX_INPUT_BYTES = 16 * 1024 * 1024
};

typedef struct CadAsmTextSource {
    /* name is used only for diagnostics and deterministic catalog ordering. */
    const char* name;
    const uint8_t* bytes;
    size_t size;
} CadAsmTextSource;

typedef struct CadAsmShapeInfo {
    char name[CAD_ASM_NAME_CAPACITY];
    char pointSection[CAD_ASM_NAME_CAPACITY];
    char faceSection[CAD_ASM_NAME_CAPACITY];
    char sourceName[CAD_ASM_SOURCE_NAME_CAPACITY];
    size_t sourceIndex;
    size_t byteOffset;
    size_t lineNumber; /* one based */
} CadAsmShapeInfo;

typedef struct CadAsmCatalog {
    size_t shapeCount;
    CadAsmShapeInfo shapes[CAD_ASM_MAX_CATALOG_SHAPES];
} CadAsmCatalog;

typedef struct CadAsmImportOptions {
    /* The recovered renderer used screen-down Y.  The editor uses Y-up, so
       the default importer behavior negates source Y coordinates. */
    int invertY;
} CadAsmImportOptions;

typedef struct CadAsmImportInfo {
    CadAsmShapeInfo shape;
    size_t sourcePointCount;
    size_t polygonCount;
    size_t generatedPointCount;
} CadAsmImportInfo;

CadAsmImportOptions CadImportAsm_DefaultOptions(void);

/* Build a case-insensitively sorted catalog from caller-owned ASM buffers.
   Duplicate names are retained (and diagnosed); source name and byte offset
   provide deterministic tie breaking for search/preview UIs. */
CadResult CadImportAsm_BuildCatalog(const CadAsmTextSource* asmSources,
                                    size_t asmSourceCount,
                                    CadAsmCatalog* output);

/* Return the first deterministic exact match, or NULL.  The returned pointer
   is owned by catalog. */
const CadAsmShapeInfo* CadImportAsm_FindShape(const CadAsmCatalog* catalog,
                                              const char* shapeName);

/* Decode one catalog entry.  constantSources are applied in caller order and
   may contain EQU or '=' definitions.  Shape-local definitions overlay that
   baseline without global state.  output changes only after full validation. */
CadResult CadImportAsm_DecodeCatalogShape(
    const CadAsmTextSource* asmSources, size_t asmSourceCount,
    const CadAsmTextSource* constantSources, size_t constantSourceCount,
    const CadAsmShapeInfo* shape, const CadAsmImportOptions* options,
    CadFileData* output, CadAsmImportInfo* info);

/* Convenience path for an exact name lookup followed by transactional decode. */
CadResult CadImportAsm_DecodeShape(
    const CadAsmTextSource* asmSources, size_t asmSourceCount,
    const CadAsmTextSource* constantSources, size_t constantSourceCount,
    const char* shapeName, const CadAsmImportOptions* options,
    CadFileData* output, CadAsmImportInfo* info);

/* CadCore wrapper for previews.  On success it clears selection/active-edit
   state, installs the decoded root topology, and marks the preview dirty.
   The supplied core is untouched on failure. */
CadResult CadImportAsm_DecodeShapeToCore(
    const CadAsmTextSource* asmSources, size_t asmSourceCount,
    const CadAsmTextSource* constantSources, size_t constantSourceCount,
    const char* shapeName, const CadAsmImportOptions* options,
    CadCore* output, CadAsmImportInfo* info);

#ifdef __cplusplus
}
#endif
