#define _CRT_SECURE_NO_WARNINGS

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

static uint8_t* read_test_file(const char* path, size_t* size) {
    FILE* file;
    long length;
    uint8_t* bytes;
    if (size) *size = 0;
    file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    bytes = (uint8_t*)malloc((size_t)length ? (size_t)length : 1);
    if (!bytes || fread(bytes, 1, (size_t)length, file) != (size_t)length) {
        free(bytes);
        fclose(file);
        return NULL;
    }
    fclose(file);
    if (size) *size = (size_t)length;
    return bytes;
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
    core.data.polygons[polygon].side = 5;
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

    /* Native streams can contain authoring records that are not attached to
       a face.  They are inspected as non-editable but must survive a native
       decode/encode cycle without reinterpretation. */
    core.data.animationIndices[1].flags = 3;
    for (frame = 0; frame < CAD_ANIMATION_FRAMES; ++frame)
        core.data.animationIndices[1].frame[frame] = -1;
    core.data.animationIndices[1].frame[0] = 3;
    core.data.animationIndexCount = 2;
    for (frame = 3; frame < 5; ++frame) {
        core.data.animationPoints[frame].flags = 4;
        core.data.animationPoints[frame].nextPoint =
            (int16_t)(frame == 4 ? -1 : frame + 1);
        core.data.animationPoints[frame].pointx = 40.0 + frame;
        core.data.animationPoints[frame].pointy = 50.0 + frame;
        core.data.animationPoints[frame].pointz = 60.0 + frame;
    }
    core.data.animationPointCount = 5;

    result = CadCodec_Encode(&core.data, CAD_FORMAT_X11_STREAM, &bytes, &size);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(size == (size_t)(43 + 17 + 3 * 35 + 2 * 133 + 5 * 35));
    decoded = (CadFileData*)malloc(sizeof(*decoded));
    CHECK(decoded != NULL);
    if (decoded && bytes) {
        result = CadCodec_Decode(bytes, size, CAD_FORMAT_AUTO, decoded);
        CHECK(CadResult_IsSuccess(&result));
        CHECK(result.format == CAD_FORMAT_X11_STREAM);
        CHECK(decoded->polygons[0].color == 255);
        CHECK(decoded->polygons[0].side == 5);
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
        CHECK(decoded->animationIndices[1].flags == 3);
        CHECK(decoded->animationIndices[1].frame[0] == 3);
        CHECK(decoded->animationIndices[1].frame[1] == -1);
        CHECK(decoded->animationPoints[3].flags == 4);
        CHECK(decoded->animationPoints[3].nextPoint == 4);
        CHECK(fabs(decoded->animationPoints[4].pointz - 64.0) < 1e-12);
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

static size_t make_legacy_collinear_leading_face(uint8_t* bytes,
                                                 size_t capacity) {
    static const double xyz[5][3] = {
        {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {2.0, 0.0, 0.0},
        {2.0, -2.0, 0.0}, {0.0, -2.0, 0.0}
    };
    const size_t required = 2 + CAD_LEGACY_OBJECT_PAYLOAD_SIZE +
                            2 + CAD_LEGACY_POLYGON_PAYLOAD_SIZE +
                            5 * (2 + CAD_LEGACY_POINT_PAYLOAD_SIZE);
    size_t p = 0;
    int i;
    if (capacity < required) return 0;
    memset(bytes, 0, capacity);

    be16(bytes + p, 0x0000); p += 2;
    bytes[p] = 1;
    be16(bytes + p + 2, -1);
    be16(bytes + p + 4, -1);
    be16(bytes + p + 6, -1);
    be16(bytes + p + 8, 0);
    p += CAD_LEGACY_OBJECT_PAYLOAD_SIZE;

    be16(bytes + p, 0x4000); p += 2;
    bytes[p] = 1;
    be16(bytes + p + 2, -1);
    be16(bytes + p + 4, 0);
    bytes[p + 6] = 9;
    bytes[p + 7] = 5;
    p += CAD_LEGACY_POLYGON_PAYLOAD_SIZE;

    for (i = 0; i < 5; ++i) {
        be16(bytes + p, 0x8000 | i); p += 2;
        bytes[p] = 2;
        be16(bytes + p + 2, i == 4 ? -1 : i + 1);
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
    CHECK(decoded->polygons[0].side == 2);
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

static void test_legacy_collinear_leading_side_reconstruction(void) {
    uint8_t legacy[222];
    size_t size = make_legacy_collinear_leading_face(legacy, sizeof(legacy));
    CadFileData* decoded = (CadFileData*)malloc(sizeof(*decoded));
    CadResult result;
    CHECK(size == sizeof(legacy));
    CHECK(decoded != NULL);
    if (!decoded) return;
    result = CadCodec_Decode(legacy, size, CAD_FORMAT_LEGACY_PACKED, decoded);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(decoded->polygonCount == 1);
    CHECK(decoded->polygons[0].npoints == 5);
    /* The clockwise XY chain has a negative Z normal. */
    CHECK(decoded->polygons[0].side == 0);
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

static void test_polygon_color_stepping(void) {
    CadCore core;
    int16_t polygon;
    CadCore_Init(&core);
    polygon = append_triangle(&core, 0.0, 0);
    CHECK(polygon >= 0);
    core.isDirty = 0;

    CHECK(!CadCore_StepPolygonColor(NULL, polygon, 1));
    CHECK(!CadCore_StepPolygonColor(&core, INVALID_INDEX, 1));
    CHECK(!CadCore_StepPolygonColor(&core, (int16_t)CAD_MAX_POLYGONS, 1));
    CHECK(!CadCore_StepPolygonColor(&core, (int16_t)(polygon + 1), 1));
    CHECK(!CadCore_StepPolygonColor(&core, polygon, 0));
    CHECK(core.data.polygons[polygon].color == 0);
    CHECK(core.isDirty == 0);

    CHECK(CadCore_StepPolygonColor(&core, polygon, -1));
    CHECK(core.data.polygons[polygon].color == UINT8_MAX);
    CHECK(core.isDirty == 1);
    core.isDirty = 0;
    CHECK(CadCore_StepPolygonColor(&core, polygon, 1));
    CHECK(core.data.polygons[polygon].color == 0);
    CHECK(core.isDirty == 1);

    core.data.polygons[polygon].color = 10;
    core.isDirty = 0;
    CHECK(CadCore_StepPolygonColor(&core, polygon, 7));
    CHECK(core.data.polygons[polygon].color == 11);
    CHECK(CadCore_StepPolygonColor(&core, polygon, -12));
    CHECK(core.data.polygons[polygon].color == 10);
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

static void test_polygon_point_merge(void) {
    CadCore core;
    int16_t points[5];
    int16_t line[2];
    int16_t polygon;
    int16_t degenerate;
    char diagnostic[192];
    int i;
    static const double coordinates[5][3] = {
        {0.0, 0.0, 0.0},
        {0.2, 0.0, 0.0}, /* same recovered grid position as point zero */
        {2.0, 0.0, 0.0},
        {2.0, 2.0, 0.0},
        {0.4, 0.0, 0.0}  /* closing duplicate of point zero */
    };

    CadCore_Init(&core);
    for (i = 0; i < 5; ++i) {
        points[i] = CadCore_AddPoint(
            &core, coordinates[i][0], coordinates[i][1], coordinates[i][2]);
        CHECK(points[i] >= 0);
        if (i > 0) core.data.points[points[i - 1]].nextPoint = points[i];
    }
    core.data.points[points[4]].nextPoint = INVALID_INDEX;
    polygon = CadCore_AddPolygon(&core, points[0], 7, 5);
    CHECK(polygon >= 0);
    CHECK(!CadCore_ArePointsMerged(&core));
    CHECK(CadCore_MergePolygonPoints(&core) == 2);
    CHECK(core.data.polygons[polygon].npoints == 3);
    CHECK(core.data.polygons[polygon].firstPoint == points[0]);
    CHECK(core.data.points[points[0]].nextPoint == points[2]);
    CHECK(core.data.points[points[2]].nextPoint == points[3]);
    CHECK(core.data.points[points[3]].nextPoint == INVALID_INDEX);
    CHECK(!core.data.points[points[1]].flags);
    CHECK(!core.data.points[points[4]].flags);
    CHECK(CadCore_ArePointsMerged(&core));
    CHECK(CadCore_IsFullyMerged(&core));
    CHECK(CadCore_ValidateDocument(&core, diagnostic, sizeof(diagnostic)));

    line[0] = CadCore_AddPoint(&core, 10.0, 0.0, 0.0);
    line[1] = CadCore_AddPoint(&core, 10.0, 0.0, 0.0);
    CHECK(line[0] >= 0 && line[1] >= 0);
    core.data.points[line[0]].nextPoint = line[1];
    core.data.points[line[1]].nextPoint = INVALID_INDEX;
    degenerate = CadCore_AddPolygon(&core, line[0], 8, 2);
    CHECK(degenerate >= 0);
    CHECK(!CadCore_ArePointsMerged(&core));
    CHECK(CadCore_MergePolygonPoints(&core) == 2);
    CHECK(!core.data.polygons[degenerate].flags);
    CHECK(CadCore_GetActivePolygonCount(&core) == 1);
    CHECK(CadCore_IsFullyMerged(&core));
    CHECK(CadCore_ValidateDocument(&core, diagnostic, sizeof(diagnostic)));
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
    CHECK(CadDocument_SetPalette(document, palette, "palette-test.col"));
    CHECK(document->paletteValid);
    CHECK(document->palette[255].r == 255);
    CHECK(document->colSourcePath != NULL);
    CHECK(!document->isDirty);
    CadDocument_ClearPalette(document);
    CHECK(!document->paletteValid);
    CHECK(!document->isDirty);

    result = CadDocument_BeginEditNamed(document, "Load Palette A");
    CHECK(CadResult_IsSuccess(&result));
    CHECK(CadDocument_SetPalette(document, palette, "palette-a.col"));
    result = CadDocument_CommitEdit(document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(strcmp(document->colSourcePath, "palette-a.col") == 0);
    CHECK(document->palette[0].r == 0);

    palette[0].r = 82; /* exactly representable after BGR555 quantization */
    result = CadDocument_BeginEditNamed(document, "Cancelled Palette");
    CHECK(CadResult_IsSuccess(&result));
    CHECK(CadDocument_SetPalette(document, palette, "palette-cancelled.col"));
    CadDocument_CancelEdit(document);
    CHECK(strcmp(document->colSourcePath, "palette-a.col") == 0);
    CHECK(document->palette[0].r == 0);

    result = CadDocument_BeginEditNamed(document, "Load Palette B");
    CHECK(CadResult_IsSuccess(&result));
    CHECK(CadDocument_SetPalette(document, palette, "palette-b.col"));
    result = CadDocument_CommitEdit(document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(strcmp(document->colSourcePath, "palette-b.col") == 0);
    CHECK(document->palette[0].r == 82);
    result = CadDocument_Undo(document);
    CHECK(CadResult_IsSuccess(&result));
    /* File associations are resource state rather than undoable content;
       undo restores colors while retaining the current save destination. */
    CHECK(strcmp(document->colSourcePath, "palette-b.col") == 0);
    CHECK(document->palette[0].r == 0);
    result = CadDocument_Redo(document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(strcmp(document->colSourcePath, "palette-b.col") == 0);
    CHECK(document->palette[0].r == 82);

    CadDocument_MakeUnnamed(document);
    CHECK(document->sourcePath == NULL);
    CHECK(document->savePath == NULL);
    CHECK(document->isDirty);
    result = CadDocument_Undo(document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(document->isDirty);
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
    result = CadDocument_Save(document, legacyPath);
    CHECK(!CadResult_IsSuccess(&result));
    CHECK(document->savePath == NULL);
    CHECK(document->isDirty);
    CadDocument_Destroy(document);
    free(document);
    free(before);
    remove(legacyPath);
    remove(brokenPath);
}

static void test_untouched_native_document_preserves_source_bytes(void) {
    const char* sourcePath = "threedcad-native-original-test.cad";
    const char* savedPath = "threedcad-native-original-saved.cad";
    CadCore core;
    CadDocument* document = (CadDocument*)malloc(sizeof(*document));
    CadResult result;
    uint8_t* sourceBytes = NULL;
    uint8_t* savedBytes = NULL;
    size_t sourceSize = 0;
    size_t savedSize = 0;
    size_t position = 0;
    int changedReservedByte = 0;

    CHECK(document != NULL);
    if (!document) return;
    CadCore_Init(&core);
    CHECK(append_triangle(&core, 0.0, 6) >= 0);
    result = CadCodec_Encode(&core.data, CAD_FORMAT_X11_STREAM,
                             &sourceBytes, &sourceSize);
    CHECK(CadResult_IsSuccess(&result));
    while (sourceBytes && position + 3 <= sourceSize) {
        const uint8_t tag = sourceBytes[position];
        size_t payloadSize = 0;
        if (tag == CAD_TAG_OBJECT) payloadSize = CAD_X11_OBJECT_PAYLOAD_SIZE;
        else if (tag == CAD_TAG_POLYGON) payloadSize = CAD_X11_POLYGON_PAYLOAD_SIZE;
        else if (tag == CAD_TAG_POINT) payloadSize = CAD_X11_POINT_PAYLOAD_SIZE;
        else if (tag == CAD_TAG_ANIMATION_INDEX)
            payloadSize = CAD_X11_ANIMATION_INDEX_PAYLOAD_SIZE;
        else if (tag == CAD_TAG_ANIMATION_POINT)
            payloadSize = CAD_X11_ANIMATION_POINT_PAYLOAD_SIZE;
        else break;
        if (position + 3 + payloadSize > sourceSize) break;
        if (tag == CAD_TAG_POINT) {
            sourceBytes[position + 4] = 1;    /* recovered selection byte */
            sourceBytes[position + 7] = 0xA5; /* uninterpreted alignment byte */
            changedReservedByte = 1;
            break;
        }
        position += 3 + payloadSize;
    }
    CHECK(changedReservedByte);
    CHECK(write_test_file(sourcePath, sourceBytes, sourceSize));

    CadDocument_Init(document);
    result = CadDocument_Load(document, sourcePath);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(document->originalNativeBytes != NULL);
    CHECK(CadCore_IsPointSelected(&document->core, 0));
    CadCore_DeselectPoint(&document->core, 0);
    CHECK(!document->isDirty);
    result = CadDocument_Save(document, savedPath);
    CHECK(CadResult_IsSuccess(&result));
    savedBytes = read_test_file(savedPath, &savedSize);
    CHECK(savedBytes != NULL);
    CHECK(savedSize == sourceSize);
    if (savedBytes && savedSize == sourceSize)
        CHECK(memcmp(savedBytes, sourceBytes, sourceSize) == 0);

    free(savedBytes);
    savedBytes = NULL;
    result = CadDocument_BeginEditNamed(document, "Move Loaded Point");
    CHECK(CadResult_IsSuccess(&result));
    document->core.data.points[0].pointx += 4.0;
    result = CadDocument_CommitEdit(document);
    CHECK(CadResult_IsSuccess(&result));
    result = CadDocument_Save(document, savedPath);
    CHECK(CadResult_IsSuccess(&result));
    savedBytes = read_test_file(savedPath, &savedSize);
    CHECK(savedBytes != NULL);
    CHECK(savedSize == sourceSize);
    if (savedBytes && savedSize == sourceSize)
        CHECK(memcmp(savedBytes, sourceBytes, sourceSize) != 0);

    free(savedBytes);
    CadDocument_Destroy(document);
    free(document);
    CadCodec_FreeBuffer(sourceBytes);
    CadCore_Destroy(&core);
    remove(sourcePath);
    remove(savedPath);
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

static void test_named_history_and_invalid_rollback(void) {
    CadDocument document;
    CadResult result;
    uint64_t revision;
    uint64_t nextRevision;
    unsigned historyCount;
    int polygon;
    int point;

    CadDocument_Init(&document);

    /* A cancelled or rejected transaction is observationally a no-op,
       including the monotonic allocator used for future content revisions.
       This matters when a composed edit marks itself dirty before a later
       mutation fails validation. */
    revision = document.revision;
    nextRevision = document.nextRevision;
    historyCount = document.historyCount;
    result = CadDocument_BeginEditNamed(&document, "Cancelled Dirty Edit");
    CHECK(CadResult_IsSuccess(&result));
    CadDocument_MarkDirty(&document);
    CHECK(document.nextRevision > nextRevision);
    CadDocument_CancelEdit(&document);
    CHECK(document.revision == revision);
    CHECK(document.nextRevision == nextRevision);
    CHECK(document.historyCount == historyCount);
    CHECK(!document.isDirty);

    result = CadDocument_BeginEditNamed(&document, "Rejected Dirty Edit");
    CHECK(CadResult_IsSuccess(&result));
    CadDocument_MarkDirty(&document);
    polygon = append_triangle(&document.core, 0.0, 7);
    CHECK(polygon >= 0);
    point = document.core.data.polygons[polygon].firstPoint;
    document.core.data.points[point].nextPoint = (int16_t)point;
    result = CadDocument_CommitEdit(&document);
    CHECK(!CadResult_IsSuccess(&result));
    CHECK(document.revision == revision);
    CHECK(document.nextRevision == nextRevision);
    CHECK(document.historyCount == historyCount);
    CHECK(CadCore_GetActivePolygonCount(&document.core) == 0);
    CHECK(!document.isDirty);

    result = CadDocument_BeginEditNamed(&document, "Create Triangle");
    CHECK(CadResult_IsSuccess(&result));
    polygon = append_triangle(&document.core, 0.0, 7);
    CHECK(polygon >= 0);
    result = CadDocument_CommitEdit(&document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(CadDocument_GetUndoLabel(&document) != NULL);
    CHECK(strcmp(CadDocument_GetUndoLabel(&document), "Create Triangle") == 0);

    revision = document.revision;
    historyCount = document.historyCount;
    result = CadDocument_BeginEditNamed(&document, "Break Topology");
    CHECK(CadResult_IsSuccess(&result));
    point = document.core.data.polygons[polygon].firstPoint;
    CHECK(point >= 0);
    document.core.data.points[point].nextPoint = (int16_t)point;
    result = CadDocument_CommitEdit(&document);
    CHECK(!CadResult_IsSuccess(&result));
    CHECK(document.transactionBefore == NULL);
    CHECK(document.revision == revision);
    CHECK(document.historyCount == historyCount);
    CHECK(document.core.data.points[point].nextPoint != point);
    CHECK(strcmp(CadDocument_GetUndoLabel(&document), "Create Triangle") == 0);

    result = CadDocument_BeginEditNamed(&document, "Non-finite Coordinate");
    CHECK(CadResult_IsSuccess(&result));
    document.core.data.points[point].pointx = NAN;
    result = CadDocument_CommitEdit(&document);
    CHECK(!CadResult_IsSuccess(&result));
    CHECK(document.revision == revision);
    CHECK(document.historyCount == historyCount);
    CHECK(isfinite(document.core.data.points[point].pointx));

    result = CadDocument_Undo(&document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(CadDocument_GetRedoLabel(&document) != NULL);
    CHECK(strcmp(CadDocument_GetRedoLabel(&document), "Create Triangle") == 0);
    result = CadDocument_Redo(&document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(strcmp(CadDocument_GetUndoLabel(&document), "Create Triangle") == 0);
    CadDocument_Destroy(&document);
}

static CadResult append_two_points_edit(CadDocument* document,
                                        void* userData) {
    double offset = userData ? *(const double*)userData : 0.0;
    if (CadCore_AddPoint(&document->core, offset, 0.0, 0.0) < 0 ||
        CadCore_AddPoint(&document->core, offset + 1.0, 0.0, 0.0) < 0) {
        CadResult result = CadResult_Ok(CAD_FORMAT_AUTO);
        result.status = CAD_STATUS_INDEX_OUT_OF_RANGE;
        return result;
    }
    return CadResult_Ok(CAD_FORMAT_AUTO);
}

static CadResult failing_composite_edit(CadDocument* document,
                                        void* userData) {
    CadResult result = append_two_points_edit(document, userData);
    if (!CadResult_IsSuccess(&result)) return result;
    result.status = CAD_STATUS_INVALID_ARGUMENT;
    return result;
}

static void test_composable_document_edit(void) {
    CadDocument document;
    CadResult result;
    double offset = 4.0;
    CadDocument_Init(&document);
    result = CadDocument_ApplyEdit(&document, "Add Point Pair",
                                   append_two_points_edit, &offset);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(CadCore_GetActivePointCount(&document.core) == 2);
    CHECK(strcmp(CadDocument_GetUndoLabel(&document), "Add Point Pair") == 0);
    result = CadDocument_Undo(&document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(CadCore_GetActivePointCount(&document.core) == 0);
    result = CadDocument_ApplyEdit(&document, "Rejected Pair",
                                   failing_composite_edit, &offset);
    CHECK(!CadResult_IsSuccess(&result));
    CHECK(CadCore_GetActivePointCount(&document.core) == 0);
    CHECK(!CadDocument_CanUndo(&document));
    CadDocument_Destroy(&document);
}

static void test_history_capacity_eviction(void) {
    CadDocument document;
    CadResult result;
    int edit;
    int undoCount = 0;
    int redoCount = 0;
    char label[32];
    CadDocument_Init(&document);
    for (edit = 1; edit <= 70; ++edit) {
        snprintf(label, sizeof(label), "Add Point %d", edit);
        result = CadDocument_BeginEditNamed(&document, label);
        CHECK(CadResult_IsSuccess(&result));
        CHECK(CadCore_AddPoint(&document.core, (double)edit, 0.0, 0.0) >= 0);
        result = CadDocument_CommitEdit(&document);
        CHECK(CadResult_IsSuccess(&result));
    }
    CHECK(document.historyCount == CAD_DOCUMENT_HISTORY_LIMIT);
    CHECK(strcmp(CadDocument_GetUndoLabel(&document), "Add Point 70") == 0);
    while (CadDocument_CanUndo(&document)) {
        result = CadDocument_Undo(&document);
        CHECK(CadResult_IsSuccess(&result));
        ++undoCount;
    }
    CHECK(undoCount == CAD_DOCUMENT_HISTORY_LIMIT - 1);
    CHECK(CadCore_GetActivePointCount(&document.core) == 7);
    while (CadDocument_CanRedo(&document)) {
        result = CadDocument_Redo(&document);
        CHECK(CadResult_IsSuccess(&result));
        ++redoCount;
    }
    CHECK(redoCount == CAD_DOCUMENT_HISTORY_LIMIT - 1);
    CHECK(CadCore_GetActivePointCount(&document.core) == 70);
    CadDocument_Destroy(&document);
}

int main(void) {
    test_later_animation_round_trip();
    test_legacy_decode_and_transactionality();
    test_legacy_collinear_leading_side_reconstruction();
    test_root_and_delete_repair();
    test_polygon_color_stepping();
    test_mutation_topology_guards();
    test_polygon_point_merge();
    test_document_undo_redo_and_palette();
    test_document_load_replacement_safety();
    test_untouched_native_document_preserves_source_bytes();
    test_editor_tool_lifecycle();
    test_named_history_and_invalid_rollback();
    test_composable_document_edit();
    test_history_capacity_eviction();
    if (failures) {
        fprintf(stderr, "%d core test(s) failed\n", failures);
        return 1;
    }
    puts("All core/document/codec tests passed.");
    return 0;
}
