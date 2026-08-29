#pragma once

/* Buffer-based codecs for the recovered 3DCad color resources.

   COL is a 256-entry, little-endian BGR555 color table.  PAL is a separate
   256-record mapping resource: its first 0x200 bytes contain packed record
   descriptors and its remaining 0x8000 bytes contain 128 raw COL indices per
   record.  Filesystem access deliberately remains outside this module. */

#include "cad_codec.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAD_PALETTE_ENTRY_COUNT 256u
#define CAD_COL_FILE_SIZE 0x200u
#define CAD_PAL_RECORD_COUNT 256u
#define CAD_PAL_INDEX_COUNT 128u
#define CAD_PAL_DESCRIPTOR_SIZE 0x200u
#define CAD_PAL_PAYLOAD_SIZE 0x8000u
#define CAD_PAL_FILE_SIZE 0x8200u

typedef enum CadPaletteFormat {
    CAD_PALETTE_FORMAT_AUTO = 0,
    CAD_PALETTE_FORMAT_COL,
    CAD_PALETTE_FORMAT_PAL
} CadPaletteFormat;

typedef enum CadPalRecordType {
    CAD_PAL_TYPE_NORMAL = 0,
    CAD_PAL_TYPE_DEPTH_CUE = 1,
    CAD_PAL_TYPE_LIGHT_SOURCE = 2,
    CAD_PAL_TYPE_LIGHT_DEPTH = 3,
    CAD_PAL_TYPE_ANIMATION = 4,
    CAD_PAL_TYPE_TEXTURE_MAP = 5
} CadPalRecordType;

typedef struct CadPalRecord {
    /* Unknown type values are intentionally retained for lossless round trips. */
    uint8_t type;
    /* Both packed descriptor nibbles represent the stored value minus one. */
    uint8_t paletteNumber; /* 1..16 */
    uint8_t colorCount;    /* 1..16 */
    uint8_t indices[CAD_PAL_INDEX_COUNT];
} CadPalRecord;

typedef struct CadPaletteFile {
    CadPaletteFormat format;
    /* Raw COL words are authoritative.  Bit 15 is retained even though the
       BGR555 conversion helpers ignore it. */
    uint16_t colWords[CAD_PALETTE_ENTRY_COUNT];
    CadPalRecord palRecords[CAD_PAL_RECORD_COUNT];
} CadPaletteFile;

typedef struct CadPaletteRgba5 {
    uint8_t r, g, b, a;
} CadPaletteRgba5;

typedef struct CadPaletteRgba8 {
    uint8_t r, g, b, a;
} CadPaletteRgba8;

/* AUTO creates a COL resource.  PAL creation uses the recovered default
   descriptor (Light Depth, palette 4, 16 colors) and zero indices. */
CadResult CadPalette_Create(CadPaletteFormat format, CadPaletteFile* output);

/* Decode is transactional: output changes only after the complete buffer has
   decoded and validated.  AUTO recognizes exact-size PAL before treating any
   buffer of at least 0x200 bytes as COL.  COL trailing data is ignored with a
   warning; PAL must be exactly 0x8200 bytes. */
CadResult CadPalette_Decode(const uint8_t* bytes, size_t size,
                            CadPaletteFormat requestedFormat,
                            CadPaletteFile* output);

/* AUTO encodes using file->format, falling back to COL for an unset format.
   The returned allocation belongs to the caller. */
CadResult CadPalette_Encode(const CadPaletteFile* file,
                            CadPaletteFormat format,
                            uint8_t** outputBytes, size_t* outputSize);
CadResult CadPalette_Validate(const CadPaletteFile* file);
void CadPalette_FreeBuffer(void* buffer);

/* BGR555 uses bits 0..4 for red, 5..9 for green, and 10..14 for blue.
   Alpha is always opaque on decode and ignored on encode.  RGBA5 inputs above
   31 are saturated; RGBA8 conversion uses nearest-value quantization. */
CadPaletteRgba5 CadPalette_Bgr555ToRgba5(uint16_t word);
CadPaletteRgba8 CadPalette_Bgr555ToRgba8(uint16_t word);
uint16_t CadPalette_Rgba5ToBgr555(CadPaletteRgba5 color);
uint16_t CadPalette_Rgba8ToBgr555(CadPaletteRgba8 color);

#ifdef __cplusplus
}
#endif
