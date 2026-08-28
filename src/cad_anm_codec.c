#include "cad_anm_codec.h"
#include "cad_geometry.h"

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    CAD_ANM_GAME_COORD_MIN = -127,
    CAD_ANM_GAME_COORD_MAX = 127
};

typedef struct AnmParser {
    const uint8_t* bytes;
    size_t size;
    size_t contentSize;
    size_t position;
} AnmParser;

typedef struct AnmFaceRecord {
    uint8_t pointCount;
    uint8_t color;
    int32_t point[CAD_MAX_FACE_POINTS];
    size_t byteOffset;
} AnmFaceRecord;

typedef struct AnmTrack {
    int32_t coordinate[CAD_ANIMATION_FRAMES][3];
} AnmTrack;

typedef struct AnmExportFace {
    uint8_t pointCount;
    uint8_t color;
    uint16_t track[CAD_MAX_FACE_POINTS];
} AnmExportFace;

typedef struct AnmExportPlan {
    int frameCount;
    int faceCount;
    int trackCount;
    AnmTrack* tracks;
    AnmExportFace* faces;
} AnmExportPlan;

typedef struct AnmBufferBuilder {
    uint8_t* bytes;
    size_t size;
    size_t capacity;
} AnmBufferBuilder;

static CadResult anm_result(CadFormat format) {
    CadResult result;
    memset(&result, 0, sizeof(result));
    result.status = CAD_STATUS_OK;
    result.format = format;
    return result;
}

static void anm_add_diagnostic(CadResult* result,
                               CadDiagnosticSeverity severity,
                               CadStatus code, size_t offset,
                               int recordTag, int recordIndex,
                               const char* message, ...) {
    CadDiagnostic* diagnostic = NULL;
    va_list arguments;
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
    diagnostic->recordTag = recordTag;
    diagnostic->recordIndex = recordIndex;
    va_start(arguments, message);
    vsnprintf(diagnostic->message, sizeof(diagnostic->message),
              message, arguments);
    va_end(arguments);
}

static CadResult anm_error(CadFormat format, CadStatus status,
                           size_t offset, int recordIndex,
                           const char* message) {
    CadResult result = anm_result(format);
    anm_add_diagnostic(&result, CAD_DIAGNOSTIC_ERROR, status, offset,
                       -1, recordIndex, "%s", message);
    return result;
}

static int anm_is_space(uint8_t byte) {
    return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n' ||
           byte == '\f' || byte == '\v';
}

static void parser_skip_space(AnmParser* parser) {
    while (parser->position < parser->contentSize &&
           anm_is_space(parser->bytes[parser->position]))
        parser->position++;
}

static int parser_has_token(AnmParser* parser) {
    parser_skip_space(parser);
    return parser->position < parser->contentSize;
}

/* Return 1 for a complete integer, 0 for EOF, and -1 for malformed/overflow. */
static int parser_integer(AnmParser* parser, int32_t* output,
                          size_t* tokenOffset) {
    uint64_t magnitude = 0;
    uint64_t limit;
    int negative = 0;
    int digitCount = 0;
    parser_skip_space(parser);
    if (tokenOffset) *tokenOffset = parser->position;
    if (parser->position >= parser->contentSize) return 0;
    if (parser->bytes[parser->position] == '+' ||
        parser->bytes[parser->position] == '-') {
        negative = parser->bytes[parser->position] == '-';
        parser->position++;
    }
    limit = negative ? UINT64_C(2147483648) : UINT64_C(2147483647);
    while (parser->position < parser->contentSize) {
        uint8_t byte = parser->bytes[parser->position];
        unsigned digit;
        if (byte < '0' || byte > '9') break;
        digit = (unsigned)(byte - '0');
        if (magnitude > (limit - digit) / 10U) return -1;
        magnitude = magnitude * 10U + digit;
        parser->position++;
        digitCount++;
    }
    if (!digitCount) return -1;
    if (parser->position < parser->contentSize &&
        !anm_is_space(parser->bytes[parser->position]))
        return -1;
    if (negative) {
        if (magnitude == UINT64_C(2147483648)) *output = INT32_MIN;
        else *output = -(int32_t)magnitude;
    } else {
        *output = (int32_t)magnitude;
    }
    return 1;
}

static int parser_required_integer(AnmParser* parser, int32_t* output,
                                   CadResult* result, int recordIndex,
                                   const char* description) {
    size_t offset = parser->position;
    int status = parser_integer(parser, output, &offset);
    if (status == 1) return 1;
    if (status == 0)
        anm_add_diagnostic(result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_TRUNCATED_RECORD, offset, -1,
                           recordIndex, "Missing %s", description);
    else
        anm_add_diagnostic(result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_INVALID_NUMBER, offset, -1,
                           recordIndex, "Invalid or overflowing %s",
                           description);
    return 0;
}

static int parser_prepare(AnmParser* parser, const uint8_t* bytes, size_t size,
                          CadResult* result) {
    size_t i;
    parser->bytes = bytes;
    parser->size = size;
    parser->contentSize = size;
    parser->position = 0;
    for (i = 0; i < size; ++i) {
        if (bytes[i] != 0x1a) continue;
        parser->contentSize = i;
        for (++i; i < size; ++i) {
            if (!anm_is_space(bytes[i])) {
                anm_add_diagnostic(result, CAD_DIAGNOSTIC_ERROR,
                                   CAD_STATUS_UNKNOWN_RECORD, i, -1, -1,
                                   "Unexpected data follows the DOS EOF marker");
                return 0;
            }
        }
        break;
    }
    return 1;
}

static int parser_header(AnmParser* parser, CadFormat requestedFormat,
                         CadFormat* detectedFormat, CadResult* result) {
    size_t start;
    size_t length;
    parser_skip_space(parser);
    start = parser->position;
    while (parser->position < parser->contentSize &&
           !anm_is_space(parser->bytes[parser->position]))
        parser->position++;
    length = parser->position - start;
    if (length == 4 && memcmp(parser->bytes + start, "3DAN", 4) == 0)
        *detectedFormat = CAD_FORMAT_ANM_3DAN;
    else if (length == 4 && memcmp(parser->bytes + start, "3DGI", 4) == 0)
        *detectedFormat = CAD_FORMAT_ANM_3DGI;
    else {
        anm_add_diagnostic(result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_UNRECOGNIZED_FORMAT, start, -1, -1,
                           "Expected a 3DAN or 3DGI header");
        return 0;
    }
    result->format = *detectedFormat;
    if (requestedFormat != CAD_FORMAT_AUTO &&
        requestedFormat != *detectedFormat) {
        anm_add_diagnostic(result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_UNRECOGNIZED_FORMAT, start, -1, -1,
                           "ANM header does not match the requested format");
        return 0;
    }
    return 1;
}

static int face_records_match(const AnmFaceRecord* first,
                              const AnmFaceRecord* second) {
    uint8_t used[CAD_MAX_FACE_POINTS];
    int i;
    int j;
    if (first->pointCount != second->pointCount) return 0;
    memset(used, 0, sizeof(used));
    for (i = 0; i < first->pointCount; ++i) {
        int found = 0;
        for (j = 0; j < second->pointCount; ++j) {
            if (!used[j] && first->point[i] == second->point[j]) {
                used[j] = 1;
                found = 1;
                break;
            }
        }
        if (!found) return 0;
    }
    return 1;
}

static uint8_t reconstructed_side(const AnmFaceRecord* face,
                                  const int32_t* coordinates) {
    double faceCoordinates[CAD_MAX_FACE_POINTS][3];
    double normal[3] = {0.0, 0.0, 0.0};
    uint8_t side = 0;
    int ordinal;
    for (ordinal = 0; ordinal < face->pointCount; ++ordinal) {
        const int32_t* point = coordinates + (size_t)face->point[ordinal] * 3U;
        faceCoordinates[ordinal][0] = (double)point[0];
        faceCoordinates[ordinal][1] = (double)point[1];
        faceCoordinates[ordinal][2] = (double)point[2];
    }
    (void)CadGeometry_ComputePolygonNormal(faceCoordinates,
                                           face->pointCount, normal);
    if (normal[1] < 0.0) side |= 1U;
    if (normal[2] >= 0.0) side |= 2U;
    if (normal[0] < 0.0) side |= 4U;
    return side;
}

static int build_native_document(CadFileData* candidate,
                                 const int32_t* coordinates,
                                 int sourcePointCount, int frameCount,
                                 const AnmFaceRecord* faces, int faceCount,
                                 CadResult* result) {
    int pointCursor = 0;
    int animationPointCursor = 0;
    int polygonIndex;
    CadFile_Init(candidate);
    candidate->objects[0].flags = 1;
    candidate->objects[0].parentObject = -1;
    candidate->objects[0].nextBrother = -1;
    candidate->objects[0].childObject = -1;
    candidate->objects[0].firstPolygon = faceCount ? 0 : -1;
    candidate->objectCount = 1;

    for (polygonIndex = 0; polygonIndex < faceCount; ++polygonIndex) {
        const AnmFaceRecord* sourceFace = &faces[polygonIndex];
        CadPolygon* polygon = &candidate->polygons[polygonIndex];
        CadAnimationIndex* animation =
            &candidate->animationIndices[polygonIndex];
        int ordinal;
        int frame;
        polygon->flags = 1;
        polygon->nextPolygon = polygonIndex + 1 < faceCount
            ? (int16_t)(polygonIndex + 1) : -1;
        polygon->firstPoint = (int16_t)pointCursor;
        polygon->animation = (int16_t)polygonIndex;
        polygon->both = -1;
        polygon->side = reconstructed_side(sourceFace, coordinates);
        polygon->color = sourceFace->color;
        polygon->npoints = sourceFace->pointCount;

        for (ordinal = 0; ordinal < sourceFace->pointCount; ++ordinal) {
            int sourceIndex = sourceFace->point[ordinal];
            const int32_t* source = coordinates + (size_t)sourceIndex * 3U;
            CadPoint* point = &candidate->points[pointCursor];
            point->flags = 2;
            point->nextPoint = ordinal + 1 < sourceFace->pointCount
                ? (int16_t)(pointCursor + 1) : -1;
            point->pointx = (double)source[0];
            point->pointy = (double)source[1];
            point->pointz = (double)source[2];
            pointCursor++;
        }

        animation->flags = 1;
        for (frame = 0; frame < frameCount; ++frame) {
            animation->frame[frame] = (int16_t)animationPointCursor;
            for (ordinal = 0; ordinal < sourceFace->pointCount; ++ordinal) {
                int sourceIndex = sourceFace->point[ordinal];
                const int32_t* source = coordinates +
                    (((size_t)frame * (size_t)sourcePointCount +
                      (size_t)sourceIndex) * 3U);
                CadAnimationPoint* point =
                    &candidate->animationPoints[animationPointCursor];
                point->flags = 1;
                point->nextPoint = ordinal + 1 < sourceFace->pointCount
                    ? (int16_t)(animationPointCursor + 1) : -1;
                point->pointx = (double)source[0];
                point->pointy = (double)source[1];
                point->pointz = (double)source[2];
                animationPointCursor++;
            }
        }
    }

    for (polygonIndex = 0; polygonIndex < faceCount; ++polygonIndex) {
        int other;
        if (candidate->polygons[polygonIndex].both != -1) continue;
        for (other = polygonIndex + 1; other < faceCount; ++other) {
            if (candidate->polygons[other].both == -1 &&
                face_records_match(&faces[polygonIndex], &faces[other])) {
                candidate->polygons[polygonIndex].both = (int16_t)other;
                candidate->polygons[other].both = (int16_t)polygonIndex;
                break;
            }
        }
    }

    candidate->polygonCount = faceCount;
    candidate->pointCount = pointCursor;
    candidate->animationIndexCount = faceCount;
    candidate->animationPointCount = animationPointCursor;
    {
        CadResult validation = CadCodec_Validate(candidate);
        if (!CadResult_IsSuccess(&validation)) {
            validation.format = result->format;
            *result = validation;
            return 0;
        }
    }
    return 1;
}

CadResult CadAnmCodec_Decode(const uint8_t* bytes, size_t size,
                             CadFormat requestedFormat,
                             CadFileData* output) {
    CadResult result = anm_result(requestedFormat);
    AnmParser parser;
    CadFormat detectedFormat = CAD_FORMAT_AUTO;
    CadFileData* candidate = NULL;
    int32_t* coordinates = NULL;
    AnmFaceRecord* faces = NULL;
    int32_t sourcePointCountValue;
    int32_t frameCountValue;
    int sourcePointCount;
    int frameCount;
    int faceCount = 0;
    int referenceCount = 0;
    int frame;
    int point;

    if (!bytes || !output)
        return anm_error(requestedFormat, CAD_STATUS_INVALID_ARGUMENT,
                         0, -1, "ANM decode requires a buffer and output");
    if (!size)
        return anm_error(requestedFormat, CAD_STATUS_EMPTY_INPUT,
                         0, -1, "ANM input is empty");
    if (requestedFormat != CAD_FORMAT_AUTO &&
        requestedFormat != CAD_FORMAT_ANM_3DAN &&
        requestedFormat != CAD_FORMAT_ANM_3DGI)
        return anm_error(requestedFormat, CAD_STATUS_UNSUPPORTED_FORMAT,
                         0, -1, "Requested format is not an ANM text variant");
    if (!parser_prepare(&parser, bytes, size, &result)) return result;
    if (!parser_header(&parser, requestedFormat, &detectedFormat, &result))
        return result;
    if (!parser_required_integer(&parser, &sourcePointCountValue, &result,
                                 -1, "point count") ||
        !parser_required_integer(&parser, &frameCountValue, &result,
                                 -1, "frame count"))
        return result;
    if (sourcePointCountValue < 1 ||
        sourcePointCountValue > CAD_MAX_POINTS) {
        anm_add_diagnostic(&result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_INDEX_OUT_OF_RANGE, parser.position,
                           -1, -1, "ANM point count must be between 1 and %d",
                           CAD_MAX_POINTS);
        return result;
    }
    if (frameCountValue < 1 || frameCountValue > CAD_ANIMATION_FRAMES) {
        anm_add_diagnostic(&result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_INDEX_OUT_OF_RANGE, parser.position,
                           -1, -1, "ANM frame count must be between 1 and %d",
                           CAD_ANIMATION_FRAMES);
        return result;
    }
    sourcePointCount = (int)sourcePointCountValue;
    frameCount = (int)frameCountValue;
    coordinates = (int32_t*)malloc((size_t)sourcePointCount *
                                   (size_t)frameCount * 3U *
                                   sizeof(*coordinates));
    faces = (AnmFaceRecord*)calloc(CAD_MAX_ANIMATION_INDICES,
                                   sizeof(*faces));
    candidate = (CadFileData*)malloc(sizeof(*candidate));
    if (!coordinates || !faces || !candidate) {
        free(coordinates);
        free(faces);
        free(candidate);
        return anm_error(detectedFormat, CAD_STATUS_OUT_OF_MEMORY,
                         parser.position, -1,
                         "Could not allocate an ANM decode candidate");
    }
    for (frame = 0; frame < frameCount; ++frame) {
        for (point = 0; point < sourcePointCount; ++point) {
            int axis;
            for (axis = 0; axis < 3; ++axis) {
                int32_t value;
                if (!parser_required_integer(&parser, &value, &result,
                                             point, "point coordinate"))
                    goto decode_failed;
                coordinates[(((size_t)frame * (size_t)sourcePointCount +
                              (size_t)point) * 3U) + (size_t)axis] = value;
            }
        }
    }
    while (parser_has_token(&parser)) {
        AnmFaceRecord* face;
        int32_t facePointCount;
        int32_t color;
        int ordinal;
        size_t faceOffset = parser.position;
        if (faceCount >= CAD_MAX_ANIMATION_INDICES) {
            anm_add_diagnostic(&result, CAD_DIAGNOSTIC_ERROR,
                               CAD_STATUS_INDEX_OUT_OF_RANGE, faceOffset,
                               -1, faceCount,
                               "ANM has more than %d animated faces",
                               CAD_MAX_ANIMATION_INDICES);
            goto decode_failed;
        }
        if (!parser_required_integer(&parser, &facePointCount, &result,
                                     faceCount, "face point count"))
            goto decode_failed;
        if (facePointCount < CAD_MIN_FACE_POINTS ||
            facePointCount > CAD_MAX_FACE_POINTS) {
            anm_add_diagnostic(&result, CAD_DIAGNOSTIC_ERROR,
                               CAD_STATUS_INVALID_TOPOLOGY, faceOffset,
                               -1, faceCount,
                               "ANM face %d has %d points; supported range is %d..%d",
                               faceCount, (int)facePointCount,
                               CAD_MIN_FACE_POINTS, CAD_MAX_FACE_POINTS);
            goto decode_failed;
        }
        if (referenceCount + facePointCount > CAD_MAX_POINTS ||
            ((size_t)referenceCount + (size_t)facePointCount) *
                (size_t)frameCount > CAD_MAX_ANIMATION_POINTS) {
            anm_add_diagnostic(&result, CAD_DIAGNOSTIC_ERROR,
                               CAD_STATUS_INDEX_OUT_OF_RANGE, faceOffset,
                               -1, faceCount,
                               "ANM face expansion exceeds native point capacity");
            goto decode_failed;
        }
        face = &faces[faceCount];
        face->pointCount = (uint8_t)facePointCount;
        face->byteOffset = faceOffset;
        for (ordinal = 0; ordinal < facePointCount; ++ordinal) {
            int32_t index;
            if (!parser_required_integer(&parser, &index, &result,
                                         faceCount, "face point index"))
                goto decode_failed;
            if (index < 0 || index >= sourcePointCount) {
                anm_add_diagnostic(&result, CAD_DIAGNOSTIC_ERROR,
                                   CAD_STATUS_INDEX_OUT_OF_RANGE,
                                   parser.position, -1, faceCount,
                                   "ANM face %d references point %d outside 0..%d",
                                   faceCount, (int)index,
                                   sourcePointCount - 1);
                goto decode_failed;
            }
            face->point[ordinal] = index;
        }
        if (!parser_required_integer(&parser, &color, &result,
                                     faceCount, "face color"))
            goto decode_failed;
        if (color < 0 || color > 255) {
            anm_add_diagnostic(&result, CAD_DIAGNOSTIC_ERROR,
                               CAD_STATUS_INDEX_OUT_OF_RANGE,
                               parser.position, -1, faceCount,
                               "ANM face %d color must be between 0 and 255",
                               faceCount);
            goto decode_failed;
        }
        face->color = (uint8_t)color;
        referenceCount += (int)facePointCount;
        faceCount++;
    }
    if (!faceCount) {
        anm_add_diagnostic(&result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_INVALID_TOPOLOGY, parser.position,
                           -1, -1, "ANM contains no faces");
        goto decode_failed;
    }
    if (!build_native_document(candidate, coordinates, sourcePointCount,
                               frameCount, faces, faceCount, &result))
        goto decode_failed;
    *output = *candidate;
    result.bytesConsumed = size;
    free(coordinates);
    free(faces);
    free(candidate);
    return result;

decode_failed:
    result.bytesConsumed = parser.position;
    free(coordinates);
    free(faces);
    free(candidate);
    return result;
}

static int round_coordinate(double value, int32_t* rounded,
                            int* changed) {
    double integral;
    double fraction;
    if (!isfinite(value)) return 0;
    fraction = modf(value, &integral);
    if (value >= 0.0) {
        if (fraction >= 0.5) integral += 1.0;
    } else if (fraction <= -0.5) {
        integral -= 1.0;
    }
    if (integral < (double)INT32_MIN || integral > (double)INT32_MAX)
        return 0;
    *rounded = (int32_t)integral;
    *changed = value != integral;
    return 1;
}

static int count_animation_frames(const CadFileData* data,
                                  const CadPolygon* polygon) {
    int count = 0;
    const CadAnimationIndex* animation;
    if (polygon->animation < 0) return 0;
    animation = &data->animationIndices[polygon->animation];
    while (count < CAD_ANIMATION_FRAMES && animation->frame[count] != -1)
        count++;
    return count;
}

static const CadAnimationPoint* animation_point_at(
    const CadFileData* data, const CadPolygon* polygon,
    int frame, int ordinal) {
    int16_t point =
        data->animationIndices[polygon->animation].frame[frame];
    int step;
    for (step = 0; step < ordinal; ++step)
        point = data->animationPoints[point].nextPoint;
    return &data->animationPoints[point];
}

static int tracks_equal(const AnmTrack* first, const AnmTrack* second,
                        int frameCount) {
    return memcmp(first->coordinate, second->coordinate,
                  (size_t)frameCount * 3U * sizeof(int32_t)) == 0;
}

static void export_plan_clear(AnmExportPlan* plan) {
    if (!plan) return;
    free(plan->tracks);
    free(plan->faces);
    memset(plan, 0, sizeof(*plan));
}

static CadResult prepare_export(const CadFileData* data,
                                AnmExportPlan* plan) {
    CadResult result;
    int faceCount = 0;
    int referenceCount = 0;
    int frameCount = 0;
    int polygonIndex;
    size_t quantizedCount = 0;
    size_t gameRangeCount = 0;
    memset(plan, 0, sizeof(*plan));
    if (!data)
        return anm_error(CAD_FORMAT_ANM_3DAN,
                         CAD_STATUS_INVALID_ARGUMENT, 0, -1,
                         "ANM validation requires CAD data");
    result = CadCodec_Validate(data);
    result.format = CAD_FORMAT_ANM_3DAN;
    if (!CadResult_IsSuccess(&result)) return result;
    for (polygonIndex = 0; polygonIndex < CAD_MAX_POLYGONS; ++polygonIndex) {
        const CadPolygon* polygon = &data->polygons[polygonIndex];
        int polygonFrames;
        if (!polygon->flags) continue;
        faceCount++;
        referenceCount += polygon->npoints;
        if (faceCount > CAD_MAX_ANIMATION_INDICES ||
            referenceCount > CAD_MAX_POINTS) {
            anm_add_diagnostic(&result, CAD_DIAGNOSTIC_ERROR,
                               CAD_STATUS_INDEX_OUT_OF_RANGE, 0,
                               CAD_TAG_POLYGON, polygonIndex,
                               "ANM expansion exceeds recovered face or point capacity");
            return result;
        }
        polygonFrames = count_animation_frames(data, polygon);
        if (polygonFrames) frameCount = polygonFrames;
    }
    if (!faceCount) {
        anm_add_diagnostic(&result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_INVALID_TOPOLOGY, 0, -1, -1,
                           "ANM export requires at least one face");
        return result;
    }
    if (!frameCount) frameCount = 1;
    if ((size_t)referenceCount * (size_t)frameCount >
        CAD_MAX_ANIMATION_POINTS) {
        anm_add_diagnostic(&result, CAD_DIAGNOSTIC_ERROR,
                           CAD_STATUS_INDEX_OUT_OF_RANGE, 0, -1, -1,
                           "ANM expansion needs more than %d animation points",
                           CAD_MAX_ANIMATION_POINTS);
        return result;
    }
    plan->tracks = (AnmTrack*)calloc((size_t)referenceCount,
                                     sizeof(*plan->tracks));
    plan->faces = (AnmExportFace*)calloc((size_t)faceCount,
                                         sizeof(*plan->faces));
    if (!plan->tracks || !plan->faces) {
        export_plan_clear(plan);
        return anm_error(CAD_FORMAT_ANM_3DAN,
                         CAD_STATUS_OUT_OF_MEMORY, 0, -1,
                         "Could not allocate an ANM export plan");
    }
    plan->frameCount = frameCount;
    plan->faceCount = faceCount;
    plan->trackCount = 0;
    faceCount = 0;
    for (polygonIndex = 0; polygonIndex < CAD_MAX_POLYGONS; ++polygonIndex) {
        const CadPolygon* polygon = &data->polygons[polygonIndex];
        AnmExportFace* face;
        int16_t staticPoint;
        int ordinal;
        if (!polygon->flags) continue;
        face = &plan->faces[faceCount++];
        face->pointCount = polygon->npoints;
        face->color = polygon->color;
        staticPoint = polygon->firstPoint;
        for (ordinal = 0; ordinal < polygon->npoints; ++ordinal) {
            AnmTrack candidateTrack;
            int trackIndex;
            int frame;
            memset(&candidateTrack, 0, sizeof(candidateTrack));
            for (frame = 0; frame < frameCount; ++frame) {
                const CadPoint* point;
                double values[3];
                int axis;
                if (polygon->animation != -1)
                    point = animation_point_at(data, polygon, frame, ordinal);
                else
                    point = &data->points[staticPoint];
                values[0] = point->pointx;
                values[1] = point->pointy;
                values[2] = point->pointz;
                for (axis = 0; axis < 3; ++axis) {
                    int changed;
                    int32_t rounded;
                    if (!round_coordinate(values[axis], &rounded, &changed)) {
                        anm_add_diagnostic(&result, CAD_DIAGNOSTIC_ERROR,
                                           CAD_STATUS_INVALID_NUMBER, 0,
                                           CAD_TAG_POLYGON, polygonIndex,
                                           "Polygon %d has a coordinate outside signed 32-bit ANM range",
                                           polygonIndex);
                        export_plan_clear(plan);
                        return result;
                    }
                    if (changed) quantizedCount++;
                    if (rounded < CAD_ANM_GAME_COORD_MIN ||
                        rounded > CAD_ANM_GAME_COORD_MAX)
                        gameRangeCount++;
                    candidateTrack.coordinate[frame][axis] = rounded;
                }
            }
            for (trackIndex = 0; trackIndex < plan->trackCount; ++trackIndex)
                if (tracks_equal(&candidateTrack, &plan->tracks[trackIndex],
                                 frameCount))
                    break;
            if (trackIndex == plan->trackCount)
                plan->tracks[plan->trackCount++] = candidateTrack;
            face->track[ordinal] = (uint16_t)trackIndex;
            staticPoint = data->points[staticPoint].nextPoint;
        }
    }
    if (quantizedCount)
        anm_add_diagnostic(&result, CAD_DIAGNOSTIC_WARNING,
                           CAD_STATUS_INVALID_NUMBER, 0, -1, -1,
                           "%zu coordinate values will be rounded using the recovered half-away-from-zero rule",
                           quantizedCount);
    if (gameRangeCount)
        anm_add_diagnostic(&result, CAD_DIAGNOSTIC_WARNING,
                           CAD_STATUS_INDEX_OUT_OF_RANGE, 0, -1, -1,
                           "%zu coordinate values are outside the recovered game range %d..%d",
                           gameRangeCount, CAD_ANM_GAME_COORD_MIN,
                           CAD_ANM_GAME_COORD_MAX);
    return result;
}

CadResult CadAnmCodec_Validate(const CadFileData* data) {
    AnmExportPlan plan;
    CadResult result = prepare_export(data, &plan);
    export_plan_clear(&plan);
    return result;
}

static int builder_reserve(AnmBufferBuilder* builder, size_t additional) {
    size_t needed;
    size_t capacity;
    uint8_t* resized;
    if (additional > SIZE_MAX - builder->size) return 0;
    needed = builder->size + additional;
    if (needed <= builder->capacity) return 1;
    capacity = builder->capacity ? builder->capacity : 1024U;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2U) {
            capacity = needed;
            break;
        }
        capacity *= 2U;
    }
    resized = (uint8_t*)realloc(builder->bytes, capacity);
    if (!resized) return 0;
    builder->bytes = resized;
    builder->capacity = capacity;
    return 1;
}

static int builder_append(AnmBufferBuilder* builder,
                          const void* bytes, size_t size) {
    if (!builder_reserve(builder, size)) return 0;
    memcpy(builder->bytes + builder->size, bytes, size);
    builder->size += size;
    return 1;
}

static int builder_format(AnmBufferBuilder* builder, const char* format, ...) {
    char text[512];
    va_list arguments;
    int length;
    va_start(arguments, format);
    length = vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    if (length < 0 || (size_t)length >= sizeof(text)) return 0;
    return builder_append(builder, text, (size_t)length);
}

CadResult CadAnmCodec_Encode(const CadFileData* data, CadFormat format,
                             uint8_t** outputBytes, size_t* outputSize) {
    AnmExportPlan plan;
    AnmBufferBuilder builder;
    CadResult result;
    const char* header;
    int frame;
    int track;
    int face;
    if (!outputBytes || !outputSize)
        return anm_error(format, CAD_STATUS_INVALID_ARGUMENT, 0, -1,
                         "ANM encode requires output buffer pointers");
    *outputBytes = NULL;
    *outputSize = 0;
    if (format == CAD_FORMAT_AUTO) format = CAD_FORMAT_ANM_3DAN;
    if (format != CAD_FORMAT_ANM_3DAN && format != CAD_FORMAT_ANM_3DGI)
        return anm_error(format, CAD_STATUS_UNSUPPORTED_FORMAT, 0, -1,
                         "ANM encode supports only 3DAN and 3DGI");
    result = prepare_export(data, &plan);
    result.format = format;
    if (!CadResult_IsSuccess(&result)) return result;
    memset(&builder, 0, sizeof(builder));
    header = format == CAD_FORMAT_ANM_3DGI ? "3DGI" : "3DAN";
    if (!builder_format(&builder, "%s\r\n%d\r\n%d\r\n",
                        header, plan.trackCount, plan.frameCount))
        goto encode_out_of_memory;
    for (frame = 0; frame < plan.frameCount; ++frame) {
        for (track = 0; track < plan.trackCount; ++track) {
            const int32_t* coordinate = plan.tracks[track].coordinate[frame];
            if (!builder_format(&builder, "%d %d %d\r\n",
                                (int)coordinate[0], (int)coordinate[1],
                                (int)coordinate[2]))
                goto encode_out_of_memory;
        }
    }
    for (face = 0; face < plan.faceCount; ++face) {
        int ordinal;
        if (!builder_format(&builder, "%u",
                            (unsigned)plan.faces[face].pointCount))
            goto encode_out_of_memory;
        for (ordinal = 0; ordinal < plan.faces[face].pointCount; ++ordinal)
            if (!builder_format(&builder, " %u",
                                (unsigned)plan.faces[face].track[ordinal]))
                goto encode_out_of_memory;
        if (!builder_format(&builder, " %u\r\n",
                            (unsigned)plan.faces[face].color))
            goto encode_out_of_memory;
    }
    {
        const uint8_t dosEof = 0x1a;
        if (!builder_append(&builder, &dosEof, 1U))
            goto encode_out_of_memory;
    }
    *outputBytes = builder.bytes;
    *outputSize = builder.size;
    result.bytesConsumed = builder.size;
    export_plan_clear(&plan);
    return result;

encode_out_of_memory:
    free(builder.bytes);
    export_plan_clear(&plan);
    anm_add_diagnostic(&result, CAD_DIAGNOSTIC_ERROR,
                       CAD_STATUS_OUT_OF_MEMORY, 0, -1, -1,
                       "Could not allocate the encoded ANM buffer");
    return result;
}
