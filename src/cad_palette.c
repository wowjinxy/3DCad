#include "cad_palette.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void palette_add_diagnostic(CadResult* result,
                                   CadDiagnosticSeverity severity,
                                   CadStatus code, size_t byteOffset,
                                   int recordIndex, const char* format, ...) {
    CadDiagnostic* diagnostic = NULL;
    va_list args;
    if (!result) return;
    if (severity == CAD_DIAGNOSTIC_WARNING) ++result->warningCount;
    if (severity == CAD_DIAGNOSTIC_ERROR) {
        ++result->errorCount;
        if (result->status == CAD_STATUS_OK) result->status = code;
    }
    if (result->diagnosticCount < CAD_DIAGNOSTIC_CAPACITY) {
        diagnostic = &result->diagnostics[result->diagnosticCount++];
        memset(diagnostic, 0, sizeof(*diagnostic));
        diagnostic->severity = severity;
        diagnostic->code = code;
        diagnostic->byteOffset = byteOffset;
        diagnostic->recordTag = -1;
        diagnostic->recordIndex = recordIndex;
        va_start(args, format);
        vsnprintf(diagnostic->message, sizeof(diagnostic->message), format,
                  args);
        va_end(args);
    }
}

static CadResult palette_error(CadStatus status, size_t byteOffset,
                               int recordIndex, const char* message) {
    CadResult result = CadResult_Ok(CAD_FORMAT_AUTO);
    palette_add_diagnostic(&result, CAD_DIAGNOSTIC_ERROR, status, byteOffset,
                           recordIndex, "%s", message ? message : "Palette error");
    return result;
}

static int palette_format_valid(CadPaletteFormat format) {
    return format == CAD_PALETTE_FORMAT_COL ||
           format == CAD_PALETTE_FORMAT_PAL;
}

static int pal_type_known(uint8_t type) {
    return type <= (uint8_t)CAD_PAL_TYPE_TEXTURE_MAP;
}

static uint16_t read_le16(const uint8_t* bytes) {
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static void write_le16(uint8_t* bytes, uint16_t value) {
    bytes[0] = (uint8_t)(value & 0xffu);
    bytes[1] = (uint8_t)(value >> 8);
}

static CadResult validate_as(const CadPaletteFile* file,
                             CadPaletteFormat format) {
    CadResult result = CadResult_Ok(CAD_FORMAT_AUTO);
    size_t index;
    if (!file)
        return palette_error(CAD_STATUS_INVALID_ARGUMENT, 0, -1,
                             "Palette validation requires a resource");
    if (!palette_format_valid(format))
        return palette_error(CAD_STATUS_UNSUPPORTED_FORMAT, 0, -1,
                             "Palette validation supports only COL and PAL");
    if (format == CAD_PALETTE_FORMAT_COL) return result;

    for (index = 0; index < CAD_PAL_RECORD_COUNT; ++index) {
        const CadPalRecord* record = &file->palRecords[index];
        size_t offset = index * 2u;
        if (record->paletteNumber < 1 || record->paletteNumber > 16) {
            palette_add_diagnostic(
                &result, CAD_DIAGNOSTIC_ERROR, CAD_STATUS_INVALID_NUMBER,
                offset + 1u, (int)index,
                "PAL record %zu palette number must be between 1 and 16",
                index);
        }
        if (record->colorCount < 1 || record->colorCount > 16) {
            palette_add_diagnostic(
                &result, CAD_DIAGNOSTIC_ERROR, CAD_STATUS_INVALID_NUMBER,
                offset + 1u, (int)index,
                "PAL record %zu color count must be between 1 and 16", index);
        }
        if (!pal_type_known(record->type)) {
            palette_add_diagnostic(
                &result, CAD_DIAGNOSTIC_WARNING, CAD_STATUS_UNKNOWN_RECORD,
                offset, (int)index,
                "PAL record %zu uses unknown type %u; it will be preserved",
                index, (unsigned)record->type);
        }
    }
    return result;
}

CadResult CadPalette_Create(CadPaletteFormat format, CadPaletteFile* output) {
    CadPaletteFile created;
    size_t index;
    if (!output)
        return palette_error(CAD_STATUS_INVALID_ARGUMENT, 0, -1,
                             "Palette creation requires an output resource");
    if (format == CAD_PALETTE_FORMAT_AUTO) format = CAD_PALETTE_FORMAT_COL;
    if (!palette_format_valid(format))
        return palette_error(CAD_STATUS_UNSUPPORTED_FORMAT, 0, -1,
                             "Palette creation supports only COL and PAL");
    memset(&created, 0, sizeof(created));
    created.format = format;
    if (format == CAD_PALETTE_FORMAT_PAL) {
        for (index = 0; index < CAD_PAL_RECORD_COUNT; ++index) {
            created.palRecords[index].type = CAD_PAL_TYPE_LIGHT_DEPTH;
            created.palRecords[index].paletteNumber = 4;
            created.palRecords[index].colorCount = 16;
        }
    }
    *output = created;
    return CadResult_Ok(CAD_FORMAT_AUTO);
}

CadResult CadPalette_Decode(const uint8_t* bytes, size_t size,
                            CadPaletteFormat requestedFormat,
                            CadPaletteFile* output) {
    CadPaletteFile decoded;
    CadPaletteFormat format = requestedFormat;
    CadResult result;
    size_t index;
    if (!output)
        return palette_error(CAD_STATUS_INVALID_ARGUMENT, 0, -1,
                             "Palette decode requires an output resource");
    if (!size)
        return palette_error(CAD_STATUS_EMPTY_INPUT, 0, -1,
                             "Palette input is empty");
    if (!bytes)
        return palette_error(CAD_STATUS_INVALID_ARGUMENT, 0, -1,
                             "Palette decode requires an input buffer");
    if (format == CAD_PALETTE_FORMAT_AUTO) {
        if (size == CAD_PAL_FILE_SIZE)
            format = CAD_PALETTE_FORMAT_PAL;
        else if (size >= CAD_COL_FILE_SIZE)
            format = CAD_PALETTE_FORMAT_COL;
        else
            return palette_error(
                CAD_STATUS_TRUNCATED_RECORD, size, -1,
                "Palette input is shorter than a 0x200-byte COL table");
    }
    if (!palette_format_valid(format))
        return palette_error(CAD_STATUS_UNSUPPORTED_FORMAT, 0, -1,
                             "Palette decode supports only COL and PAL");
    if (format == CAD_PALETTE_FORMAT_COL && size < CAD_COL_FILE_SIZE)
        return palette_error(CAD_STATUS_TRUNCATED_RECORD, size, -1,
                             "COL input is shorter than 0x200 bytes");
    if (format == CAD_PALETTE_FORMAT_PAL && size < CAD_PAL_FILE_SIZE)
        return palette_error(CAD_STATUS_TRUNCATED_RECORD, size, -1,
                             "PAL input is shorter than 0x8200 bytes");
    if (format == CAD_PALETTE_FORMAT_PAL && size > CAD_PAL_FILE_SIZE)
        return palette_error(CAD_STATUS_INVALID_NUMBER, CAD_PAL_FILE_SIZE, -1,
                             "PAL input must be exactly 0x8200 bytes");

    memset(&decoded, 0, sizeof(decoded));
    decoded.format = format;
    if (format == CAD_PALETTE_FORMAT_COL) {
        for (index = 0; index < CAD_PALETTE_ENTRY_COUNT; ++index)
            decoded.colWords[index] = read_le16(bytes + index * 2u);
    } else {
        for (index = 0; index < CAD_PAL_RECORD_COUNT; ++index) {
            const uint8_t packed = bytes[index * 2u + 1u];
            CadPalRecord* record = &decoded.palRecords[index];
            record->type = bytes[index * 2u];
            record->paletteNumber = (uint8_t)(((packed >> 4) & 0x0fu) + 1u);
            record->colorCount = (uint8_t)((packed & 0x0fu) + 1u);
            memcpy(record->indices,
                   bytes + CAD_PAL_DESCRIPTOR_SIZE +
                       index * CAD_PAL_INDEX_COUNT,
                   CAD_PAL_INDEX_COUNT);
        }
    }

    result = validate_as(&decoded, format);
    if (!CadResult_IsSuccess(&result)) return result;
    result.bytesConsumed = format == CAD_PALETTE_FORMAT_COL
                               ? CAD_COL_FILE_SIZE
                               : CAD_PAL_FILE_SIZE;
    if (format == CAD_PALETTE_FORMAT_COL && size > CAD_COL_FILE_SIZE) {
        palette_add_diagnostic(
            &result, CAD_DIAGNOSTIC_WARNING, CAD_STATUS_INVALID_NUMBER,
            CAD_COL_FILE_SIZE, -1,
            "COL input contains %zu trailing byte(s); only the first 0x200 bytes were used",
            size - CAD_COL_FILE_SIZE);
    }
    *output = decoded;
    return result;
}

CadResult CadPalette_Validate(const CadPaletteFile* file) {
    if (!file)
        return palette_error(CAD_STATUS_INVALID_ARGUMENT, 0, -1,
                             "Palette validation requires a resource");
    return validate_as(file, file->format);
}

CadResult CadPalette_Encode(const CadPaletteFile* file,
                            CadPaletteFormat format,
                            uint8_t** outputBytes, size_t* outputSize) {
    CadResult result;
    uint8_t* bytes;
    size_t size;
    size_t index;
    if (!outputBytes || !outputSize)
        return palette_error(CAD_STATUS_INVALID_ARGUMENT, 0, -1,
                             "Palette encode requires output buffer pointers");
    *outputBytes = NULL;
    *outputSize = 0;
    if (!file)
        return palette_error(CAD_STATUS_INVALID_ARGUMENT, 0, -1,
                             "Palette encode requires a resource");
    if (format == CAD_PALETTE_FORMAT_AUTO) {
        format = palette_format_valid(file->format)
                     ? file->format
                     : CAD_PALETTE_FORMAT_COL;
    }
    if (!palette_format_valid(format))
        return palette_error(CAD_STATUS_UNSUPPORTED_FORMAT, 0, -1,
                             "Palette encode supports only COL and PAL");
    result = validate_as(file, format);
    if (!CadResult_IsSuccess(&result)) return result;
    size = format == CAD_PALETTE_FORMAT_COL ? CAD_COL_FILE_SIZE
                                            : CAD_PAL_FILE_SIZE;
    bytes = (uint8_t*)malloc(size);
    if (!bytes)
        return palette_error(CAD_STATUS_OUT_OF_MEMORY, 0, -1,
                             "Could not allocate the encoded palette buffer");

    if (format == CAD_PALETTE_FORMAT_COL) {
        for (index = 0; index < CAD_PALETTE_ENTRY_COUNT; ++index)
            write_le16(bytes + index * 2u, file->colWords[index]);
    } else {
        for (index = 0; index < CAD_PAL_RECORD_COUNT; ++index) {
            const CadPalRecord* record = &file->palRecords[index];
            bytes[index * 2u] = record->type;
            bytes[index * 2u + 1u] = (uint8_t)(
                ((record->paletteNumber - 1u) << 4) |
                (record->colorCount - 1u));
            memcpy(bytes + CAD_PAL_DESCRIPTOR_SIZE +
                       index * CAD_PAL_INDEX_COUNT,
                   record->indices, CAD_PAL_INDEX_COUNT);
        }
    }
    *outputBytes = bytes;
    *outputSize = size;
    result.bytesConsumed = size;
    return result;
}

void CadPalette_FreeBuffer(void* buffer) {
    free(buffer);
}

static uint8_t saturate5(uint8_t value) {
    return value > 31u ? 31u : value;
}

static uint8_t expand5(uint8_t value) {
    return (uint8_t)(((unsigned)value * 255u + 15u) / 31u);
}

static uint8_t quantize8(uint8_t value) {
    return (uint8_t)(((unsigned)value * 31u + 127u) / 255u);
}

CadPaletteRgba5 CadPalette_Bgr555ToRgba5(uint16_t word) {
    CadPaletteRgba5 result;
    result.r = (uint8_t)(word & 31u);
    result.g = (uint8_t)((word >> 5) & 31u);
    result.b = (uint8_t)((word >> 10) & 31u);
    result.a = 31u;
    return result;
}

CadPaletteRgba8 CadPalette_Bgr555ToRgba8(uint16_t word) {
    CadPaletteRgba5 color5 = CadPalette_Bgr555ToRgba5(word);
    CadPaletteRgba8 result;
    result.r = expand5(color5.r);
    result.g = expand5(color5.g);
    result.b = expand5(color5.b);
    result.a = 255u;
    return result;
}

uint16_t CadPalette_Rgba5ToBgr555(CadPaletteRgba5 color) {
    const uint16_t red = saturate5(color.r);
    const uint16_t green = saturate5(color.g);
    const uint16_t blue = saturate5(color.b);
    return (uint16_t)(red | (green << 5) | (blue << 10));
}

uint16_t CadPalette_Rgba8ToBgr555(CadPaletteRgba8 color) {
    CadPaletteRgba5 color5;
    color5.r = quantize8(color.r);
    color5.g = quantize8(color.g);
    color5.b = quantize8(color.b);
    color5.a = 31u;
    return CadPalette_Rgba5ToBgr555(color5);
}
