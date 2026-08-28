#include "cad_codec.h"
#include "cad_core.h"
#include "cad_document.h"
#include "editor_tool.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", \
                    __FILE__, __LINE__, #condition); \
            failures++; \
        } \
    } while (0)

static void be16(uint8_t* p, int value) {
    uint16_t u = (uint16_t)(int16_t)value;
    p[0] = (uint8_t)(u >> 8);
    p[1] = (uint8_t)u;
}

static void be64(uint8_t* p, uint64_t value) {
    int i;
    for (i = 7; i >= 0; --i) {
        p[i] = (uint8_t)value;
        value >>= 8;
    }
}

static void be_double(uint8_t* p, double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    be64(p, bits);
}

static int16_t append_triangle(CadCore* core, double offset, uint8_t color) {
    int16_t a = CadCore_AddPoint(core, offset + 0.0, 0.0, 0.0);
    int16_t b = CadCore_AddPoint(core, offset + 1.0, 0.0, 0.0);
    int16_t c = CadCore_AddPoint(core, offset + 0.0, 1.0, 0.0);
    CHECK(a >= 0 && b >= 0 && c >= 0);
    core->data.points[a].nextPoint = b;
    core->data.points[b].nextPoint = c;
    core->data.points[c].nextPoint = -1;
    return CadCore_AddPolygon(core, a, color, 3);
}

static int write_test_file(const char* path, const void* bytes, size_t size) {
    FILE* file = fopen(path, "wb");
    int ok;
    if (!file) return 0;
    ok = fwrite(bytes, 1, size, file) == size;
    if (fclose(file) != 0) ok = 0;
    return ok;
}

static void test_later_animation_round_trip(void) {
    CadCore core;
    CadFileData* decoded;
    CadResult result;
    uint8_t* bytes = NULL;
    size_t size = 0;
    int frame;
    int16_t polygon;
    CadCore_Init(&core);
    polygon = append_triangle(&core, 0.0, 255);
    CHECK(polygon == 0);
    core.data.objects[0].selectFlag = 1;
    core.data.polygons[polygon].selectFlag = 1;
    core.data.points[0].selectFlag = 1;
    core.data.polygons[polygon].animation = 0;
    core.data.animationIndices[0].flags = 1;
    for (frame = 0; frame < CAD_ANIMATION_FRAMES; ++frame)
        core.data.animationIndices[0].frame[frame] = -1;
    core.data.animationIndices[0].frame[0] = 0;
    core.data.animationIndexCount = 1;
    for (frame = 0; frame < 3; ++frame) {
        core.data.animationPoints[frame].flags = 1;
        core.data.animationPoints[frame].nextPoint =
            (int16_t)(frame == 2 ? -1 : frame + 1);
        core.data.animationPoints[frame].pointx = 3.25 + frame;
        core.data.animationPoints[frame].pointy = -2.5;
        core.data.animationPoints[frame].pointz = 8.0;
    }
    core.data.animationPointCount = 3;

    result = CadCodec_Encode(&core.data, CAD_FORMAT_X11_STREAM, &bytes, &size);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(size == (size_t)(43 + 17 + 3 * 35 + 133 + 3 * 35));
    decoded = (CadFileData*)malloc(sizeof(*decoded));
    CHECK(decoded != NULL);
    if (decoded && bytes) {
        result = CadCodec_Decode(bytes, size, CAD_FORMAT_AUTO, decoded);
        CHECK(CadResult_IsSuccess(&result));
        CHECK(result.format == CAD_FORMAT_X11_STREAM);
        CHECK(decoded->polygons[0].color == 255);
        CHECK(decoded->objects[0].selectFlag == 0);
        CHECK(decoded->polygons[0].selectFlag == 0);
        CHECK(decoded->points[0].selectFlag == 0);
        CHECK(core.data.objects[0].selectFlag == 1);
        CHECK(decoded->polygons[0].animation == 0);
        CHECK(decoded->animationIndices[0].frame[0] == 0);
        CHECK(decoded->animationIndices[0].frame[1] == -1);
        CHECK(fabs(decoded->animationPoints[0].pointx - 3.25) < 1e-12);
        CHECK(fabs(decoded->animationPoints[0].pointy + 2.5) < 1e-12);
        CHECK(decoded->animationPoints[0].nextPoint == 1);
        CHECK(decoded->animationPoints[2].nextPoint == -1);
        decoded->animationPoints[1].nextPoint = -1;
        result = CadCodec_Validate(decoded);
        CHECK(!CadResult_IsSuccess(&result));
        decoded->animationPoints[1].nextPoint = 2;
        decoded->animationPoints[2].nextPoint = 0;
        result = CadCodec_Validate(decoded);
        CHECK(!CadResult_IsSuccess(&result));
        decoded->animationPoints[2].nextPoint = -1;
        decoded->animationIndices[0].frame[0] = -1;
        decoded->animationIndices[0].frame[1] = 0;
        result = CadCodec_Validate(decoded);
        CHECK(!CadResult_IsSuccess(&result));
    }
    free(decoded);
    CadCodec_FreeBuffer(bytes);
    CadCore_Destroy(&core);
}

static size_t make_legacy_triangle(uint8_t* bytes, size_t capacity) {
    size_t p = 0;
    int i;
    const double xyz[3][3] = {
        {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}
    };
    if (capacity < 154) return 0;
    memset(bytes, 0, capacity);

    /* Packed key type 0, object index 0; fixed O40 payload. */
    be16(bytes + p, 0x0000); p += 2;
    bytes[p] = 1;
    be16(bytes + p + 2, -1);
    be16(bytes + p + 4, -1);
    be16(bytes + p + 6, -1);
    be16(bytes + p + 8, 0);
    p += CAD_LEGACY_OBJECT_PAYLOAD_SIZE;

    /* Packed key type 1, polygon 0; fixed P8 payload. */
    be16(bytes + p, 0x4000); p += 2;
    bytes[p] = 1;
    be16(bytes + p + 2, -1);
    be16(bytes + p + 4, 0);
    bytes[p + 6] = 255;
    bytes[p + 7] = 3;
    p += CAD_LEGACY_POLYGON_PAYLOAD_SIZE;

    for (i = 0; i < 3; ++i) {
        be16(bytes + p, 0x8000 | i); p += 2;
        bytes[p] = 2;
        be16(bytes + p + 2, i == 2 ? -1 : i + 1);
        be_double(bytes + p + 8, xyz[i][0]);
        be_double(bytes + p + 16, xyz[i][1]);
        be_double(bytes + p + 24, xyz[i][2]);
        p += CAD_LEGACY_POINT_PAYLOAD_SIZE;
    }
    return p;
}

static void test_legacy_decode_and_transactionality(void) {
    uint8_t legacy[154];
    size_t size = make_legacy_triangle(legacy, sizeof(legacy));
    CadFileData* decoded = (CadFileData*)malloc(sizeof(*decoded));
    CadFileData* before = (CadFileData*)malloc(sizeof(*before));
    CadResult result;
    CHECK(size == sizeof(legacy));
    CHECK(decoded != NULL && before != NULL);
    if (!decoded || !before) { free(decoded); free(before); return; }
    CadFile_Init(decoded);
    decoded->points[999].flags = 77;
    *before = *decoded;
    result = CadCodec_Decode(legacy, size, CAD_FORMAT_AUTO, decoded);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(result.format == CAD_FORMAT_LEGACY_PACKED);
    CHECK(decoded->objectCount == 1 && decoded->polygonCount == 1);
    CHECK(decoded->pointCount == 3);
    CHECK(decoded->polygons[0].animation == -1);
    CHECK(decoded->polygons[0].both == -1);
    CHECK(decoded->polygons[0].color == 255);

    /* A failed decode must leave the caller's existing document untouched. */
    *before = *decoded;
    result = CadCodec_Decode(legacy, size - 1,
                             CAD_FORMAT_LEGACY_PACKED, decoded);
    CHECK(!CadResult_IsSuccess(&result));
    CHECK(result.status == CAD_STATUS_TRUNCATED_RECORD);
    CHECK(memcmp(before, decoded, sizeof(*decoded)) == 0);
    legacy[0] = 0xc0;
    result = CadCodec_Decode(legacy, size,
                             CAD_FORMAT_LEGACY_PACKED, decoded);
    CHECK(!CadResult_IsSuccess(&result));
    CHECK(memcmp(before, decoded, sizeof(*decoded)) == 0);
    free(before);
    free(decoded);
}

static void test_root_and_delete_repair(void) {
    CadCore core;
    int16_t first;
    int16_t second;
    int16_t four[4];
    int i;
    char diagnostic[192];
    CadCore_Init(&core);
    first = append_triangle(&core, 0.0, 10);
    second = append_triangle(&core, 4.0, 200);
    CHECK(core.data.objects[0].flags == 1);
    CHECK(core.data.objects[0].parentObject == -1);
    CHECK(core.data.objects[0].firstPolygon == first);
    CHECK(core.data.polygons[first].nextPolygon == second);
    CHECK(core.data.polygons[second].nextPolygon == -1);
    CHECK(core.rootPolygon == second);
    CHECK(CadCore_ValidateDocument(&core, diagnostic, sizeof(diagnostic)));

    CHECK(CadCore_DeletePolygon(&core, first));
    CHECK(core.data.objects[0].firstPolygon == second);
    CHECK(!core.data.polygons[first].flags);
    CHECK(CadCore_ValidateDocument(&core, diagnostic, sizeof(diagnostic)));

    for (i = 0; i < 4; ++i)
        four[i] = CadCore_AddPoint(&core, 10.0 + i, 0.0, 0.0);
    for (i = 0; i < 4; ++i)
        core.data.points[four[i]].nextPoint = i == 3 ? -1 : four[i + 1];
    first = CadCore_AddPolygon(&core, four[0], 32, 4);
    CHECK(first >= 0);
    CHECK(CadCore_DeletePoint(&core, four[1]));
    CHECK(core.data.polygons[first].npoints == 3);
    CHECK(core.data.points[four[0]].nextPoint == four[2]);
    CHECK(CadCore_ValidateDocument(&core, diagnostic, sizeof(diagnostic)));

    core.data.polygons[first].both = second;
    core.data.polygons[second].both = first;
    CHECK(CadCore_DeletePolygon(&core, first));
    CHECK(core.data.polygons[second].both == -1);
    CHECK(CadCore_ValidateDocument(&core, diagnostic, sizeof(diagnostic)));
    CadCore_Destroy(&core);
}

static void test_mutation_topology_guards(void) {
    CadCore core;
    int16_t a;
    int16_t b;
    int16_t c;
    int16_t d;
    int16_t e;
    int16_t polygon;
    int16_t secondPolygon;
    int16_t root;
    int16_t child;
    CadResult validation;

    CadCore_Init(&core);
    a = CadCore_AddPoint(&core, 0.0, 0.0, 0.0);
    b = CadCore_AddPoint(&core, 1.0, 0.0, 0.0);
    c = CadCore_AddPoint(&core, 0.0, 1.0, 0.0);
    core.data.points[a].nextPoint = b;
    core.data.points[b].nextPoint = -1;
    CHECK(CadCore_AddPolygon(&core, a, 1, 3) == INVALID_INDEX);
    CHECK(CadCore_GetActivePolygonCount(&core) == 0);
    core.data.points[b].nextPoint = c;
    core.data.points[c].nextPoint = -1;
    polygon = CadCore_AddPolygon(&core, a, 1, 3);
    CHECK(polygon >= 0);
    CHECK(CadCore_AddPolygon(&core, a, 2, 3) == INVALID_INDEX);
    CHECK(!CadCore_AddPointToPolygon(&core, polygon, a));

    d = CadCore_AddPoint(&core, 2.0, 0.0, 0.0);
    e = CadCore_AddPoint(&core, 2.0, 1.0, 0.0);
    core.data.points[d].nextPoint = e;
    CHECK(!CadCore_AddPointToPolygon(&core, polygon, d));
    core.data.points[d].nextPoint = -1;
    CHECK(CadCore_AddPointToPolygon(&core, polygon, d));
    CHECK(core.data.polygons[polygon].npoints == 4);
    validation = CadCodec_Validate(&core.data);
    CHECK(CadResult_IsSuccess(&validation));

    /* A point owned by a different face cannot be spliced into this one. */
    a = CadCore_AddPoint(&core, 10.0, 0.0, 0.0);
    b = CadCore_AddPoint(&core, 11.0, 0.0, 0.0);
    c = CadCore_AddPoint(&core, 10.0, 1.0, 0.0);
    core.data.points[a].nextPoint = b;
    core.data.points[b].nextPoint = c;
    core.data.points[c].nextPoint = -1;
    secondPolygon = CadCore_AddPolygon(&core, a, 3, 3);
    CHECK(secondPolygon >= 0);
    CHECK(!CadCore_AddPointToPolygon(&core, polygon, b));
    CadCore_Destroy(&core);

    CadCore_Init(&core);
    root = CadCore_AddObject(&core, INVALID_INDEX, 0.0, 0.0, 0.0);
    child = CadCore_AddObject(&core, root, 0.0, 0.0, 0.0);
    CHECK(root >= 0 && child >= 0);
    validation = CadCodec_Validate(&core.data);
    CHECK(CadResult_IsSuccess(&validation));
    core.data.objects[root].parentObject = child;
    core.data.objects[child].childObject = root;
    validation = CadCodec_Validate(&core.data);
    CHECK(!CadResult_IsSuccess(&validation));
    CHECK(CadCore_DeleteObject(&core, root));
    CHECK(CadCore_GetActiveObjectCount(&core) == 0);
    CadCore_Destroy(&core);
}

static void test_document_undo_redo_and_palette(void) {
    CadDocument* document = (CadDocument*)malloc(sizeof(*document));
    CadResult result;
    CadRgba palette[256];
    const char* savePath = "threedcad-document-revision-test.cad";
    int i;
    CHECK(document != NULL);
    if (!document) return;
    CadDocument_Init(document);

    /* The core dirty bit is derived document state, not an editable value.
       Changing it alone must remain a no-op and must not create history. */
    result = CadDocument_BeginEdit(document);
    CHECK(CadResult_IsSuccess(&result));
    document->core.isDirty = 1;
    result = CadDocument_CommitEdit(document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(!document->isDirty);
    CHECK(!document->core.isDirty);
    CHECK(!CadDocument_CanUndo(document));

    result = CadDocument_BeginEdit(document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(append_triangle(&document->core, 0.0, 250) == 0);
    result = CadDocument_CommitEdit(document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(document->isDirty);
    CHECK(CadCore_GetActivePointCount(&document->core) == 3);
    CHECK(CadDocument_CanUndo(document));
    result = CadDocument_Undo(document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(CadCore_GetActivePointCount(&document->core) == 0);
    CHECK(CadDocument_CanRedo(document));
    result = CadDocument_Redo(document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(CadCore_GetActivePointCount(&document->core) == 3);

    /* Saving identifies a particular content revision.  Undoing away from
       that revision must become dirty, while redoing exactly back is clean. */
    result = CadDocument_Save(document, savePath);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(!document->isDirty);
    result = CadDocument_Undo(document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(document->isDirty);
    result = CadDocument_Redo(document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(!document->isDirty);

    result = CadDocument_BeginEdit(document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(CadCore_AddPoint(&document->core, 9.0, 9.0, 9.0) >= 0);
    result = CadDocument_SaveCurrent(document);
    CHECK(!CadResult_IsSuccess(&result));
    CadDocument_CancelEdit(document);
    CHECK(CadCore_GetActivePointCount(&document->core) == 3);
    CHECK(!document->isDirty);

    result = CadDocument_BeginEdit(document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(CadCore_AddPoint(&document->core, 8.0, 8.0, 8.0) >= 0);
    result = CadDocument_SaveCurrent(document);
    CHECK(!CadResult_IsSuccess(&result));
    result = CadDocument_CommitEdit(document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(document->isDirty);
    result = CadDocument_Undo(document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(!document->isDirty);
    CHECK(CadCore_GetActivePointCount(&document->core) == 3);

    for (i = 0; i < 256; ++i) {
        palette[i].r = (uint8_t)i;
        palette[i].g = (uint8_t)(255 - i);
        palette[i].b = (uint8_t)(i ^ 0x55);
        palette[i].a = 255;
    }
    CHECK(CadDocument_SetPalette(document, palette, "palette-test.pal"));
    CHECK(document->paletteValid);
    CHECK(document->palette[255].r == 255);
    CHECK(document->paletteSourcePath != NULL);
    CHECK(!document->isDirty);
    CadDocument_ClearPalette(document);
    CHECK(!document->paletteValid);
    CHECK(!document->isDirty);
    remove(savePath);
    CadDocument_Destroy(document);
    free(document);
}

static void test_document_load_replacement_safety(void) {
    const char* legacyPath = "threedcad-document-legacy-test.cad";
    const char* brokenPath = "threedcad-document-broken-test.cad";
    uint8_t legacy[154];
    size_t size = make_legacy_triangle(legacy, sizeof(legacy));
    CadDocument* document = (CadDocument*)malloc(sizeof(*document));
    CadFileData* before = (CadFileData*)malloc(sizeof(*before));
    CadResult result;
    CHECK(document != NULL && before != NULL);
    if (!document || !before) {
        free(document);
        free(before);
        return;
    }
    CadDocument_Init(document);
    result = CadDocument_BeginEdit(document);
    CHECK(CadResult_IsSuccess(&result));
    append_triangle(&document->core, 20.0, 9);
    result = CadDocument_CommitEdit(document);
    CHECK(CadResult_IsSuccess(&result));
    *before = document->core.data;

    CHECK(write_test_file(brokenPath, legacy, size - 1));
    result = CadDocument_Load(document, brokenPath);
    CHECK(!CadResult_IsSuccess(&result));
    CHECK(memcmp(before, &document->core.data, sizeof(*before)) == 0);
    CHECK(document->isDirty);

    CHECK(write_test_file(legacyPath, legacy, size));
    result = CadDocument_Load(document, legacyPath);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(result.format == CAD_FORMAT_LEGACY_PACKED);
    CHECK(document->isDirty);
    CHECK(document->savePath == NULL);
    CHECK(document->lastImportPath != NULL);
    CHECK(CadCore_GetActivePointCount(&document->core) == 3);
    CadDocument_Destroy(document);
    free(document);
    free(before);
    remove(legacyPath);
    remove(brokenPath);
}

static void test_editor_tool_lifecycle(void) {
    CadDocument document;
    EditorTool tool;
    CadResult result;

    CadDocument_Init(&document);
    EditorTool_Init(&tool, &document);
    CHECK(!EditorTool_IsActive(&tool));

    result = EditorTool_Begin(&tool, CAD_TOOL_POINT_CREATE);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(EditorTool_IsActive(&tool));
    CHECK(CadCore_AddPoint(&document.core, 1.0, 2.0, 3.0) >= 0);
    result = EditorTool_Update(&tool);
    CHECK(CadResult_IsSuccess(&result));
    result = EditorTool_Commit(&tool);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(!EditorTool_IsActive(&tool));
    CHECK(CadCore_GetActivePointCount(&document.core) == 1);
    CHECK(CadDocument_CanUndo(&document));

    result = EditorTool_Begin(&tool, CAD_TOOL_POINT_CREATE);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(CadCore_AddPoint(&document.core, 4.0, 5.0, 6.0) >= 0);
    EditorTool_Cancel(&tool);
    CHECK(!EditorTool_IsActive(&tool));
    CHECK(CadCore_GetActivePointCount(&document.core) == 1);

    CadDocument_Destroy(&document);
}

int main(void) {
    test_later_animation_round_trip();
    test_legacy_decode_and_transactionality();
    test_root_and_delete_repair();
    test_mutation_topology_guards();
    test_document_undo_redo_and_palette();
    test_document_load_replacement_safety();
    test_editor_tool_lifecycle();
    if (failures) {
        fprintf(stderr, "%d core test(s) failed\n", failures);
        return 1;
    }
    puts("All core/document/codec tests passed.");
    return 0;
}
