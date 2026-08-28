#pragma once

/* Fixed-layout codecs for the recovered 3Ddraw formats.  No public structure
   is read from or written to disk directly. */

#include "cad_file.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAD_DIAGNOSTIC_CAPACITY 16
#define CAD_DIAGNOSTIC_MESSAGE_CAPACITY 192

typedef enum CadFormat {
    CAD_FORMAT_AUTO = 0,
    CAD_FORMAT_X11_STREAM,
    CAD_FORMAT_LEGACY_PACKED,
    CAD_FORMAT_ANM_3DAN,
    CAD_FORMAT_ANM_3DGI
} CadFormat;

typedef enum CadStatus {
    CAD_STATUS_OK = 0,
    CAD_STATUS_INVALID_ARGUMENT,
    CAD_STATUS_OUT_OF_MEMORY,
    CAD_STATUS_IO_ERROR,
    CAD_STATUS_EMPTY_INPUT,
    CAD_STATUS_UNRECOGNIZED_FORMAT,
    CAD_STATUS_TRUNCATED_RECORD,
    CAD_STATUS_UNKNOWN_RECORD,
    CAD_STATUS_INDEX_OUT_OF_RANGE,
    CAD_STATUS_DUPLICATE_RECORD,
    CAD_STATUS_INVALID_NUMBER,
    CAD_STATUS_INVALID_TOPOLOGY,
    CAD_STATUS_UNSUPPORTED_FORMAT
} CadStatus;

typedef enum CadDiagnosticSeverity {
    CAD_DIAGNOSTIC_INFO = 0,
    CAD_DIAGNOSTIC_WARNING,
    CAD_DIAGNOSTIC_ERROR
} CadDiagnosticSeverity;

typedef struct CadDiagnostic {
    CadDiagnosticSeverity severity;
    CadStatus code;
    size_t byteOffset;
    int recordTag;
    int recordIndex;
    char message[CAD_DIAGNOSTIC_MESSAGE_CAPACITY];
} CadDiagnostic;

typedef struct CadResult {
    CadStatus status;
    CadFormat format;
    size_t bytesConsumed;
    unsigned warningCount;
    unsigned errorCount;
    size_t diagnosticCount;
    CadDiagnostic diagnostics[CAD_DIAGNOSTIC_CAPACITY];
} CadResult;

/* Explicit payload sizes from the recovered NEWS/SPARC ABI. */
enum {
    CAD_X11_OBJECT_PAYLOAD_SIZE = 40,
    CAD_X11_POLYGON_PAYLOAD_SIZE = 14,
    CAD_X11_POINT_PAYLOAD_SIZE = 32,
    CAD_X11_ANIMATION_INDEX_PAYLOAD_SIZE = 130,
    CAD_X11_ANIMATION_POINT_PAYLOAD_SIZE = 32,
    CAD_LEGACY_OBJECT_PAYLOAD_SIZE = 40,
    CAD_LEGACY_POLYGON_PAYLOAD_SIZE = 8,
    CAD_LEGACY_POINT_PAYLOAD_SIZE = 32
};

CadResult CadResult_Ok(CadFormat format);
int CadResult_IsSuccess(const CadResult* result);
const char* CadStatus_Name(CadStatus status);

/* Decode is transactional: output is changed only after the complete buffer
   has decoded and passed structural validation.  AUTO tries both recovered
   encodings and reports the one that validates completely. */
CadResult CadCodec_Decode(const uint8_t* bytes, size_t size,
                          CadFormat requestedFormat, CadFileData* output);

/* The supported save format is the later X11 stream.  The allocated output
   buffer belongs to the caller and must be released with CadCodec_FreeBuffer. */
CadResult CadCodec_Encode(const CadFileData* data, CadFormat format,
                          uint8_t** outputBytes, size_t* outputSize);
void CadCodec_FreeBuffer(void* buffer);

CadResult CadCodec_Validate(const CadFileData* data);

#ifdef __cplusplus
}
#endif
