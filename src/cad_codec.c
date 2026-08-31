#define _CRT_SECURE_NO_WARNINGS

#include "cad_codec.h"
#include "cad_geometry.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct CadDecodeState {
    CadFileData data;
    uint8_t seenObjects[CAD_MAX_OBJECTS];
    uint8_t seenPolygons[CAD_MAX_POLYGONS];
    uint8_t seenPoints[CAD_MAX_POINTS];
    uint8_t seenAnimationIndices[CAD_MAX_ANIMATION_INDICES];
    uint8_t seenAnimationPoints[CAD_MAX_ANIMATION_POINTS];
    unsigned recordCount;
} CadDecodeState;

static CadResult result_make(CadStatus status, CadFormat format) {
    CadResult result;
    memset(&result, 0, sizeof(result));
    result.status = status;
    result.format = format;
    if (status != CAD_STATUS_OK) result.errorCount = 1;
    return result;
}

static void result_add(CadResult* result, CadDiagnosticSeverity severity,
                       CadStatus code, size_t offset, int tag, int index,
                       const char* format, ...) {
    CadDiagnostic* diagnostic = NULL;
    va_list args;
    if (!result) return;
    if (severity == CAD_DIAGNOSTIC_ERROR) {
        result->errorCount++;
        if (result->status == CAD_STATUS_OK) result->status = code;
    } else if (severity == CAD_DIAGNOSTIC_WARNING) {
        result->warningCount++;
    }
    if (result->diagnosticCount >= CAD_DIAGNOSTIC_CAPACITY) return;
    diagnostic = &result->diagnostics[result->diagnosticCount++];
    memset(diagnostic, 0, sizeof(*diagnostic));
    diagnostic->severity = severity;
    diagnostic->code = code;
    diagnostic->byteOffset = offset;
    diagnostic->recordTag = tag;
    diagnostic->recordIndex = index;
    va_start(args, format);
    vsnprintf(diagnostic->message, sizeof(diagnostic->message), format, args);
    va_end(args);
}

static CadResult result_error(CadFormat format, CadStatus status, size_t offset,
                              int tag, int index, const char* message) {
    CadResult result = result_make(CAD_STATUS_OK, format);
    result_add(&result, CAD_DIAGNOSTIC_ERROR, status, offset, tag, index,
               "%s", message ? message : "CAD operation failed");
    return result;
}

CadResult CadResult_Ok(CadFormat format) {
    return result_make(CAD_STATUS_OK, format);
}

int CadResult_IsSuccess(const CadResult* result) {
    return result && result->status == CAD_STATUS_OK && result->errorCount == 0;
}

const char* CadStatus_Name(CadStatus status) {
    switch (status) {
        case CAD_STATUS_OK: return "ok";
        case CAD_STATUS_INVALID_ARGUMENT: return "invalid argument";
        case CAD_STATUS_OUT_OF_MEMORY: return "out of memory";
        case CAD_STATUS_IO_ERROR: return "I/O error";
        case CAD_STATUS_EMPTY_INPUT: return "empty input";
        case CAD_STATUS_UNRECOGNIZED_FORMAT: return "unrecognized format";
        case CAD_STATUS_TRUNCATED_RECORD: return "truncated record";
        case CAD_STATUS_UNKNOWN_RECORD: return "unknown record";
        case CAD_STATUS_INDEX_OUT_OF_RANGE: return "index out of range";
        case CAD_STATUS_DUPLICATE_RECORD: return "duplicate record";
        case CAD_STATUS_INVALID_NUMBER: return "invalid number";
        case CAD_STATUS_INVALID_TOPOLOGY: return "invalid topology";
        case CAD_STATUS_UNSUPPORTED_FORMAT: return "unsupported format";
        default: return "unknown error";
    }
}

static uint16_t read_be_u16(const uint8_t* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static int16_t read_be_i16(const uint8_t* p) {
    return (int16_t)read_be_u16(p);
}

static uint64_t read_be_u64(const uint8_t* p) {
    uint64_t value = 0;
    int i;
    for (i = 0; i < 8; ++i) value = (value << 8) | p[i];
    return value;
}

static double read_be_double(const uint8_t* p) {
    uint64_t bits = read_be_u64(p);
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void write_be_u16(uint8_t* p, uint16_t value) {
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static void write_be_i16(uint8_t* p, int16_t value) {
    write_be_u16(p, (uint16_t)value);
}

static void write_be_u64(uint8_t* p, uint64_t value) {
    int i;
    for (i = 7; i >= 0; --i) {
        p[i] = (uint8_t)value;
        value >>= 8;
    }
}

static void write_be_double(uint8_t* p, double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    write_be_u64(p, bits);
}

static void update_high_water(CadDecodeState* state) {
    int i;
    state->data.objectCount = 0;
    state->data.polygonCount = 0;
    state->data.pointCount = 0;
    state->data.animationIndexCount = 0;
    state->data.animationPointCount = 0;
    for (i = 0; i < CAD_MAX_OBJECTS; ++i)
        if (state->data.objects[i].flags) state->data.objectCount = i + 1;
    for (i = 0; i < CAD_MAX_POLYGONS; ++i)
        if (state->data.polygons[i].flags) state->data.polygonCount = i + 1;
    for (i = 0; i < CAD_MAX_POINTS; ++i)
        if (state->data.points[i].flags) state->data.pointCount = i + 1;
    for (i = 0; i < CAD_MAX_ANIMATION_INDICES; ++i)
        if (state->data.animationIndices[i].flags)
            state->data.animationIndexCount = i + 1;
    for (i = 0; i < CAD_MAX_ANIMATION_POINTS; ++i)
        if (state->data.animationPoints[i].flags)
            state->data.animationPointCount = i + 1;
}

static unsigned normalize_recovered_topology(CadFileData* data) {
    CadObject* root;
    unsigned corrections = 0;
    int i;
    int havePolygon = 0;
    int allBothZero = 1;
    if (!data) return 0;
    root = &data->objects[0];
    /* Some original builds only initialized firstPolygon.  The static root's
       unused hierarchy fields therefore reached disk as zero (a self-link).
       Zero cannot be meaningful for object zero, so canonicalize it to -1. */
    if (root->flags) {
        if (root->parentObject == 0) { root->parentObject = -1; corrections++; }
        if (root->nextBrother == 0) { root->nextBrother = -1; corrections++; }
        if (root->childObject == 0) { root->childObject = -1; corrections++; }
    }
    /* Several intermediate builds left the optional paired-face field at its
       zero-initialized value for every polygon.  A table of all-zero links is
       impossible (polygon zero would pair with itself), so it is an
       evidence-backed sentinel for "no pairing" in those files. */
    for (i = 0; i < CAD_MAX_POLYGONS; ++i) {
        if (!data->polygons[i].flags) continue;
        havePolygon = 1;
        if (data->polygons[i].both != 0) allBothZero = 0;
    }
    if (havePolygon && allBothZero) {
        for (i = 0; i < CAD_MAX_POLYGONS; ++i) {
            if (data->polygons[i].flags) {
                data->polygons[i].both = -1;
                corrections++;
            }
        }
    } else {
        for (i = 0; i < CAD_MAX_POLYGONS; ++i) {
            CadPolygon* polygon = &data->polygons[i];
            int16_t both;
            if (!polygon->flags || polygon->both == -1) continue;
            both = polygon->both;
            if (both < 0 || both >= CAD_MAX_POLYGONS ||
                !data->polygons[both].flags || both == i) {
                polygon->both = -1;
                corrections++;
            }
        }
        for (i = 0; i < CAD_MAX_POLYGONS; ++i) {
            CadPolygon* polygon = &data->polygons[i];
            int16_t both;
            if (!polygon->flags || polygon->both == -1) continue;
            both = polygon->both;
            if (data->polygons[both].both == -1) {
                data->polygons[both].both = (int16_t)i;
                corrections++;
            } else if (data->polygons[both].both != i) {
                polygon->both = -1;
                corrections++;
            }
        }
    }
    return corrections;
}

static void decode_object(const uint8_t* p, CadObject* object) {
    object->flags = p[0];
    object->selectFlag = p[1];
    object->parentObject = read_be_i16(p + 2);
    object->nextBrother = read_be_i16(p + 4);
    object->childObject = read_be_i16(p + 6);
    object->firstPolygon = read_be_i16(p + 8);
    object->offsetx = read_be_double(p + 16);
    object->offsety = read_be_double(p + 24);
    object->offsetz = read_be_double(p + 32);
}

static void decode_x11_polygon(const uint8_t* p, CadPolygon* polygon) {
    polygon->flags = p[0];
    polygon->selectFlag = p[1];
    polygon->nextPolygon = read_be_i16(p + 2);
    polygon->firstPoint = read_be_i16(p + 4);
    polygon->animation = read_be_i16(p + 6);
    polygon->both = read_be_i16(p + 8);
    polygon->side = p[10];
    polygon->color = p[11];
    polygon->npoints = p[12];
}

static void decode_legacy_polygon(const uint8_t* p, CadPolygon* polygon) {
    polygon->flags = p[0];
    polygon->selectFlag = p[1];
    polygon->nextPolygon = read_be_i16(p + 2);
    polygon->firstPoint = read_be_i16(p + 4);
    polygon->animation = -1;
    polygon->both = -1;
    polygon->side = 0;
    polygon->color = p[6];
    polygon->npoints = p[7];
}

static void reconstruct_legacy_sides(CadFileData* data) {
    int polygonIndex;
    if (!data) return;
    for (polygonIndex = 0; polygonIndex < CAD_MAX_POLYGONS; ++polygonIndex) {
        CadPolygon* polygon = &data->polygons[polygonIndex];
        double coordinates[CAD_MAX_FACE_POINTS][3];
        double normal[3] = {0.0, 0.0, 0.0};
        int16_t pointIndex;
        int pointCount = 0;
        if (!polygon->flags) continue;
        pointIndex = polygon->firstPoint;
        while (pointCount < polygon->npoints &&
               pointCount < CAD_MAX_FACE_POINTS &&
               pointIndex >= 0 && pointIndex < CAD_MAX_POINTS &&
               data->points[pointIndex].flags) {
            const CadPoint* point = &data->points[pointIndex];
            coordinates[pointCount][0] = point->pointx;
            coordinates[pointCount][1] = point->pointy;
            coordinates[pointCount][2] = point->pointz;
            ++pointCount;
            pointIndex = point->nextPoint;
        }
        if (pointCount == polygon->npoints)
            (void)CadGeometry_ComputePolygonNormal(coordinates[0], pointCount,
                                                   normal);
        polygon->side = (uint8_t)((normal[1] < 0.0 ? 1 : 0) |
                                  (normal[2] >= 0.0 ? 2 : 0) |
                                  (normal[0] < 0.0 ? 4 : 0));
    }
}

static void decode_point(const uint8_t* p, CadPoint* point) {
    point->flags = p[0];
    point->selectFlag = p[1];
    point->nextPoint = read_be_i16(p + 2);
    point->pointx = read_be_double(p + 8);
    point->pointy = read_be_double(p + 16);
    point->pointz = read_be_double(p + 24);
}

static void decode_animation_index(const uint8_t* p,
                                   CadAnimationIndex* animationIndex) {
    int frame;
    animationIndex->flags = p[0];
    for (frame = 0; frame < CAD_ANIMATION_FRAMES; ++frame)
        animationIndex->frame[frame] = read_be_i16(p + 2 + frame * 2);
}

static CadResult decode_x11(const uint8_t* bytes, size_t size,
                            CadFileData* output) {
    CadDecodeState* state;
    CadResult result = CadResult_Ok(CAD_FORMAT_X11_STREAM);
    size_t position = 0;
    state = (CadDecodeState*)calloc(1, sizeof(*state));
    if (!state)
        return result_error(CAD_FORMAT_X11_STREAM, CAD_STATUS_OUT_OF_MEMORY,
                            0, -1, -1, "Could not allocate decode state");
    CadFile_Init(&state->data);

    while (position < size) {
        size_t recordOffset = position;
        uint8_t tag;
        int16_t index;
        size_t payloadSize = 0;
        uint8_t* seen = NULL;
        size_t capacity = 0;

        if (size - position < 3) {
            result = result_error(CAD_FORMAT_X11_STREAM,
                                  CAD_STATUS_TRUNCATED_RECORD, position,
                                  -1, -1, "Truncated X11 record header");
            goto done;
        }
        tag = bytes[position++];
        index = read_be_i16(bytes + position);
        position += 2;
        switch (tag) {
            case CAD_TAG_OBJECT:
                payloadSize = CAD_X11_OBJECT_PAYLOAD_SIZE;
                seen = state->seenObjects; capacity = CAD_MAX_OBJECTS; break;
            case CAD_TAG_POLYGON:
                payloadSize = CAD_X11_POLYGON_PAYLOAD_SIZE;
                seen = state->seenPolygons; capacity = CAD_MAX_POLYGONS; break;
            case CAD_TAG_POINT:
                payloadSize = CAD_X11_POINT_PAYLOAD_SIZE;
                seen = state->seenPoints; capacity = CAD_MAX_POINTS; break;
            case CAD_TAG_ANIMATION_INDEX:
                payloadSize = CAD_X11_ANIMATION_INDEX_PAYLOAD_SIZE;
                seen = state->seenAnimationIndices;
                capacity = CAD_MAX_ANIMATION_INDICES; break;
            case CAD_TAG_ANIMATION_POINT:
                payloadSize = CAD_X11_ANIMATION_POINT_PAYLOAD_SIZE;
                seen = state->seenAnimationPoints;
                capacity = CAD_MAX_ANIMATION_POINTS; break;
            default:
                result = result_error(CAD_FORMAT_X11_STREAM,
                                      CAD_STATUS_UNKNOWN_RECORD,
                                      recordOffset, tag, index,
                                      "Unknown X11 record tag");
                goto done;
        }
        if (index < 0 || (size_t)index >= capacity) {
            result = result_error(CAD_FORMAT_X11_STREAM,
                                  CAD_STATUS_INDEX_OUT_OF_RANGE,
                                  recordOffset, tag, index,
                                  "X11 record index is outside the recovered limits");
            goto done;
        }
        if (seen[index]) {
            result = result_error(CAD_FORMAT_X11_STREAM,
                                  CAD_STATUS_DUPLICATE_RECORD,
                                  recordOffset, tag, index,
                                  "Duplicate X11 record index");
            goto done;
        }
        if (size - position < payloadSize) {
            result = result_error(CAD_FORMAT_X11_STREAM,
                                  CAD_STATUS_TRUNCATED_RECORD,
                                  recordOffset, tag, index,
                                  "Truncated X11 record payload");
            goto done;
        }
        seen[index] = 1;
        switch (tag) {
            case CAD_TAG_OBJECT:
                decode_object(bytes + position, &state->data.objects[index]); break;
            case CAD_TAG_POLYGON:
                decode_x11_polygon(bytes + position,
                                   &state->data.polygons[index]); break;
            case CAD_TAG_POINT:
                decode_point(bytes + position, &state->data.points[index]); break;
            case CAD_TAG_ANIMATION_INDEX:
                decode_animation_index(bytes + position,
                                       &state->data.animationIndices[index]); break;
            case CAD_TAG_ANIMATION_POINT:
                decode_point(bytes + position,
                             &state->data.animationPoints[index]); break;
            default: break;
        }
        if ((tag == CAD_TAG_OBJECT && !state->data.objects[index].flags) ||
            (tag == CAD_TAG_POLYGON && !state->data.polygons[index].flags) ||
            (tag == CAD_TAG_POINT && !state->data.points[index].flags) ||
            (tag == CAD_TAG_ANIMATION_INDEX &&
             !state->data.animationIndices[index].flags) ||
            (tag == CAD_TAG_ANIMATION_POINT &&
             !state->data.animationPoints[index].flags)) {
            result = result_error(CAD_FORMAT_X11_STREAM,
                                  CAD_STATUS_INVALID_TOPOLOGY,
                                  recordOffset, tag, index,
                                  "Stored record is marked inactive");
            goto done;
        }
        position += payloadSize;
        state->recordCount++;
    }
    if (!state->recordCount) {
        result = result_error(CAD_FORMAT_X11_STREAM, CAD_STATUS_EMPTY_INPUT,
                              0, -1, -1, "X11 stream contains no records");
        goto done;
    }
    update_high_water(state);
    {
        unsigned corrections = normalize_recovered_topology(&state->data);
        result = CadCodec_Validate(&state->data);
        if (CadResult_IsSuccess(&result) && corrections)
            result_add(&result, CAD_DIAGNOSTIC_WARNING,
                       CAD_STATUS_INVALID_TOPOLOGY, 0, -1, -1,
                       "Canonicalized %u uninitialized or stale topology links",
                       corrections);
    }
    result.format = CAD_FORMAT_X11_STREAM;
    result.bytesConsumed = position;
    if (CadResult_IsSuccess(&result)) *output = state->data;

done:
    result.bytesConsumed = position;
    free(state);
    return result;
}

static CadResult decode_legacy(const uint8_t* bytes, size_t size,
                              CadFileData* output) {
    CadDecodeState* state;
    CadResult result = CadResult_Ok(CAD_FORMAT_LEGACY_PACKED);
    size_t position = 0;
    state = (CadDecodeState*)calloc(1, sizeof(*state));
    if (!state)
        return result_error(CAD_FORMAT_LEGACY_PACKED,
                            CAD_STATUS_OUT_OF_MEMORY, 0, -1, -1,
                            "Could not allocate decode state");
    CadFile_Init(&state->data);

    while (position < size) {
        size_t recordOffset = position;
        uint16_t key;
        unsigned tag;
        unsigned index;
        size_t payloadSize;
        uint8_t* seen;
        size_t capacity;
        if (size - position < 2) {
            result = result_error(CAD_FORMAT_LEGACY_PACKED,
                                  CAD_STATUS_TRUNCATED_RECORD, position,
                                  -1, -1, "Truncated legacy record key");
            goto done;
        }
        key = read_be_u16(bytes + position);
        position += 2;
        tag = key >> 14;
        index = key & 0x3fffu;
        if (tag == 0) {
            payloadSize = CAD_LEGACY_OBJECT_PAYLOAD_SIZE;
            seen = state->seenObjects; capacity = CAD_MAX_OBJECTS;
        } else if (tag == 1) {
            payloadSize = CAD_LEGACY_POLYGON_PAYLOAD_SIZE;
            seen = state->seenPolygons; capacity = CAD_MAX_POLYGONS;
        } else if (tag == 2) {
            payloadSize = CAD_LEGACY_POINT_PAYLOAD_SIZE;
            seen = state->seenPoints; capacity = CAD_MAX_POINTS;
        } else {
            result = result_error(CAD_FORMAT_LEGACY_PACKED,
                                  CAD_STATUS_UNKNOWN_RECORD, recordOffset,
                                  (int)tag, (int)index,
                                  "Unsupported legacy packed record type");
            goto done;
        }
        if (index >= capacity) {
            result = result_error(CAD_FORMAT_LEGACY_PACKED,
                                  CAD_STATUS_INDEX_OUT_OF_RANGE, recordOffset,
                                  (int)tag, (int)index,
                                  "Legacy record index is outside editor limits");
            goto done;
        }
        if (seen[index]) {
            result = result_error(CAD_FORMAT_LEGACY_PACKED,
                                  CAD_STATUS_DUPLICATE_RECORD, recordOffset,
                                  (int)tag, (int)index,
                                  "Duplicate legacy record index");
            goto done;
        }
        if (size - position < payloadSize) {
            result = result_error(CAD_FORMAT_LEGACY_PACKED,
                                  CAD_STATUS_TRUNCATED_RECORD, recordOffset,
                                  (int)tag, (int)index,
                                  "Truncated legacy record payload");
            goto done;
        }
        seen[index] = 1;
        if (tag == 0)
            decode_object(bytes + position, &state->data.objects[index]);
        else if (tag == 1)
            decode_legacy_polygon(bytes + position,
                                  &state->data.polygons[index]);
        else
            decode_point(bytes + position, &state->data.points[index]);

        if ((tag == 0 && !state->data.objects[index].flags) ||
            (tag == 1 && !state->data.polygons[index].flags) ||
            (tag == 2 && !state->data.points[index].flags)) {
            result = result_error(CAD_FORMAT_LEGACY_PACKED,
                                  CAD_STATUS_INVALID_TOPOLOGY, recordOffset,
                                  (int)tag, (int)index,
                                  "Stored legacy record is marked inactive");
            goto done;
        }
        position += payloadSize;
        state->recordCount++;
    }
    if (!state->recordCount) {
        result = result_error(CAD_FORMAT_LEGACY_PACKED,
                              CAD_STATUS_EMPTY_INPUT, 0, -1, -1,
                              "Legacy stream contains no records");
        goto done;
    }
    update_high_water(state);
    {
        unsigned corrections = normalize_recovered_topology(&state->data);
        reconstruct_legacy_sides(&state->data);
        result = CadCodec_Validate(&state->data);
        if (CadResult_IsSuccess(&result) && corrections)
            result_add(&result, CAD_DIAGNOSTIC_WARNING,
                       CAD_STATUS_INVALID_TOPOLOGY, 0, -1, -1,
                       "Canonicalized %u uninitialized or stale topology links",
                       corrections);
    }
    result.format = CAD_FORMAT_LEGACY_PACKED;
    result.bytesConsumed = position;
    if (CadResult_IsSuccess(&result)) *output = state->data;

done:
    result.bytesConsumed = position;
    free(state);
    return result;
}

static int valid_link(int16_t index, int capacity) {
    return index == -1 || (index >= 0 && index < capacity);
}

static int active_object(const CadFileData* data, int16_t index) {
    return index >= 0 && index < CAD_MAX_OBJECTS && data->objects[index].flags;
}

static int active_polygon(const CadFileData* data, int16_t index) {
    return index >= 0 && index < CAD_MAX_POLYGONS &&
           data->polygons[index].flags;
}

static int active_point(const CadFileData* data, int16_t index) {
    return index >= 0 && index < CAD_MAX_POINTS && data->points[index].flags;
}

static int active_animation_index(const CadFileData* data, int16_t index) {
    return index >= 0 && index < CAD_MAX_ANIMATION_INDICES &&
           data->animationIndices[index].flags;
}

static int active_animation_point(const CadFileData* data, int16_t index) {
    return index >= 0 && index < CAD_MAX_ANIMATION_POINTS &&
           data->animationPoints[index].flags;
}

CadResult CadCodec_Validate(const CadFileData* data) {
    CadResult result = CadResult_Ok(CAD_FORMAT_AUTO);
    uint16_t pointOwners[CAD_MAX_POINTS];
    uint16_t polygonOwners[CAD_MAX_POLYGONS];
    uint16_t objectChildOwners[CAD_MAX_OBJECTS];
    uint8_t objectVisited[CAD_MAX_OBJECTS];
    uint16_t animationIndexOwners[CAD_MAX_ANIMATION_INDICES];
    uint32_t animationPointOwners[CAD_MAX_ANIMATION_POINTS];
    uint8_t animationVisited[CAD_MAX_ANIMATION_POINTS];
    int globalAnimationFrameCount = -1;
    int i;
    memset(pointOwners, 0, sizeof(pointOwners));
    memset(polygonOwners, 0, sizeof(polygonOwners));
    memset(objectChildOwners, 0, sizeof(objectChildOwners));
    memset(animationIndexOwners, 0, sizeof(animationIndexOwners));
    memset(animationPointOwners, 0, sizeof(animationPointOwners));
    if (!data)
        return result_error(CAD_FORMAT_AUTO, CAD_STATUS_INVALID_ARGUMENT,
                            0, -1, -1, "CAD data is NULL");
    if (data->objectCount < 0 || data->objectCount > CAD_MAX_OBJECTS ||
        data->polygonCount < 0 || data->polygonCount > CAD_MAX_POLYGONS ||
        data->pointCount < 0 || data->pointCount > CAD_MAX_POINTS ||
        data->animationIndexCount < 0 ||
        data->animationIndexCount > CAD_MAX_ANIMATION_INDICES ||
        data->animationPointCount < 0 ||
        data->animationPointCount > CAD_MAX_ANIMATION_POINTS)
        return result_error(CAD_FORMAT_AUTO, CAD_STATUS_INDEX_OUT_OF_RANGE,
                            0, -1, -1, "A high-water count exceeds capacity");

    for (i = 0; i < CAD_MAX_OBJECTS; ++i) {
        const CadObject* object = &data->objects[i];
        if (!object->flags) continue;
        if (!isfinite(object->offsetx) || !isfinite(object->offsety) ||
            !isfinite(object->offsetz)) {
            result_add(&result, CAD_DIAGNOSTIC_ERROR,
                       CAD_STATUS_INVALID_NUMBER, 0, CAD_TAG_OBJECT, i,
                       "Object %d contains a non-finite offset", i);
            return result;
        }
        if (!valid_link(object->parentObject, CAD_MAX_OBJECTS) ||
            !valid_link(object->nextBrother, CAD_MAX_OBJECTS) ||
            !valid_link(object->childObject, CAD_MAX_OBJECTS) ||
            !valid_link(object->firstPolygon, CAD_MAX_POLYGONS)) {
            result_add(&result, CAD_DIAGNOSTIC_ERROR,
                       CAD_STATUS_INVALID_TOPOLOGY, 0, CAD_TAG_OBJECT, i,
                       "Object %d contains an out-of-range link", i);
            return result;
        }
        if ((object->parentObject != -1 &&
             !active_object(data, object->parentObject)) ||
            (object->nextBrother != -1 &&
             !active_object(data, object->nextBrother)) ||
            (object->childObject != -1 &&
             !active_object(data, object->childObject)) ||
            (object->firstPolygon != -1 &&
             !active_polygon(data, object->firstPolygon))) {
            result_add(&result, CAD_DIAGNOSTIC_ERROR,
                       CAD_STATUS_INVALID_TOPOLOGY, 0, CAD_TAG_OBJECT, i,
                       "Object %d links to an inactive record", i);
            return result;
        }
        if (object->parentObject == i || object->nextBrother == i ||
            object->childObject == i) {
            result_add(&result, CAD_DIAGNOSTIC_ERROR,
                       CAD_STATUS_INVALID_TOPOLOGY, 0, CAD_TAG_OBJECT, i,
                       "Object %d contains a self-link", i);
            return result;
        }
        if (object->nextBrother != -1 &&
            data->objects[object->nextBrother].parentObject !=
                object->parentObject) {
            result_add(&result, CAD_DIAGNOSTIC_ERROR,
                       CAD_STATUS_INVALID_TOPOLOGY, 0, CAD_TAG_OBJECT, i,
                       "Object %d has a sibling with a different parent", i);
            return result;
        }
    }

    for (i = 0; i < CAD_MAX_OBJECTS; ++i) {
        int16_t child;
        int steps = 0;
        if (!data->objects[i].flags) continue;
        memset(objectVisited, 0, sizeof(objectVisited));
        child = data->objects[i].childObject;
        while (child != -1 && steps++ < CAD_MAX_OBJECTS) {
            if (!active_object(data, child) || objectVisited[child]) {
                result_add(&result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_INVALID_TOPOLOGY, 0,
                           CAD_TAG_OBJECT, i,
                           "Object %d has a broken or cyclic child/sibling chain", i);
                return result;
            }
            objectVisited[child] = 1;
            if (data->objects[child].parentObject != i) {
                result_add(&result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_INVALID_TOPOLOGY, 0,
                           CAD_TAG_OBJECT, child,
                           "Object %d is not reciprocal with parent %d", child, i);
                return result;
            }
            if (++objectChildOwners[child] > 1) {
                result_add(&result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_INVALID_TOPOLOGY, 0,
                           CAD_TAG_OBJECT, child,
                           "Object %d belongs to multiple child chains", child);
                return result;
            }
            child = data->objects[child].nextBrother;
        }
        if (child != -1) {
            result_add(&result, CAD_DIAGNOSTIC_ERROR,
                       CAD_STATUS_INVALID_TOPOLOGY, 0, CAD_TAG_OBJECT, i,
                       "Object %d child/sibling chain exceeds capacity", i);
            return result;
        }
    }

    for (i = 0; i < CAD_MAX_OBJECTS; ++i) {
        int16_t current;
        int steps;
        if (!data->objects[i].flags) continue;
        if ((data->objects[i].parentObject == -1 && objectChildOwners[i]) ||
            (data->objects[i].parentObject != -1 &&
             objectChildOwners[i] != 1)) {
            result_add(&result, CAD_DIAGNOSTIC_ERROR,
                       CAD_STATUS_INVALID_TOPOLOGY, 0, CAD_TAG_OBJECT, i,
                       "Object %d is not owned by its declared parent", i);
            return result;
        }
        memset(objectVisited, 0, sizeof(objectVisited));
        current = (int16_t)i;
        steps = 0;
        while (current != -1 && steps++ < CAD_MAX_OBJECTS) {
            if (!active_object(data, current) || objectVisited[current]) {
                result_add(&result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_INVALID_TOPOLOGY, 0,
                           CAD_TAG_OBJECT, i,
                           "Object %d belongs to a cyclic parent hierarchy", i);
                return result;
            }
            objectVisited[current] = 1;
            current = data->objects[current].parentObject;
        }
        if (current != -1) {
            result_add(&result, CAD_DIAGNOSTIC_ERROR,
                       CAD_STATUS_INVALID_TOPOLOGY, 0, CAD_TAG_OBJECT, i,
                       "Object %d parent hierarchy exceeds capacity", i);
            return result;
        }

        memset(objectVisited, 0, sizeof(objectVisited));
        current = (int16_t)i;
        steps = 0;
        while (current != -1 && steps++ < CAD_MAX_OBJECTS) {
            if (!active_object(data, current) || objectVisited[current]) {
                result_add(&result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_INVALID_TOPOLOGY, 0,
                           CAD_TAG_OBJECT, i,
                           "Object %d belongs to a cyclic sibling chain", i);
                return result;
            }
            objectVisited[current] = 1;
            current = data->objects[current].nextBrother;
        }
        if (current != -1) {
            result_add(&result, CAD_DIAGNOSTIC_ERROR,
                       CAD_STATUS_INVALID_TOPOLOGY, 0, CAD_TAG_OBJECT, i,
                       "Object %d sibling chain exceeds capacity", i);
            return result;
        }
    }

    for (i = 0; i < CAD_MAX_POLYGONS; ++i) {
        const CadPolygon* polygon = &data->polygons[i];
        uint8_t visited[CAD_MAX_POINTS];
        int16_t point;
        int count = 0;
        if (!polygon->flags) continue;
        if (polygon->npoints < CAD_MIN_FACE_POINTS ||
            polygon->npoints > CAD_MAX_FACE_POINTS) {
            result_add(&result, CAD_DIAGNOSTIC_ERROR,
                       CAD_STATUS_INVALID_TOPOLOGY, 0, CAD_TAG_POLYGON, i,
                       "Polygon %d has %u points; supported range is %d..%d",
                       i, (unsigned)polygon->npoints, CAD_MIN_FACE_POINTS,
                       CAD_MAX_FACE_POINTS);
            return result;
        }
        if (!active_point(data, polygon->firstPoint) ||
            (polygon->nextPolygon != -1 &&
             !active_polygon(data, polygon->nextPolygon)) ||
            (polygon->both != -1 && !active_polygon(data, polygon->both)) ||
            (polygon->animation != -1 &&
             !active_animation_index(data, polygon->animation))) {
            result_add(&result, CAD_DIAGNOSTIC_ERROR,
                       CAD_STATUS_INVALID_TOPOLOGY, 0, CAD_TAG_POLYGON, i,
                       "Polygon %d links to an inactive record", i);
            return result;
        }
        if (polygon->nextPolygon == i || polygon->both == i) {
            result_add(&result, CAD_DIAGNOSTIC_ERROR,
                       CAD_STATUS_INVALID_TOPOLOGY, 0, CAD_TAG_POLYGON, i,
                       "Polygon %d contains a self-link", i);
            return result;
        }
        if (polygon->both != -1 &&
            data->polygons[polygon->both].both != i) {
            result_add(&result, CAD_DIAGNOSTIC_ERROR,
                       CAD_STATUS_INVALID_TOPOLOGY, 0, CAD_TAG_POLYGON, i,
                       "Polygon %d has a non-reciprocal paired-face link", i);
            return result;
        }
        memset(visited, 0, sizeof(visited));
        point = polygon->firstPoint;
        while (point != -1 && count < polygon->npoints) {
            if (!active_point(data, point) || visited[point]) {
                result_add(&result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_INVALID_TOPOLOGY, 0,
                           CAD_TAG_POLYGON, i,
                           "Polygon %d has a broken or cyclic point chain", i);
                return result;
            }
            visited[point] = 1;
            pointOwners[point]++;
            point = data->points[point].nextPoint;
            count++;
        }
        if (count != polygon->npoints || point != -1) {
            result_add(&result, CAD_DIAGNOSTIC_ERROR,
                       CAD_STATUS_INVALID_TOPOLOGY, 0, CAD_TAG_POLYGON, i,
                       "Polygon %d point count does not match its chain", i);
            return result;
        }
    }

    for (i = 0; i < CAD_MAX_POINTS; ++i) {
        const CadPoint* point = &data->points[i];
        if (!point->flags) continue;
        if (!isfinite(point->pointx) || !isfinite(point->pointy) ||
            !isfinite(point->pointz)) {
            result_add(&result, CAD_DIAGNOSTIC_ERROR,
                       CAD_STATUS_INVALID_NUMBER, 0, CAD_TAG_POINT, i,
                       "Point %d contains a non-finite coordinate", i);
            return result;
        }
        if (!valid_link(point->nextPoint, CAD_MAX_POINTS) ||
            (point->nextPoint != -1 && !active_point(data, point->nextPoint))) {
            result_add(&result, CAD_DIAGNOSTIC_ERROR,
                       CAD_STATUS_INVALID_TOPOLOGY, 0, CAD_TAG_POINT, i,
                       "Point %d links to an inactive or invalid point", i);
            return result;
        }
        if (pointOwners[i] > 1) {
            result_add(&result, CAD_DIAGNOSTIC_ERROR,
                       CAD_STATUS_INVALID_TOPOLOGY, 0, CAD_TAG_POINT, i,
                       "Point record %d belongs to multiple polygon chains", i);
            return result;
        }
        if (!pointOwners[i])
            result_add(&result, CAD_DIAGNOSTIC_WARNING,
                       CAD_STATUS_INVALID_TOPOLOGY, 0, CAD_TAG_POINT, i,
                       "Point %d is not attached to a polygon", i);
    }

    for (i = 0; i < CAD_MAX_OBJECTS; ++i) {
        int16_t polygon;
        int steps = 0;
        if (!data->objects[i].flags) continue;
        polygon = data->objects[i].firstPolygon;
        while (polygon != -1 && steps < CAD_MAX_POLYGONS) {
            if (!active_polygon(data, polygon)) {
                result_add(&result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_INVALID_TOPOLOGY, 0,
                           CAD_TAG_OBJECT, i,
                           "Object %d has a broken polygon chain", i);
                return result;
            }
            if (++polygonOwners[polygon] > 1) {
                result_add(&result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_INVALID_TOPOLOGY, 0,
                           CAD_TAG_POLYGON, polygon,
                           "Polygon %d belongs to multiple object chains",
                           polygon);
                return result;
            }
            polygon = data->polygons[polygon].nextPolygon;
            steps++;
        }
        if (steps == CAD_MAX_POLYGONS && polygon != -1) {
            result_add(&result, CAD_DIAGNOSTIC_ERROR,
                       CAD_STATUS_INVALID_TOPOLOGY, 0, CAD_TAG_OBJECT, i,
                       "Object %d polygon chain is cyclic", i);
            return result;
        }
    }
    for (i = 0; i < CAD_MAX_POLYGONS; ++i) {
        if (data->polygons[i].flags && !polygonOwners[i])
            result_add(&result, CAD_DIAGNOSTIC_WARNING,
                       CAD_STATUS_INVALID_TOPOLOGY, 0, CAD_TAG_POLYGON, i,
                       "Polygon %d is not attached to an object", i);
    }

    for (i = 0; i < CAD_MAX_ANIMATION_INDICES; ++i) {
        int frame;
        if (!data->animationIndices[i].flags) continue;
        for (frame = 0; frame < CAD_ANIMATION_FRAMES; ++frame) {
            int16_t point = data->animationIndices[i].frame[frame];
            if (point != -1 && !active_animation_point(data, point)) {
                result_add(&result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_INVALID_TOPOLOGY, 0,
                           CAD_TAG_ANIMATION_INDEX, i,
                           "Animation index %d frame %d references an inactive point",
                           i, frame);
                return result;
            }
        }
    }
    for (i = 0; i < CAD_MAX_ANIMATION_POINTS; ++i) {
        const CadAnimationPoint* point = &data->animationPoints[i];
        if (!point->flags) continue;
        if (!isfinite(point->pointx) || !isfinite(point->pointy) ||
            !isfinite(point->pointz) ||
            (point->nextPoint != -1 &&
             !active_animation_point(data, point->nextPoint))) {
            result_add(&result, CAD_DIAGNOSTIC_ERROR,
                       CAD_STATUS_INVALID_TOPOLOGY, 0,
                       CAD_TAG_ANIMATION_POINT, i,
                       "Animation point %d is invalid", i);
            return result;
        }
    }
    /* A populated frame is a point chain for one polygon, not merely a valid
       head reference.  Enforce the same exact-length and acyclic invariants
       as the polygon's static point chain so malformed animation data cannot
       be accepted and later round-tripped as if it were trustworthy. */
    for (i = 0; i < CAD_MAX_POLYGONS; ++i) {
        const CadPolygon* polygon = &data->polygons[i];
        int frame;
        int frameCount = 0;
        int frameSequenceEnded = 0;
        if (!polygon->flags || polygon->animation == -1) continue;
        if (++animationIndexOwners[polygon->animation] > 1) {
            result_add(&result, CAD_DIAGNOSTIC_ERROR,
                       CAD_STATUS_INVALID_TOPOLOGY, 0,
                       CAD_TAG_ANIMATION_INDEX, polygon->animation,
                       "Animation index %d is shared by multiple polygons",
                       polygon->animation);
            return result;
        }
        for (frame = 0; frame < CAD_ANIMATION_FRAMES; ++frame) {
            int16_t point = data->animationIndices[polygon->animation].frame[frame];
            int count = 0;
            if (point == -1) {
                frameSequenceEnded = 1;
                continue;
            }
            if (frameSequenceEnded) {
                result_add(&result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_INVALID_TOPOLOGY, 0,
                           CAD_TAG_ANIMATION_INDEX, polygon->animation,
                           "Polygon %d animation frames are not a contiguous prefix",
                           i);
                return result;
            }
            frameCount++;
            memset(animationVisited, 0, sizeof(animationVisited));
            while (point != -1 && count < polygon->npoints) {
                if (!active_animation_point(data, point) ||
                    animationVisited[point]) {
                    result_add(&result, CAD_DIAGNOSTIC_ERROR,
                               CAD_STATUS_INVALID_TOPOLOGY, 0,
                               CAD_TAG_ANIMATION_INDEX, polygon->animation,
                               "Polygon %d frame %d has a broken or cyclic animation chain",
                               i, frame);
                    return result;
                }
                animationVisited[point] = 1;
                if (++animationPointOwners[point] > 1) {
                    result_add(&result, CAD_DIAGNOSTIC_ERROR,
                               CAD_STATUS_INVALID_TOPOLOGY, 0,
                               CAD_TAG_ANIMATION_POINT, point,
                               "Animation point %d is shared by multiple frame chains",
                               point);
                    return result;
                }
                point = data->animationPoints[point].nextPoint;
                count++;
            }
            if (count != polygon->npoints || point != -1) {
                result_add(&result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_INVALID_TOPOLOGY, 0,
                           CAD_TAG_ANIMATION_INDEX, polygon->animation,
                           "Polygon %d frame %d animation point count does not match its face",
                           i, frame);
                return result;
            }
        }
        if (!frameCount) {
            result_add(&result, CAD_DIAGNOSTIC_ERROR,
                       CAD_STATUS_INVALID_TOPOLOGY, 0,
                       CAD_TAG_ANIMATION_INDEX, polygon->animation,
                       "Polygon %d has an animation index with no frames", i);
            return result;
        }
        if (globalAnimationFrameCount == -1)
            globalAnimationFrameCount = frameCount;
        else if (globalAnimationFrameCount != frameCount) {
            result_add(&result, CAD_DIAGNOSTIC_ERROR,
                       CAD_STATUS_INVALID_TOPOLOGY, 0,
                       CAD_TAG_ANIMATION_INDEX, polygon->animation,
                       "Polygon %d has %d frames; animated polygons must share %d frames",
                       i, frameCount, globalAnimationFrameCount);
            return result;
        }
    }
    for (i = 0; i < CAD_MAX_ANIMATION_INDICES; ++i)
        if (data->animationIndices[i].flags && !animationIndexOwners[i])
            result_add(&result, CAD_DIAGNOSTIC_WARNING,
                       CAD_STATUS_INVALID_TOPOLOGY, 0,
                       CAD_TAG_ANIMATION_INDEX, i,
                       "Animation index %d is not attached to a polygon", i);
    for (i = 0; i < CAD_MAX_ANIMATION_POINTS; ++i)
        if (data->animationPoints[i].flags && !animationPointOwners[i])
            result_add(&result, CAD_DIAGNOSTIC_WARNING,
                       CAD_STATUS_INVALID_TOPOLOGY, 0,
                       CAD_TAG_ANIMATION_POINT, i,
                       "Animation point %d is not attached to a frame", i);
    return result;
}

CadResult CadCodec_Decode(const uint8_t* bytes, size_t size,
                          CadFormat requestedFormat, CadFileData* output) {
    CadFileData* candidate;
    CadResult x11Result;
    CadResult legacyResult;
    CadResult result;
    if (!bytes || !output)
        return result_error(requestedFormat, CAD_STATUS_INVALID_ARGUMENT,
                            0, -1, -1, "Decode requires a buffer and output");
    if (!size)
        return result_error(requestedFormat, CAD_STATUS_EMPTY_INPUT,
                            0, -1, -1, "CAD input is empty");
    candidate = (CadFileData*)malloc(sizeof(*candidate));
    if (!candidate)
        return result_error(requestedFormat, CAD_STATUS_OUT_OF_MEMORY,
                            0, -1, -1, "Could not allocate decode candidate");

    if (requestedFormat == CAD_FORMAT_X11_STREAM) {
        result = decode_x11(bytes, size, candidate);
        if (CadResult_IsSuccess(&result)) *output = *candidate;
        free(candidate);
        return result;
    }
    if (requestedFormat == CAD_FORMAT_LEGACY_PACKED) {
        result = decode_legacy(bytes, size, candidate);
        if (CadResult_IsSuccess(&result)) *output = *candidate;
        free(candidate);
        return result;
    }
    if (requestedFormat != CAD_FORMAT_AUTO) {
        free(candidate);
        return result_error(requestedFormat, CAD_STATUS_UNSUPPORTED_FORMAT,
                            0, -1, -1, "Unsupported requested CAD format");
    }

    x11Result = decode_x11(bytes, size, candidate);
    if (CadResult_IsSuccess(&x11Result)) {
        *output = *candidate;
        free(candidate);
        return x11Result;
    }
    legacyResult = decode_legacy(bytes, size, candidate);
    if (CadResult_IsSuccess(&legacyResult)) {
        *output = *candidate;
        free(candidate);
        return legacyResult;
    }
    free(candidate);
    result = result_error(CAD_FORMAT_AUTO, CAD_STATUS_UNRECOGNIZED_FORMAT,
                          0, -1, -1,
                          "Input is neither a valid later-X11 nor legacy packed CAD stream");
    if (x11Result.diagnosticCount)
        result_add(&result, CAD_DIAGNOSTIC_INFO, x11Result.status,
                   x11Result.diagnostics[0].byteOffset,
                   x11Result.diagnostics[0].recordTag,
                   x11Result.diagnostics[0].recordIndex,
                   "X11 probe: %s", x11Result.diagnostics[0].message);
    if (legacyResult.diagnosticCount)
        result_add(&result, CAD_DIAGNOSTIC_INFO, legacyResult.status,
                   legacyResult.diagnostics[0].byteOffset,
                   legacyResult.diagnostics[0].recordTag,
                   legacyResult.diagnostics[0].recordIndex,
                   "Legacy probe: %s", legacyResult.diagnostics[0].message);
    return result;
}

static void encode_object(uint8_t* p, const CadObject* object) {
    memset(p, 0, CAD_X11_OBJECT_PAYLOAD_SIZE);
    p[0] = object->flags; p[1] = 0;
    write_be_i16(p + 2, object->parentObject);
    write_be_i16(p + 4, object->nextBrother);
    write_be_i16(p + 6, object->childObject);
    write_be_i16(p + 8, object->firstPolygon);
    write_be_double(p + 16, object->offsetx);
    write_be_double(p + 24, object->offsety);
    write_be_double(p + 32, object->offsetz);
}

static void encode_polygon(uint8_t* p, const CadPolygon* polygon) {
    memset(p, 0, CAD_X11_POLYGON_PAYLOAD_SIZE);
    p[0] = polygon->flags; p[1] = 0;
    write_be_i16(p + 2, polygon->nextPolygon);
    write_be_i16(p + 4, polygon->firstPoint);
    write_be_i16(p + 6, polygon->animation);
    write_be_i16(p + 8, polygon->both);
    p[10] = polygon->side; p[11] = polygon->color;
    p[12] = polygon->npoints;
}

static void encode_point(uint8_t* p, const CadPoint* point) {
    memset(p, 0, CAD_X11_POINT_PAYLOAD_SIZE);
    p[0] = point->flags; p[1] = 0;
    write_be_i16(p + 2, point->nextPoint);
    write_be_double(p + 8, point->pointx);
    write_be_double(p + 16, point->pointy);
    write_be_double(p + 24, point->pointz);
}

static void encode_animation_index(uint8_t* p,
                                   const CadAnimationIndex* index) {
    int frame;
    memset(p, 0, CAD_X11_ANIMATION_INDEX_PAYLOAD_SIZE);
    p[0] = index->flags;
    for (frame = 0; frame < CAD_ANIMATION_FRAMES; ++frame)
        write_be_i16(p + 2 + frame * 2, index->frame[frame]);
}

CadResult CadCodec_Encode(const CadFileData* data, CadFormat format,
                          uint8_t** outputBytes, size_t* outputSize) {
    CadResult result;
    size_t size = 0;
    size_t position = 0;
    uint8_t* bytes;
    int i;
    if (!data || !outputBytes || !outputSize)
        return result_error(format, CAD_STATUS_INVALID_ARGUMENT, 0, -1, -1,
                            "Encode requires data and output pointers");
    *outputBytes = NULL;
    *outputSize = 0;
    if (format == CAD_FORMAT_AUTO) format = CAD_FORMAT_X11_STREAM;
    if (format != CAD_FORMAT_X11_STREAM)
        return result_error(format, CAD_STATUS_UNSUPPORTED_FORMAT, 0, -1, -1,
                            "Only the later X11 stream is a supported save format");
    result = CadCodec_Validate(data);
    result.format = format;
    if (!CadResult_IsSuccess(&result)) return result;
    for (i = 0; i < CAD_MAX_OBJECTS; ++i)
        if (data->objects[i].flags) size += 3 + CAD_X11_OBJECT_PAYLOAD_SIZE;
    for (i = 0; i < CAD_MAX_POLYGONS; ++i)
        if (data->polygons[i].flags) size += 3 + CAD_X11_POLYGON_PAYLOAD_SIZE;
    for (i = 0; i < CAD_MAX_POINTS; ++i)
        if (data->points[i].flags) size += 3 + CAD_X11_POINT_PAYLOAD_SIZE;
    for (i = 0; i < CAD_MAX_ANIMATION_INDICES; ++i)
        if (data->animationIndices[i].flags)
            size += 3 + CAD_X11_ANIMATION_INDEX_PAYLOAD_SIZE;
    for (i = 0; i < CAD_MAX_ANIMATION_POINTS; ++i)
        if (data->animationPoints[i].flags)
            size += 3 + CAD_X11_ANIMATION_POINT_PAYLOAD_SIZE;
    if (!size)
        return result_error(format, CAD_STATUS_EMPTY_INPUT, 0, -1, -1,
                            "An empty document has no CAD records to save");
    bytes = (uint8_t*)malloc(size);
    if (!bytes)
        return result_error(format, CAD_STATUS_OUT_OF_MEMORY, 0, -1, -1,
                            "Could not allocate encoded CAD stream");

#define WRITE_RECORD(recordTag, recordIndex, payloadSize, encoder, recordPtr) \
    do { \
        bytes[position++] = (uint8_t)(recordTag); \
        write_be_i16(bytes + position, (int16_t)(recordIndex)); \
        position += 2; \
        encoder(bytes + position, (recordPtr)); \
        position += (payloadSize); \
    } while (0)

    for (i = 0; i < CAD_MAX_OBJECTS; ++i)
        if (data->objects[i].flags)
            WRITE_RECORD(CAD_TAG_OBJECT, i, CAD_X11_OBJECT_PAYLOAD_SIZE,
                         encode_object, &data->objects[i]);
    for (i = 0; i < CAD_MAX_POLYGONS; ++i)
        if (data->polygons[i].flags)
            WRITE_RECORD(CAD_TAG_POLYGON, i, CAD_X11_POLYGON_PAYLOAD_SIZE,
                         encode_polygon, &data->polygons[i]);
    for (i = 0; i < CAD_MAX_POINTS; ++i)
        if (data->points[i].flags)
            WRITE_RECORD(CAD_TAG_POINT, i, CAD_X11_POINT_PAYLOAD_SIZE,
                         encode_point, &data->points[i]);
    for (i = 0; i < CAD_MAX_ANIMATION_INDICES; ++i)
        if (data->animationIndices[i].flags)
            WRITE_RECORD(CAD_TAG_ANIMATION_INDEX, i,
                         CAD_X11_ANIMATION_INDEX_PAYLOAD_SIZE,
                         encode_animation_index,
                         &data->animationIndices[i]);
    for (i = 0; i < CAD_MAX_ANIMATION_POINTS; ++i)
        if (data->animationPoints[i].flags)
            WRITE_RECORD(CAD_TAG_ANIMATION_POINT, i,
                         CAD_X11_ANIMATION_POINT_PAYLOAD_SIZE,
                         encode_point, &data->animationPoints[i]);
#undef WRITE_RECORD

    *outputBytes = bytes;
    *outputSize = position;
    result.bytesConsumed = position;
    return result;
}

void CadCodec_FreeBuffer(void* buffer) {
    free(buffer);
}
