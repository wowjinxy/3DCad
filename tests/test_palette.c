#define _CRT_SECURE_NO_WARNINGS

#include "cad_palette.h"
#include "cad_document.h"
#include "platform_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(expression)                                                     \
    do {                                                                      \
        if (!(expression)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,        \
                    __LINE__, #expression);                                   \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

static uint16_t fixture_word(unsigned index) {
    return (uint16_t)(((index * 0x0101u) ^ 0xa55au) & 0xffffu);
}

static void write_le16(uint8_t* bytes, uint16_t value) {
    bytes[0] = (uint8_t)(value & 0xffu);
    bytes[1] = (uint8_t)(value >> 8);
}

static void build_col_fixture(uint8_t bytes[CAD_COL_FILE_SIZE]) {
    unsigned index;
    for (index = 0; index < CAD_PALETTE_ENTRY_COUNT; ++index)
        write_le16(bytes + index * 2u, fixture_word(index));
}

static void build_pal_fixture(uint8_t bytes[CAD_PAL_FILE_SIZE],
                              int unknownRecord) {
    unsigned record;
    unsigned ordinal;
    memset(bytes, 0, CAD_PAL_FILE_SIZE);
    for (record = 0; record < CAD_PAL_RECORD_COUNT; ++record) {
        unsigned palette = record % 16u;
        unsigned colors = 15u - record % 16u;
        bytes[record * 2u] =
            (uint8_t)(record == (unsigned)unknownRecord ? 0xc7u : record % 6u);
        bytes[record * 2u + 1u] = (uint8_t)((palette << 4) | colors);
        for (ordinal = 0; ordinal < CAD_PAL_INDEX_COUNT; ++ordinal) {
            bytes[CAD_PAL_DESCRIPTOR_SIZE +
                  record * CAD_PAL_INDEX_COUNT + ordinal] =
                (uint8_t)((record * 37u + ordinal * 11u + 3u) & 0xffu);
        }
    }
}

static void test_default_creation(void) {
    CadPaletteFile file;
    CadPaletteFile before;
    CadResult result;
    uint8_t* encoded = NULL;
    size_t encodedSize = 0;
    unsigned index;
    unsigned ordinal;

    memset(&file, 0xa5, sizeof(file));
    result = CadPalette_Create(CAD_PALETTE_FORMAT_AUTO, &file);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(file.format == CAD_PALETTE_FORMAT_COL);
    for (index = 0; index < CAD_PALETTE_ENTRY_COUNT; ++index)
        CHECK(file.colWords[index] == 0);
    for (index = 0; index < CAD_PAL_RECORD_COUNT; ++index) {
        CHECK(file.palRecords[index].type == 0);
        CHECK(file.palRecords[index].paletteNumber == 0);
        CHECK(file.palRecords[index].colorCount == 0);
    }

    result = CadPalette_Create(CAD_PALETTE_FORMAT_PAL, &file);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(file.format == CAD_PALETTE_FORMAT_PAL);
    for (index = 0; index < CAD_PALETTE_ENTRY_COUNT; ++index)
        CHECK(file.colWords[index] == 0);
    for (index = 0; index < CAD_PAL_RECORD_COUNT; ++index) {
        CHECK(file.palRecords[index].type == CAD_PAL_TYPE_LIGHT_DEPTH);
        CHECK(file.palRecords[index].paletteNumber == 4);
        CHECK(file.palRecords[index].colorCount == 16);
        for (ordinal = 0; ordinal < CAD_PAL_INDEX_COUNT; ++ordinal)
            CHECK(file.palRecords[index].indices[ordinal] == 0);
    }
    result = CadPalette_Validate(&file);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(result.warningCount == 0);
    result = CadPalette_Encode(&file, CAD_PALETTE_FORMAT_PAL, &encoded,
                               &encodedSize);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(encodedSize == CAD_PAL_FILE_SIZE);
    for (index = 0; index < CAD_PAL_RECORD_COUNT; ++index) {
        CHECK(encoded[index * 2u] == 0x03u);
        CHECK(encoded[index * 2u + 1u] == 0x3fu);
    }
    for (index = CAD_PAL_DESCRIPTOR_SIZE; index < CAD_PAL_FILE_SIZE; ++index)
        CHECK(encoded[index] == 0);
    CadPalette_FreeBuffer(encoded);

    memset(&file, 0x6d, sizeof(file));
    before = file;
    result = CadPalette_Create((CadPaletteFormat)99, &file);
    CHECK(!CadResult_IsSuccess(&result));
    CHECK(memcmp(&file, &before, sizeof(file)) == 0);
    result = CadPalette_Create(CAD_PALETTE_FORMAT_COL, NULL);
    CHECK(!CadResult_IsSuccess(&result));
}

static void test_col_decode_encode(void) {
    uint8_t fixture[CAD_COL_FILE_SIZE + 9u];
    uint8_t* encoded = NULL;
    uint8_t* encodedAgain = NULL;
    size_t encodedSize = 0;
    size_t encodedAgainSize = 0;
    CadPaletteFile file;
    CadPaletteFile decodedAgain;
    CadResult result;
    unsigned index;

    build_col_fixture(fixture);
    memset(fixture + CAD_COL_FILE_SIZE, 0xee, 9u);
    memset(&file, 0, sizeof(file));
    result = CadPalette_Decode(fixture, sizeof(fixture),
                               CAD_PALETTE_FORMAT_AUTO, &file);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(file.format == CAD_PALETTE_FORMAT_COL);
    CHECK(result.bytesConsumed == CAD_COL_FILE_SIZE);
    CHECK(result.warningCount == 1);
    CHECK(result.diagnosticCount == 1);
    CHECK(result.diagnostics[0].severity == CAD_DIAGNOSTIC_WARNING);
    CHECK(result.diagnostics[0].byteOffset == CAD_COL_FILE_SIZE);
    CHECK(strstr(result.diagnostics[0].message, "trailing") != NULL);
    for (index = 0; index < CAD_PALETTE_ENTRY_COUNT; ++index)
        CHECK(file.colWords[index] == fixture_word(index));
    /* Raw bit 15 is data, even though the preview conversion ignores it. */
    CHECK((file.colWords[0] & 0x8000u) != 0);

    result = CadPalette_Encode(&file, CAD_PALETTE_FORMAT_AUTO, &encoded,
                               &encodedSize);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(result.warningCount == 0);
    CHECK(encoded != NULL);
    CHECK(encodedSize == CAD_COL_FILE_SIZE);
    CHECK(memcmp(encoded, fixture, CAD_COL_FILE_SIZE) == 0);

    memset(&decodedAgain, 0x19, sizeof(decodedAgain));
    result = CadPalette_Decode(encoded, encodedSize,
                               CAD_PALETTE_FORMAT_COL, &decodedAgain);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(result.warningCount == 0);
    CHECK(memcmp(decodedAgain.colWords, file.colWords,
                 sizeof(file.colWords)) == 0);
    result = CadPalette_Encode(&decodedAgain, CAD_PALETTE_FORMAT_COL,
                               &encodedAgain, &encodedAgainSize);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(encodedAgainSize == encodedSize);
    CHECK(memcmp(encodedAgain, encoded, encodedSize) == 0);

    CadPalette_FreeBuffer(encodedAgain);
    CadPalette_FreeBuffer(encoded);
}

static void test_pal_decode_encode(void) {
    uint8_t fixture[CAD_PAL_FILE_SIZE];
    uint8_t* encoded = NULL;
    uint8_t* encodedAgain = NULL;
    size_t encodedSize = 0;
    size_t encodedAgainSize = 0;
    CadPaletteFile file;
    CadPaletteFile second;
    CadPaletteFile asCol;
    CadResult result;
    unsigned record;
    unsigned ordinal;
    const int unknownRecord = 17;

    build_pal_fixture(fixture, unknownRecord);
    memset(&file, 0, sizeof(file));
    result = CadPalette_Decode(fixture, sizeof(fixture),
                               CAD_PALETTE_FORMAT_AUTO, &file);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(file.format == CAD_PALETTE_FORMAT_PAL);
    CHECK(result.bytesConsumed == CAD_PAL_FILE_SIZE);
    CHECK(result.warningCount == 1);
    CHECK(result.diagnosticCount == 1);
    CHECK(result.diagnostics[0].recordIndex == unknownRecord);
    CHECK(strstr(result.diagnostics[0].message, "preserved") != NULL);
    for (record = 0; record < CAD_PAL_RECORD_COUNT; ++record) {
        const CadPalRecord* value = &file.palRecords[record];
        CHECK(value->type == fixture[record * 2u]);
        CHECK(value->paletteNumber == record % 16u + 1u);
        CHECK(value->colorCount == 16u - record % 16u);
        for (ordinal = 0; ordinal < CAD_PAL_INDEX_COUNT; ++ordinal) {
            CHECK(value->indices[ordinal] ==
                  fixture[CAD_PAL_DESCRIPTOR_SIZE +
                          record * CAD_PAL_INDEX_COUNT + ordinal]);
        }
    }

    result = CadPalette_Encode(&file, CAD_PALETTE_FORMAT_AUTO, &encoded,
                               &encodedSize);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(result.warningCount == 1);
    CHECK(encodedSize == CAD_PAL_FILE_SIZE);
    CHECK(memcmp(encoded, fixture, sizeof(fixture)) == 0);

    memset(&second, 0, sizeof(second));
    result = CadPalette_Decode(encoded, encodedSize,
                               CAD_PALETTE_FORMAT_PAL, &second);
    CHECK(CadResult_IsSuccess(&result));
    result = CadPalette_Encode(&second, CAD_PALETTE_FORMAT_PAL,
                               &encodedAgain, &encodedAgainSize);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(encodedAgainSize == encodedSize);
    CHECK(memcmp(encodedAgain, encoded, encodedSize) == 0);

    /* Explicit COL remains useful for callers that intentionally want only
       the first 256 raw words; AUTO is what gives exact-size PAL priority. */
    memset(&asCol, 0, sizeof(asCol));
    result = CadPalette_Decode(fixture, sizeof(fixture),
                               CAD_PALETTE_FORMAT_COL, &asCol);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(asCol.format == CAD_PALETTE_FORMAT_COL);
    CHECK(result.warningCount == 1);
    CHECK(asCol.colWords[0] ==
          (uint16_t)(fixture[0] | ((uint16_t)fixture[1] << 8)));

    CadPalette_FreeBuffer(encodedAgain);
    CadPalette_FreeBuffer(encoded);
}

static void check_decode_failure_is_transactional(
    const uint8_t* bytes, size_t size, CadPaletteFormat format) {
    CadPaletteFile output;
    CadPaletteFile before;
    CadResult result;
    memset(&output, 0x5c, sizeof(output));
    before = output;
    result = CadPalette_Decode(bytes, size, format, &output);
    CHECK(!CadResult_IsSuccess(&result));
    CHECK(result.errorCount > 0);
    CHECK(result.diagnosticCount > 0);
    CHECK(memcmp(&output, &before, sizeof(output)) == 0);
}

static void test_errors_and_transactionality(void) {
    uint8_t col[CAD_COL_FILE_SIZE];
    uint8_t pal[CAD_PAL_FILE_SIZE + 1u];
    uint8_t* encoded = (uint8_t*)(uintptr_t)1u;
    size_t encodedSize = 123u;
    CadPaletteFile file;
    CadPaletteFile before;
    CadPaletteFile autoLong;
    CadResult result;
    build_col_fixture(col);
    build_pal_fixture(pal, -1);
    pal[CAD_PAL_FILE_SIZE] = 0;

    check_decode_failure_is_transactional(NULL, 0,
                                          CAD_PALETTE_FORMAT_AUTO);
    check_decode_failure_is_transactional(NULL, CAD_COL_FILE_SIZE,
                                          CAD_PALETTE_FORMAT_COL);
    check_decode_failure_is_transactional(col, CAD_COL_FILE_SIZE - 1u,
                                          CAD_PALETTE_FORMAT_AUTO);
    check_decode_failure_is_transactional(col, CAD_COL_FILE_SIZE - 1u,
                                          CAD_PALETTE_FORMAT_COL);
    check_decode_failure_is_transactional(pal, CAD_PAL_FILE_SIZE - 1u,
                                          CAD_PALETTE_FORMAT_PAL);
    check_decode_failure_is_transactional(pal, CAD_PAL_FILE_SIZE + 1u,
                                          CAD_PALETTE_FORMAT_PAL);
    check_decode_failure_is_transactional(col, sizeof(col),
                                          (CadPaletteFormat)99);
    result = CadPalette_Decode(col, sizeof(col), CAD_PALETTE_FORMAT_COL,
                               NULL);
    CHECK(!CadResult_IsSuccess(&result));

    /* AUTO recognizes PAL by its exact recovered size.  A larger buffer is a
       trailing-data COL unless the caller explicitly requests strict PAL. */
    result = CadPalette_Decode(pal, sizeof(pal), CAD_PALETTE_FORMAT_AUTO,
                               &autoLong);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(autoLong.format == CAD_PALETTE_FORMAT_COL);
    CHECK(result.warningCount == 1);

    result = CadPalette_Create(CAD_PALETTE_FORMAT_PAL, &file);
    CHECK(CadResult_IsSuccess(&result));
    file.palRecords[3].paletteNumber = 0;
    file.palRecords[7].colorCount = 17;
    before = file;
    result = CadPalette_Validate(&file);
    CHECK(!CadResult_IsSuccess(&result));
    CHECK(result.errorCount == 2);
    CHECK(memcmp(&file, &before, sizeof(file)) == 0);
    result = CadPalette_Encode(&file, CAD_PALETTE_FORMAT_PAL, &encoded,
                               &encodedSize);
    CHECK(!CadResult_IsSuccess(&result));
    CHECK(encoded == NULL);
    CHECK(encodedSize == 0);
    CHECK(memcmp(&file, &before, sizeof(file)) == 0);

    encoded = (uint8_t*)(uintptr_t)1u;
    encodedSize = 123u;
    result = CadPalette_Encode(NULL, CAD_PALETTE_FORMAT_COL, &encoded,
                               &encodedSize);
    CHECK(!CadResult_IsSuccess(&result));
    CHECK(encoded == NULL);
    CHECK(encodedSize == 0);
    result = CadPalette_Encode(&file, CAD_PALETTE_FORMAT_COL, NULL,
                               &encodedSize);
    CHECK(!CadResult_IsSuccess(&result));

    encoded = (uint8_t*)(uintptr_t)1u;
    encodedSize = 123u;
    result = CadPalette_Encode(&file, (CadPaletteFormat)99, &encoded,
                               &encodedSize);
    CHECK(!CadResult_IsSuccess(&result));
    CHECK(encoded == NULL);
    CHECK(encodedSize == 0);

    file.format = CAD_PALETTE_FORMAT_AUTO;
    result = CadPalette_Validate(&file);
    CHECK(!CadResult_IsSuccess(&result));
}

static void test_unknown_types_warn_without_data_loss(void) {
    CadPaletteFile file;
    CadResult result;
    uint8_t* encoded = NULL;
    size_t encodedSize = 0;
    unsigned record;
    result = CadPalette_Create(CAD_PALETTE_FORMAT_PAL, &file);
    CHECK(CadResult_IsSuccess(&result));
    for (record = 0; record < CAD_PAL_RECORD_COUNT; ++record)
        file.palRecords[record].type = 0xffu;
    result = CadPalette_Validate(&file);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(result.warningCount == CAD_PAL_RECORD_COUNT);
    CHECK(result.diagnosticCount == CAD_DIAGNOSTIC_CAPACITY);
    CHECK(result.status == CAD_STATUS_OK);
    result = CadPalette_Encode(&file, CAD_PALETTE_FORMAT_PAL, &encoded,
                               &encodedSize);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(result.warningCount == CAD_PAL_RECORD_COUNT);
    CHECK(encodedSize == CAD_PAL_FILE_SIZE);
    for (record = 0; record < CAD_PAL_RECORD_COUNT; ++record)
        CHECK(encoded[record * 2u] == 0xffu);
    CadPalette_FreeBuffer(encoded);
}

static int absolute_int(int value) {
    return value < 0 ? -value : value;
}

static void test_bgr555_helpers_exhaustively(void) {
    unsigned word;
    unsigned value;
    CadPaletteRgba5 color5;
    CadPaletteRgba8 color8;

    color8 = CadPalette_Bgr555ToRgba8(0x0000u);
    CHECK(color8.r == 0 && color8.g == 0 && color8.b == 0 && color8.a == 255);
    color8 = CadPalette_Bgr555ToRgba8(0x001fu);
    CHECK(color8.r == 255 && color8.g == 0 && color8.b == 0);
    color8 = CadPalette_Bgr555ToRgba8(0x03e0u);
    CHECK(color8.r == 0 && color8.g == 255 && color8.b == 0);
    color8 = CadPalette_Bgr555ToRgba8(0x7c00u);
    CHECK(color8.r == 0 && color8.g == 0 && color8.b == 255);
    color8 = CadPalette_Bgr555ToRgba8(0xffffu);
    CHECK(color8.r == 255 && color8.g == 255 && color8.b == 255);

    for (word = 0; word <= 0x7fffu; ++word) {
        CadPaletteRgba5 highBitColor;
        color5 = CadPalette_Bgr555ToRgba5((uint16_t)word);
        CHECK(color5.r == (word & 31u));
        CHECK(color5.g == ((word >> 5) & 31u));
        CHECK(color5.b == ((word >> 10) & 31u));
        CHECK(color5.a == 31u);
        CHECK(CadPalette_Rgba5ToBgr555(color5) == word);
        color8 = CadPalette_Bgr555ToRgba8((uint16_t)word);
        CHECK(color8.a == 255u);
        CHECK(CadPalette_Rgba8ToBgr555(color8) == word);
        highBitColor =
            CadPalette_Bgr555ToRgba5((uint16_t)(word | 0x8000u));
        CHECK(memcmp(&color5, &highBitColor, sizeof(color5)) == 0);
    }

    for (value = 0; value <= 255u; ++value) {
        CadPaletteRgba8 input = {(uint8_t)value, 0, 0, 0};
        uint16_t encoded = CadPalette_Rgba8ToBgr555(input);
        CadPaletteRgba8 preview = CadPalette_Bgr555ToRgba8(encoded);
        CHECK(absolute_int((int)preview.r - (int)value) <= 4);
        CHECK(preview.g == 0 && preview.b == 0 && preview.a == 255);
    }

    color5 = (CadPaletteRgba5){255, 200, 32, 0};
    CHECK(CadPalette_Rgba5ToBgr555(color5) == 0x7fffu);
    color8 = (CadPaletteRgba8){255, 255, 255, 0};
    CHECK(CadPalette_Rgba8ToBgr555(color8) == 0x7fffu);
}

static void test_document_palette_workflow(void) {
    const char* colPath = "threedcad-palette-workflow.col";
    const char* palPath = "threedcad-palette-workflow.pal";
    const char* cadPath = "threedcad-palette-workflow.cad";
    const char* badPath = "threedcad-palette-truncated.col";
    CadDocument* document = (CadDocument*)malloc(sizeof(*document));
    CadResult result;
    CadRgba resolved;
    FILE* bad;
    uint16_t original;
    uint64_t revision;
    unsigned historyCount;
    CHECK(document != NULL);
    if (!document) return;
    remove(colPath); remove(palPath); remove(cadPath); remove(badPath);
    CadDocument_Init(document);

    /* Give the native document a small valid chain so its independent save
       path can be exercised while palette edits remain outstanding. */
    result = CadDocument_BeginEditNamed(document, "Fixture Triangle");
    CHECK(CadResult_IsSuccess(&result));
    if (CadResult_IsSuccess(&result)) {
        int16_t p0 = CadCore_AddPoint(&document->core, 0.0, 0.0, 0.0);
        int16_t p1 = CadCore_AddPoint(&document->core, 10.0, 0.0, 0.0);
        int16_t p2 = CadCore_AddPoint(&document->core, 0.0, 10.0, 0.0);
        CHECK(p0 >= 0 && p1 >= 0 && p2 >= 0);
        document->core.data.points[p0].nextPoint = p1;
        document->core.data.points[p1].nextPoint = p2;
        document->core.data.points[p2].nextPoint = INVALID_INDEX;
        CHECK(CadCore_AddPolygon(&document->core, p0, 0, 3) >= 0);
        result = CadDocument_CommitEdit(document);
        CHECK(CadResult_IsSuccess(&result));
    }
    result = CadDocument_Save(document, cadPath);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(!document->isDirty);

    result = CadDocument_NewPalette(document, CAD_PALETTE_FORMAT_COL);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(CadDocument_HasPalette(document, CAD_PALETTE_FORMAT_COL));
    CHECK(document->colDirty);
    CHECK(!document->isDirty);
    CHECK(document->colSavePath == NULL);
    CHECK(CadDocument_GetColWord(document, 0) == 0x0000u);
    CHECK(CadDocument_GetColWord(document, 255) == 0x7fffu);
    result = CadDocument_Undo(document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(!CadDocument_HasPalette(document, CAD_PALETTE_FORMAT_COL));
    CHECK(!document->colDirty);
    result = CadDocument_Redo(document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(CadDocument_HasPalette(document, CAD_PALETTE_FORMAT_COL));
    CHECK(document->colDirty);

    original = CadDocument_GetColWord(document, 42);
    historyCount = document->historyCount;
    result = CadDocument_SetColWord(document, 42, original);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(document->historyCount == historyCount);
    result = CadDocument_SetColWord(document, 42, 0x9234u);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(CadDocument_GetColWord(document, 42) == 0x9234u);
    CHECK(document->colDirty && !document->isDirty);
    CHECK(CadDocument_CanUndo(document));
    result = CadDocument_Undo(document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(CadDocument_GetColWord(document, 42) == original);
    result = CadDocument_Redo(document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(CadDocument_GetColWord(document, 42) == 0x9234u);

    result = CadDocument_SavePalette(document, colPath,
                                     CAD_PALETTE_FORMAT_COL);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(!document->colDirty);
    CHECK(document->colSavePath && strcmp(document->colSavePath, colPath) == 0);
    result = CadDocument_SavePalette(document, cadPath,
                                     CAD_PALETTE_FORMAT_COL);
    CHECK(!CadResult_IsSuccess(&result));
    result = CadDocument_Save(document, colPath);
    CHECK(!CadResult_IsSuccess(&result));
    result = CadDocument_SetColWord(document, 42, 0x001fu);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(document->colDirty);
    /* Native CAD serialization cannot clean external palette changes. */
    result = CadDocument_Save(document, cadPath);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(!document->isDirty);
    CHECK(document->colDirty);
    result = CadDocument_SavePaletteCurrent(document,
                                            CAD_PALETTE_FORMAT_COL);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(!document->colDirty);
    result = CadDocument_SetColWord(document, 42, 0x03e0u);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(document->colDirty);
    result = CadDocument_Undo(document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(CadDocument_GetColWord(document, 42) == 0x001fu);
    CHECK(!document->colDirty);

    result = CadDocument_SetColWord(document, 42, 0x7c00u);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(document->colDirty);
    result = CadDocument_OpenPalette(document, colPath,
                                     CAD_PALETTE_FORMAT_COL);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(CadDocument_GetColWord(document, 42) == 0x001fu);
    CHECK(!document->colDirty);
    result = CadDocument_SetColWord(document, CAD_PALETTE_ENTRY_COUNT, 0);
    CHECK(!CadResult_IsSuccess(&result));

    result = CadDocument_NewPalette(document, CAD_PALETTE_FORMAT_PAL);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(CadDocument_HasPalette(document, CAD_PALETTE_FORMAT_PAL));
    CHECK(document->palDirty && !document->colDirty);
    result = CadDocument_Undo(document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(!CadDocument_HasPalette(document, CAD_PALETTE_FORMAT_PAL));
    CHECK(!document->palDirty);
    result = CadDocument_Redo(document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(CadDocument_HasPalette(document, CAD_PALETTE_FORMAT_PAL));
    CHECK(document->palDirty);
    {
        uint8_t type = 0, paletteNumber = 0, colorCount = 0;
        CHECK(CadDocument_GetPalDescriptor(document, 7, &type,
                                           &paletteNumber, &colorCount));
        CHECK(type == CAD_PAL_TYPE_LIGHT_DEPTH);
        CHECK(paletteNumber == 4 && colorCount == 16);
    }
    result = CadDocument_SetPalDescriptor(document, 7,
                                          CAD_PAL_TYPE_ANIMATION, 2, 9);
    CHECK(CadResult_IsSuccess(&result));
    result = CadDocument_SetPalDescriptor(
        document, CAD_PAL_RECORD_COUNT, CAD_PAL_TYPE_NORMAL, 1, 1);
    CHECK(!CadResult_IsSuccess(&result));
    result = CadDocument_SetPalDescriptor(
        document, 7, CAD_PAL_TYPE_NORMAL, 0, 17);
    CHECK(!CadResult_IsSuccess(&result));
    result = CadDocument_SetPalSample(document, 7, 9, 42);
    CHECK(CadResult_IsSuccess(&result));
    result = CadDocument_SetPalSample(document, CAD_PAL_RECORD_COUNT, 0, 0);
    CHECK(!CadResult_IsSuccess(&result));
    result = CadDocument_SetPalSample(document, 0, CAD_PAL_INDEX_COUNT, 0);
    CHECK(!CadResult_IsSuccess(&result));
    CHECK(CadDocument_ResolvePaletteColor(document, 7, 9, &resolved));
    CHECK(memcmp(&resolved, &document->palette[42], sizeof(resolved)) == 0);
    result = CadDocument_SavePalette(document, palPath,
                                     CAD_PALETTE_FORMAT_PAL);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(!document->palDirty);
    CHECK(CadDocument_HasUnsavedPaletteChanges(document) == 0);
    result = CadDocument_ValidateExportPath(document, cadPath);
    CHECK(!CadResult_IsSuccess(&result));
    result = CadDocument_ValidateExportPath(document, colPath);
    CHECK(!CadResult_IsSuccess(&result));
    result = CadDocument_ValidateExportPath(document, palPath);
    CHECK(!CadResult_IsSuccess(&result));
    result = CadDocument_ValidateExportPath(document, badPath);
    CHECK(CadResult_IsSuccess(&result));
    {
        CadDocument* probe = (CadDocument*)malloc(sizeof(*probe));
        CHECK(probe != NULL);
        if (probe) {
            CadDocument_Init(probe);
            result = CadDocument_OpenPalette(probe, palPath,
                                             CAD_PALETTE_FORMAT_COL);
            CHECK(!CadResult_IsSuccess(&result));
            CHECK(!CadDocument_HasPalette(probe,
                                          CAD_PALETTE_FORMAT_COL));
            result = CadDocument_OpenPalette(probe, colPath,
                                             CAD_PALETTE_FORMAT_PAL);
            CHECK(!CadResult_IsSuccess(&result));
            CHECK(!CadDocument_HasPalette(probe,
                                          CAD_PALETTE_FORMAT_PAL));
            CadDocument_Destroy(probe);
            free(probe);
        }
    }
    result = CadDocument_SavePalette(document, colPath,
                                     CAD_PALETTE_FORMAT_PAL);
    CHECK(!CadResult_IsSuccess(&result));
    result = CadDocument_SavePalette(document, palPath,
                                     CAD_PALETTE_FORMAT_COL);
    CHECK(!CadResult_IsSuccess(&result));
    result = CadDocument_OpenPalette(document, cadPath,
                                     CAD_PALETTE_FORMAT_AUTO);
    CHECK(!CadResult_IsSuccess(&result));
    result = CadDocument_OpenPalette(document, palPath,
                                     CAD_PALETTE_FORMAT_COL);
    CHECK(!CadResult_IsSuccess(&result));
    result = CadDocument_BeginEditNamed(document, "Active transaction");
    CHECK(CadResult_IsSuccess(&result));
    result = CadDocument_SavePaletteCurrent(document,
                                            CAD_PALETTE_FORMAT_PAL);
    CHECK(!CadResult_IsSuccess(&result));
    result = CadDocument_ValidateExportPath(document, badPath);
    CHECK(!CadResult_IsSuccess(&result));
    result = CadDocument_OpenPalette(document, colPath,
                                     CAD_PALETTE_FORMAT_COL);
    CHECK(!CadResult_IsSuccess(&result));
    CadDocument_CancelEdit(document);

    /* A failed open is fully transactional, including paths and revisions. */
    bad = fopen(badPath, "wb");
    CHECK(bad != NULL);
    if (bad) {
        const uint8_t bytes[3] = {1, 2, 3};
        CHECK(fwrite(bytes, 1, sizeof(bytes), bad) == sizeof(bytes));
        CHECK(fclose(bad) == 0);
    }
    original = CadDocument_GetColWord(document, 42);
    revision = document->colRevision;
    historyCount = document->historyCount;
    result = CadDocument_OpenPalette(document, badPath,
                                     CAD_PALETTE_FORMAT_COL);
    CHECK(!CadResult_IsSuccess(&result));
    CHECK(CadDocument_GetColWord(document, 42) == original);
    CHECK(document->colRevision == revision);
    CHECK(document->historyCount == historyCount);
    CHECK(document->colSavePath && strcmp(document->colSavePath, colPath) == 0);

    result = CadDocument_OpenPalette(document, palPath,
                                     CAD_PALETTE_FORMAT_PAL);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(!document->palDirty);
    {
        uint8_t mapped = 0;
        CHECK(CadDocument_GetPalSample(document, 7, 9, &mapped));
        CHECK(mapped == 42);
    }

    CadDocument_Destroy(document);
    free(document);
    remove(colPath); remove(palPath); remove(cadPath); remove(badPath);
}

static void validate_external_palette(const char* path) {
    CadPaletteFile first;
    CadPaletteFile second;
    CadResult result;
    uint8_t* source = NULL;
    uint8_t* encoded = NULL;
    uint8_t* encodedAgain = NULL;
    size_t sourceSize = 0;
    size_t encodedSize = 0;
    size_t encodedAgainSize = 0;
    size_t canonicalSize;
    result = CadPlatform_ReadFile(path, CAD_PLATFORM_DEFAULT_FILE_LIMIT,
                                  &source, &sourceSize);
    if (!CadResult_IsSuccess(&result)) goto failed;
    result = CadPalette_Decode(source, sourceSize, CAD_PALETTE_FORMAT_AUTO,
                               &first);
    if (!CadResult_IsSuccess(&result)) goto failed;
    result = CadPalette_Encode(&first, first.format, &encoded, &encodedSize);
    if (!CadResult_IsSuccess(&result)) goto failed;
    canonicalSize = first.format == CAD_PALETTE_FORMAT_COL
                        ? CAD_COL_FILE_SIZE : CAD_PAL_FILE_SIZE;
    if (encodedSize != canonicalSize || sourceSize < canonicalSize ||
        memcmp(encoded, source, canonicalSize) != 0) {
        fprintf(stderr, "external palette changed on first encoding: %s\n",
                path);
        ++failures;
        goto done;
    }
    result = CadPalette_Decode(encoded, encodedSize, first.format, &second);
    if (!CadResult_IsSuccess(&result)) goto failed;
    result = CadPalette_Encode(&second, second.format, &encodedAgain,
                               &encodedAgainSize);
    if (!CadResult_IsSuccess(&result)) goto failed;
    if (encodedAgainSize != encodedSize ||
        memcmp(encodedAgain, encoded, encodedSize) != 0) {
        fprintf(stderr, "external palette second encoding is not deterministic: %s\n",
                path);
        ++failures;
        goto done;
    }
    printf("validated external %s palette: %s\n",
           first.format == CAD_PALETTE_FORMAT_COL ? "COL" : "PAL", path);
    goto done;

failed:
    fprintf(stderr, "external palette failed: %s: %s\n", path,
            result.diagnosticCount ? result.diagnostics[0].message
                                   : CadStatus_Name(result.status));
    ++failures;
done:
    CadPlatform_Free(source);
    CadPalette_FreeBuffer(encoded);
    CadPalette_FreeBuffer(encodedAgain);
}

int main(int argc, char** argv) {
    test_default_creation();
    test_col_decode_encode();
    test_pal_decode_encode();
    test_errors_and_transactionality();
    test_unknown_types_warn_without_data_loss();
    test_bgr555_helpers_exhaustively();
    test_document_palette_workflow();
    for (int index = 1; index < argc; ++index)
        validate_external_palette(argv[index]);
    if (failures) {
        fprintf(stderr, "%d palette test(s) failed\n", failures);
        return 1;
    }
    puts("palette codec tests passed");
    return 0;
}
