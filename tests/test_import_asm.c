#include "cad_import_asm.h"
#include "platform_fs.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n",                     \
                __FILE__, __LINE__, #condition);                             \
        return 0;                                                            \
    }                                                                        \
} while (0)

static const char kConstants[] =
    "base equ 4\r\n"
    "later = base*2\r\n";

static const char kAliasShape[] =
    "Alias ShapeHdr Custom_P,0,Custom_F,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,<Alias>\r\n"
    "local = later+1\r\n"
    "Custom_P\r\n"
    "  Pointsb 2\r\n"
    "  pb local,-2,$03 ; source point zero\r\n"
    "  pbd2 -6,4,%1000\r\n"
    ".mirror PointsXw 1\r\n"
    "  pw 5,6,7\r\n"
    "  EndPoints\r\n"
    "Custom_F\r\n"
    "  Vizis 2\r\n"
    "Custom_F1 Faces\r\n"
    "  Face2 7,-1,0,0,0,0,1\r\n"
    "  FendQ\r\n"
    "Custom_F2 Faces\r\n"
    "  Face4 9,0,-1,-2,3,0,1,2,3\r\n"
    "  EndShape\r\n";

static const char kZuluShape[] =
    "Zulu_P\n"
    "Pointsb 2\n"
    "pb 0,0,0\n"
    "pb 1,0,0\n"
    "EndPoints\n"
    "Zulu_F\n"
    "Faces\n"
    "Face2 1,0,0,0,1,0,1\n"
    "EndShape\n";

/* Clean-room fixture for the recovered local mlaser convention.  The values,
   names, topology, and frame count are deliberately synthetic. */
static const char kMlaserShape[] =
    "mlaser macro nose,middle,tail,half_width\n"
    "  Pointsb 6\n"
    "  pb 0,0,\\3\n"
    "  pb -\\4,0,\\2\n"
    "  pb 0,0,\\1\n"
    "  pb \\4,0,\\2\n"
    "  pb 0,-2,\\2\n"
    "  pb 0,2,\\2\n"
    "  EndPoints\n"
    "  endm\n"
    "Pulse ShapeHdr Pulse_P,0,Pulse_F,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,<Pulse>\n"
    "Pulse_P\n"
    "  DataHdr\n"
    "  Frames 2\n"
    "  JumpTab .pose1\n"
    "  JumpTab .pose0\n"
    ".pose0 mlaser 99,88,77,6\n"
    ".pose1 mlaser 12,-7,-33,5\n"
    "Pulse_F\n"
    "  Faces\n"
    "  Face4 12,0,0,1,0,0,1,2,3\n"
    "  Face4 13,0,1,0,0,0,4,2,5\n"
    "  EndShape\n";

static const char kMalformedMlaserShape[] =
    "mlaser macro nose,middle,tail,half_width\n"
    "  Pointsb 6\n"
    "  pb 0,0,\\3\n"
    "  pb -\\4,0,\\2\n"
    "  pb 0,0,\\1\n"
    "  pb \\4,0,\\2\n"
    "  pb 0,-2,\\2\n"
    "  pb 0,2,\\2\n"
    "  EndPoints\n"
    "  endm\n"
    "Broken ShapeHdr Broken_P,0,Broken_F,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,<Broken>\n"
    "Broken_P\n"
    "  DataHdr\n"
    "  Frames 2\n"
    "  JumpTab .pose0\n"
    "  JumpTab .pose1\n"
    ".pose0 mlaser 1,2,3\n"
    ".pose1 mlaser 4,5,6,7\n"
    "Broken_F\n"
    "  Faces\n"
    "  Face4 1,0,0,1,0,0,1,2,3\n"
    "  EndShape\n";

static const char kDifferentMlaserShape[] =
    /* A later redefinition controls the invocation and must win. */
    "mlaser macro nose,middle,tail,half_width\n"
    "  Pointsb 6\n"
    "  pb 0,0,\\3\n"
    "  pb -\\4,0,\\2\n"
    "  pb 0,0,\\1\n"
    "  pb \\4,0,\\2\n"
    "  pb 0,-2,\\2\n"
    "  pb 0,2,\\2\n"
    "  EndPoints\n"
    "  endm\n"
    "mlaser macro nose,middle,tail,half_width\n"
    "  Pointsb 6\n"
    "  pb 1,0,\\3\n" /* Deliberately differs from the recovered macro. */
    "  pb -\\4,0,\\2\n"
    "  pb 0,0,\\1\n"
    "  pb \\4,0,\\2\n"
    "  pb 0,-2,\\2\n"
    "  pb 0,2,\\2\n"
    "  EndPoints\n"
    "  endm\n"
    "Mismatch ShapeHdr Mismatch_P,0,Mismatch_F,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,<Mismatch>\n"
    "Mismatch_P\n"
    "  DataHdr\n"
    "  Frames 1\n"
    "  JumpTab .pose0\n"
    ".pose0 mlaser 1,2,3,4\n"
    "Mismatch_static\n"
    "  Pointsb 4\n"
    "  pb 0,0,0\n"
    "  pb 1,0,0\n"
    "  pb 0,1,0\n"
    "  pb 0,0,1\n"
    "  EndPoints\n"
    "Mismatch_F\n"
    "  Faces\n"
    "  Face4 1,0,0,1,0,0,1,2,5\n"
    "  EndShape\n";

static CadAsmTextSource text_source(const char* name, const char* text) {
    CadAsmTextSource source;
    source.name = name;
    source.bytes = (const uint8_t*)text;
    source.size = strlen(text);
    return source;
}

static void report_failure(const CadResult* result) {
    size_t index;
    if (!result || CadResult_IsSuccess(result)) return;
    for (index = 0; index < result->diagnosticCount; ++index)
        fprintf(stderr, "ASM diagnostic line %d: %s\n",
                result->diagnostics[index].recordIndex,
                result->diagnostics[index].message);
}

static int expected_count_matches(const char* variable, size_t actual) {
    const char* text = getenv(variable);
    char* end = NULL;
    unsigned long long expected;
    if (!text || !text[0]) return 1;
    errno = 0;
    expected = strtoull(text, &end, 10);
    if (errno || end == text || *end || text[0] == '-' ||
        expected > (unsigned long long)SIZE_MAX) {
        fprintf(stderr, "Invalid %s expectation: %s\n", variable, text);
        return 0;
    }
    if (actual != (size_t)expected) {
        fprintf(stderr, "%s expected %llu, found %zu\n",
                variable, expected, actual);
        return 0;
    }
    return 1;
}

static int expected_corpus_counts_match(size_t total, size_t decoded,
                                        size_t unsupported) {
    int matches = 1;
    matches &= expected_count_matches("THREEDCAD_EXPECT_ASM_TOTAL", total);
    matches &= expected_count_matches("THREEDCAD_EXPECT_ASM_DECODED", decoded);
    matches &= expected_count_matches("THREEDCAD_EXPECT_ASM_UNSUPPORTED",
                                      unsupported);
    return matches;
}

static int test_catalog_and_decode(void) {
    CadAsmTextSource sources[2];
    CadAsmTextSource constants = text_source("STRATEQU.INC", kConstants);
    CadAsmCatalog* catalog = (CadAsmCatalog*)malloc(sizeof(*catalog));
    CadFileData* output = (CadFileData*)malloc(sizeof(*output));
    CadAsmImportInfo info;
    CadResult result;
    const CadAsmShapeInfo* alias;
    const CadAsmShapeInfo* custom;
    CHECK(catalog && output);
    sources[0] = text_source("z_shapes.asm", kZuluShape);
    sources[1] = text_source("a_shapes.asm", kAliasShape);

    result = CadImportAsm_BuildCatalog(sources, 2, catalog);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(catalog->shapeCount == 3);
    CHECK(strcmp(catalog->shapes[0].name, "Alias") == 0);
    CHECK(strcmp(catalog->shapes[1].name, "Custom") == 0);
    CHECK(strcmp(catalog->shapes[2].name, "Zulu") == 0);
    alias = CadImportAsm_FindShape(catalog, "aLiAs");
    custom = CadImportAsm_FindShape(catalog, "custom");
    CHECK(alias && custom);
    CHECK(strcmp(alias->pointSection, "Custom_P") == 0);
    CHECK(alias->sourceIndex == 1);
    CHECK(strcmp(alias->sourceName, "a_shapes.asm") == 0);

    memset(&info, 0, sizeof(info));
    result = CadImportAsm_DecodeCatalogShape(
        sources, 2, &constants, 1, alias, NULL, output, &info);
    report_failure(&result);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(result.bytesConsumed == strlen(kAliasShape));
    CHECK(info.sourcePointCount == 4);
    CHECK(info.polygonCount == 2);
    CHECK(info.generatedPointCount == 6);
    CHECK(output->objectCount == 1);
    CHECK(output->polygonCount == 2);
    CHECK(output->pointCount == 6);
    CHECK(output->objects[0].flags == 1);
    CHECK(output->objects[0].firstPolygon == 0);
    CHECK(output->polygons[0].nextPolygon == 1);
    CHECK(output->polygons[1].nextPolygon == -1);
    CHECK(output->polygons[0].npoints == 2);
    CHECK(output->polygons[0].color == 7);
    CHECK(output->polygons[0].side == 2);
    CHECK(output->polygons[1].npoints == 4);
    CHECK(output->polygons[1].color == 9);
    CHECK(output->polygons[1].side == 7);
    CHECK(output->points[0].pointx == 9.0);
    CHECK(output->points[0].pointy == 2.0);
    CHECK(output->points[0].pointz == 3.0);
    CHECK(output->points[1].pointx == -3.0);
    CHECK(output->points[1].pointy == -2.0);
    CHECK(output->points[1].pointz == 4.0);
    CHECK(output->points[4].pointx == 5.0);
    CHECK(output->points[4].pointy == -6.0);
    CHECK(output->points[5].pointx == -5.0);
    CHECK(output->points[5].pointy == -6.0);
    CHECK(CadResult_IsSuccess(&(CadResult){0}) == 1);
    result = CadCodec_Validate(output);
    CHECK(CadResult_IsSuccess(&result));

    free(output);
    free(catalog);
    return 1;
}

static int test_options_and_core_wrapper(void) {
    CadAsmTextSource source = text_source("shapes.asm", kAliasShape);
    CadAsmTextSource constants = text_source("constants.inc", kConstants);
    CadAsmImportOptions options = CadImportAsm_DefaultOptions();
    CadCore* core = (CadCore*)malloc(sizeof(*core));
    CadResult result;
    CHECK(core);
    CadCore_Init(core);
    options.invertY = 0;
    result = CadImportAsm_DecodeShapeToCore(
        &source, 1, &constants, 1, "CUSTOM", &options, core, NULL);
    report_failure(&result);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(core->data.polygonCount == 2);
    CHECK(core->data.points[0].pointy == -2.0);
    CHECK(core->selection.pointCount == 0);
    CHECK(core->selection.polygonCount == 0);
    CHECK(core->isDirty == 1);
    free(core);
    return 1;
}

static int test_mlaser_static_preview(void) {
    CadAsmTextSource source = text_source("synthetic_laser.asm", kMlaserShape);
    CadFileData* output = (CadFileData*)malloc(sizeof(*output));
    CadAsmImportInfo info;
    CadResult result;
    size_t diagnostic;
    int foundPreviewWarning = 0;
    CHECK(output);

    memset(&info, 0, sizeof(info));
    result = CadImportAsm_DecodeShape(&source, 1, NULL, 0, "Pulse", NULL,
                                      output, &info);
    report_failure(&result);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(result.warningCount == 1);
    for (diagnostic = 0; diagnostic < result.diagnosticCount; ++diagnostic) {
        if (result.diagnostics[diagnostic].severity == CAD_DIAGNOSTIC_WARNING &&
            strstr(result.diagnostics[diagnostic].message,
                   "static preview") != NULL)
            foundPreviewWarning = 1;
    }
    CHECK(foundPreviewWarning);
    CHECK(info.sourcePointCount == 6);
    CHECK(info.polygonCount == 2);
    CHECK(info.generatedPointCount == 8);
    CHECK(output->polygonCount == 2);
    CHECK(output->pointCount == 8);

    /* Only the first jump-table pose is used.  Default import flips Y. */
    CHECK(output->points[0].pointx == 0.0);
    CHECK(output->points[0].pointy == 0.0);
    CHECK(output->points[0].pointz == -33.0);
    CHECK(output->points[1].pointx == -5.0);
    CHECK(output->points[1].pointz == -7.0);
    CHECK(output->points[2].pointz == 12.0);
    CHECK(output->points[3].pointx == 5.0);
    CHECK(output->points[5].pointy == 2.0);
    CHECK(output->points[7].pointy == -2.0);
    result = CadCodec_Validate(output);
    CHECK(CadResult_IsSuccess(&result));

    free(output);
    return 1;
}

static int test_mlaser_malformed_first_frame_is_transactional(void) {
    CadAsmTextSource source = text_source("broken_laser.asm",
                                          kMalformedMlaserShape);
    CadFileData* output = (CadFileData*)malloc(sizeof(*output));
    CadFileData* before = (CadFileData*)malloc(sizeof(*before));
    CadAsmImportInfo info;
    CadAsmImportInfo infoBefore;
    CadResult result;
    CHECK(output && before);
    memset(output, 0xa7, sizeof(*output));
    *before = *output;
    memset(&info, 0x4d, sizeof(info));
    infoBefore = info;

    result = CadImportAsm_DecodeShape(&source, 1, NULL, 0, "Broken", NULL,
                                      output, &info);
    CHECK(!CadResult_IsSuccess(&result));
    CHECK(result.status == CAD_STATUS_UNRECOGNIZED_FORMAT);
    CHECK(result.diagnosticCount > 0);
    CHECK(strstr(result.diagnostics[0].message, "mlaser") != NULL);
    CHECK(memcmp(output, before, sizeof(*output)) == 0);
    CHECK(memcmp(&info, &infoBefore, sizeof(info)) == 0);

    free(before);
    free(output);
    return 1;
}

static int test_mlaser_requires_recovered_signature(void) {
    CadAsmTextSource source = text_source("different_laser.asm",
                                          kDifferentMlaserShape);
    CadFileData* output = (CadFileData*)malloc(sizeof(*output));
    CadFileData* before = (CadFileData*)malloc(sizeof(*before));
    CadResult result;
    CHECK(output && before);
    memset(output, 0xb3, sizeof(*output));
    *before = *output;

    result = CadImportAsm_DecodeShape(&source, 1, NULL, 0, "Mismatch", NULL,
                                      output, NULL);
    CHECK(!CadResult_IsSuccess(&result));
    CHECK(result.status == CAD_STATUS_UNRECOGNIZED_FORMAT);
    CHECK(result.warningCount == 0);
    CHECK(memcmp(output, before, sizeof(*output)) == 0);

    free(before);
    free(output);
    return 1;
}

static int test_transactional_errors(void) {
    static const char invalidShape[] =
        "Bad_P\n"
        "Pointsb 2\n"
        "pb 0,0,0\n"
        "pb 1,0,0\n"
        "EndPoints\n"
        "Bad_F\n"
        "Face3 2,0,0,0,1,0,1,2\n"
        "EndShape\n";
    static const char unresolvedShape[] =
        "Unknown_P\n"
        "Pointsb 2\n"
        "pb missing,0,0\n"
        "pb 1,0,0\n"
        "EndPoints\n"
        "Unknown_F\n"
        "Face2 2,0,0,0,1,0,1\n"
        "EndShape\n";
    CadAsmTextSource source;
    CadFileData* output = (CadFileData*)malloc(sizeof(*output));
    CadFileData* before = (CadFileData*)malloc(sizeof(*before));
    CadAsmImportInfo info;
    CadAsmImportInfo infoBefore;
    CadResult result;
    CHECK(output && before);
    memset(output, 0x5a, sizeof(*output));
    *before = *output;
    memset(&info, 0x3c, sizeof(info));
    infoBefore = info;

    source = text_source("bad.asm", invalidShape);
    result = CadImportAsm_DecodeShape(&source, 1, NULL, 0, "Bad", NULL,
                                      output, &info);
    CHECK(!CadResult_IsSuccess(&result));
    CHECK(result.status == CAD_STATUS_INDEX_OUT_OF_RANGE);
    CHECK(result.diagnosticCount > 0);
    CHECK(result.diagnostics[0].recordIndex == 7);
    CHECK(memcmp(output, before, sizeof(*output)) == 0);
    CHECK(memcmp(&info, &infoBefore, sizeof(info)) == 0);

    source = text_source("unknown.asm", unresolvedShape);
    result = CadImportAsm_DecodeShape(&source, 1, NULL, 0, "Unknown", NULL,
                                      output, NULL);
    CHECK(!CadResult_IsSuccess(&result));
    CHECK(result.status == CAD_STATUS_INVALID_NUMBER);
    CHECK(memcmp(output, before, sizeof(*output)) == 0);

    free(before);
    free(output);
    return 1;
}

static int test_native_capacity_failure(void) {
    const size_t capacity = 65536;
    char* text = (char*)malloc(capacity);
    CadFileData* output = (CadFileData*)malloc(sizeof(*output));
    CadFileData* before = (CadFileData*)malloc(sizeof(*before));
    CadAsmTextSource source;
    CadResult result;
    size_t used = 0;
    int face;
    CHECK(text && output && before);
    used += (size_t)snprintf(text + used, capacity - used,
        "Big_P\nPointsb 2\npb 0,0,0\npb 1,0,0\nEndPoints\nBig_F\nFaces\n");
    for (face = 0; face < 513; ++face) {
        used += (size_t)snprintf(text + used, capacity - used,
                                "Face2 1,0,0,0,1,0,1\n");
        CHECK(used < capacity);
    }
    used += (size_t)snprintf(text + used, capacity - used, "EndShape\n");
    source.name = "capacity.asm";
    source.bytes = (const uint8_t*)text;
    source.size = used;
    memset(output, 0xa5, sizeof(*output));
    *before = *output;
    result = CadImportAsm_DecodeShape(&source, 1, NULL, 0, "Big", NULL,
                                      output, NULL);
    CHECK(!CadResult_IsSuccess(&result));
    CHECK(result.status == CAD_STATUS_INDEX_OUT_OF_RANGE);
    CHECK(memcmp(output, before, sizeof(*output)) == 0);
    free(before);
    free(output);
    free(text);
    return 1;
}

static int run_external_corpus(int argc, char** argv) {
    CadAsmTextSource* asmSources;
    CadAsmTextSource* constantSources;
    size_t asmCount = 0;
    size_t constantCount = 0;
    int constants = 0;
    int argument;
    CadAsmCatalog* catalog;
    CadFileData* output;
    CadResult result;
    size_t index;
    size_t decoded = 0;
    size_t unsupported = 0;
    int failed = 0;
    asmSources = (CadAsmTextSource*)calloc((size_t)argc, sizeof(*asmSources));
    constantSources = (CadAsmTextSource*)calloc((size_t)argc,
                                                sizeof(*constantSources));
    catalog = (CadAsmCatalog*)malloc(sizeof(*catalog));
    output = (CadFileData*)malloc(sizeof(*output));
    if (!asmSources || !constantSources || !catalog || !output) return 1;
    for (argument = 2; argument < argc; ++argument) {
        CadAsmTextSource* source;
        uint8_t* bytes = NULL;
        size_t size = 0;
        if (strcmp(argv[argument], "--constants") == 0) {
            constants = 1;
            continue;
        }
        source = constants ? &constantSources[constantCount++]
                           : &asmSources[asmCount++];
        result = CadPlatform_ReadFile(argv[argument], CAD_ASM_MAX_INPUT_BYTES,
                                      &bytes, &size);
        if (!CadResult_IsSuccess(&result)) {
            fprintf(stderr, "Could not read ASM corpus file %s\n",
                    argv[argument]);
            failed = 1;
            goto cleanup;
        }
        source->name = argv[argument];
        source->bytes = bytes;
        source->size = size;
    }
    result = CadImportAsm_BuildCatalog(asmSources, asmCount, catalog);
    if (!CadResult_IsSuccess(&result)) {
        report_failure(&result);
        failed = 1;
        goto cleanup;
    }
    for (index = 0; index < catalog->shapeCount; ++index) {
        result = CadImportAsm_DecodeCatalogShape(
            asmSources, asmCount, constantSources, constantCount,
            &catalog->shapes[index], NULL, output, NULL);
        if (!CadResult_IsSuccess(&result)) {
            if (result.status == CAD_STATUS_UNRECOGNIZED_FORMAT ||
                result.status == CAD_STATUS_INVALID_TOPOLOGY) {
                ++unsupported;
                continue;
            }
            fprintf(stderr, "ASM corpus shape %s (%s) failed\n",
                    catalog->shapes[index].name,
                    catalog->shapes[index].sourceName);
            report_failure(&result);
            ++failed;
        } else {
            ++decoded;
        }
    }
    printf("ASM corpus: %zu catalog entries, %zu decoded, %zu unsupported, "
           "%d failed\n", catalog->shapeCount, decoded, unsupported, failed);
    if (!expected_corpus_counts_match(catalog->shapeCount, decoded,
                                      unsupported))
        failed = 1;

cleanup:
    for (index = 0; index < asmCount; ++index)
        CadPlatform_Free((void*)asmSources[index].bytes);
    for (index = 0; index < constantCount; ++index)
        CadPlatform_Free((void*)constantSources[index].bytes);
    free(output);
    free(catalog);
    free(constantSources);
    free(asmSources);
    return failed ? 1 : 0;
}

int main(int argc, char** argv) {
    int passed = 0;
    int total = 0;
    if (argc > 1 && strcmp(argv[1], "--corpus") == 0)
        return run_external_corpus(argc, argv);
#define RUN(test) do { ++total; if (test()) ++passed; } while (0)
    RUN(test_catalog_and_decode);
    RUN(test_options_and_core_wrapper);
    RUN(test_mlaser_static_preview);
    RUN(test_mlaser_malformed_first_frame_is_transactional);
    RUN(test_mlaser_requires_recovered_signature);
    RUN(test_transactional_errors);
    RUN(test_native_capacity_failure);
#undef RUN
    if (passed != total) {
        fprintf(stderr, "%d/%d ASM importer tests passed\n", passed, total);
        return 1;
    }
    printf("All %d ASM importer tests passed\n", total);
    return 0;
}
