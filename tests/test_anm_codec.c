#include "cad_anm_codec.h"

#include <stdarg.h>
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

typedef struct TestBuffer {
    uint8_t* bytes;
    size_t size;
    size_t capacity;
} TestBuffer;

static int buffer_reserve(TestBuffer* buffer, size_t additional) {
    size_t needed = buffer->size + additional;
    size_t capacity = buffer->capacity ? buffer->capacity : 1024;
    uint8_t* resized;
    if (needed <= buffer->capacity) return 1;
    while (capacity < needed) capacity *= 2;
    resized = (uint8_t*)realloc(buffer->bytes, capacity);
    if (!resized) return 0;
    buffer->bytes = resized;
    buffer->capacity = capacity;
    return 1;
}

static int buffer_append(TestBuffer* buffer, const void* bytes, size_t size) {
    if (!buffer_reserve(buffer, size)) return 0;
    memcpy(buffer->bytes + buffer->size, bytes, size);
    buffer->size += size;
    return 1;
}

static int buffer_format(TestBuffer* buffer, const char* format, ...) {
    char text[512];
    va_list arguments;
    int length;
    va_start(arguments, format);
    length = vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    if (length < 0 || (size_t)length >= sizeof(text)) return 0;
    return buffer_append(buffer, text, (size_t)length);
}

static const char valid_two_sided[] =
    "3DAN\n"
    "3\n"
    "2\n"
    "0 0 0\n"
    "10 0 0\n"
    "0 10 0\n"
    "0 0 1\n"
    "11 0 1\n"
    "0 11 1\n"
    "3 0 1 2 7\n"
    "3 2 1 0 8\n";

static void test_decode_and_semantic_round_trip(void) {
    CadFileData* decoded = (CadFileData*)malloc(sizeof(*decoded));
    CadFileData* second = (CadFileData*)malloc(sizeof(*second));
    CadResult result;
    uint8_t* firstBytes = NULL;
    uint8_t* secondBytes = NULL;
    size_t firstSize = 0;
    size_t secondSize = 0;
    int16_t framePoint;
    CHECK(decoded != NULL && second != NULL);
    if (!decoded || !second) goto done;
    result = CadAnmCodec_Decode((const uint8_t*)valid_two_sided,
                                strlen(valid_two_sided), CAD_FORMAT_AUTO,
                                decoded);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(result.format == CAD_FORMAT_ANM_3DAN);
    CHECK(decoded->objectCount == 1);
    CHECK(decoded->polygonCount == 2);
    CHECK(decoded->pointCount == 6);
    CHECK(decoded->animationIndexCount == 2);
    CHECK(decoded->animationPointCount == 12);
    CHECK(decoded->polygons[0].both == 1);
    CHECK(decoded->polygons[1].both == 0);
    CHECK(decoded->polygons[0].side == 2);
    CHECK(decoded->polygons[1].side == 0);
    framePoint = decoded->animationIndices[0].frame[1];
    CHECK(framePoint >= 0);
    CHECK(decoded->animationPoints[framePoint].pointz == 1.0);

    result = CadAnmCodec_Encode(decoded, CAD_FORMAT_ANM_3DAN,
                                &firstBytes, &firstSize);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(firstBytes != NULL && firstSize > 1);
    CHECK(firstBytes[firstSize - 1] == 0x1a);
    {
        static const char expectedHeader[] = "3DAN\r\n3\r\n2\r\n";
        CHECK(firstSize >= sizeof(expectedHeader) - 1 &&
              memcmp(firstBytes, expectedHeader,
                     sizeof(expectedHeader) - 1) == 0);
    }
    result = CadAnmCodec_Decode(firstBytes, firstSize,
                                CAD_FORMAT_ANM_3DAN, second);
    CHECK(CadResult_IsSuccess(&result));
    result = CadAnmCodec_Encode(second, CAD_FORMAT_ANM_3DAN,
                                &secondBytes, &secondSize);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(firstSize == secondSize);
    CHECK(firstSize == secondSize &&
          memcmp(firstBytes, secondBytes, firstSize) == 0);

done:
    CadCodec_FreeBuffer(firstBytes);
    CadCodec_FreeBuffer(secondBytes);
    free(decoded);
    free(second);
}

static void test_legacy_header_and_format_selection(void) {
    static const char legacy[] =
        "3DGI\r\n2\r\n1\r\n-1 0 1\r\n2 3 4\r\n2 0 1 255\r\n\x1a";
    CadFileData* data = (CadFileData*)malloc(sizeof(*data));
    CadResult result;
    uint8_t* encoded = NULL;
    size_t encodedSize = 0;
    CHECK(data != NULL);
    if (!data) return;
    result = CadAnmCodec_Decode((const uint8_t*)legacy, sizeof(legacy) - 1,
                                CAD_FORMAT_AUTO, data);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(result.format == CAD_FORMAT_ANM_3DGI);
    result = CadAnmCodec_Decode((const uint8_t*)legacy, sizeof(legacy) - 1,
                                CAD_FORMAT_ANM_3DAN, data);
    CHECK(!CadResult_IsSuccess(&result));
    result = CadAnmCodec_Encode(data, CAD_FORMAT_ANM_3DGI,
                                &encoded, &encodedSize);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(encodedSize >= 4 && memcmp(encoded, "3DGI", 4) == 0);
    CadCodec_FreeBuffer(encoded);
    free(data);
}

static void test_recovered_side_bits(void) {
    static const char source[] =
        "3DAN\n7\n1\n"
        "0 0 0\n0 1 0\n0 0 -1\n1 0 0\n0 0 1\n1 0 0\n0 1 0\n"
        "3 0 1 2 1\n"
        "3 0 3 4 2\n"
        "3 0 6 5 3\n";
    CadFileData* data = (CadFileData*)malloc(sizeof(*data));
    CadResult result;
    CHECK(data != NULL);
    if (!data) return;
    result = CadAnmCodec_Decode((const uint8_t*)source, sizeof(source) - 1,
                                CAD_FORMAT_AUTO, data);
    CHECK(CadResult_IsSuccess(&result));
    /* normalX < 0 sets bit 2 and normalZ == 0 sets bit 1. */
    CHECK(data->polygons[0].side == 6);
    /* normalY < 0 sets bit 0 and normalZ == 0 sets bit 1. */
    CHECK(data->polygons[1].side == 3);
    /* A negative Z normal leaves all three recovered side bits clear. */
    CHECK(data->polygons[2].side == 0);
    free(data);
}

static void test_collinear_leading_vertices_reconstruct_side(void) {
    static const char source[] =
        "3DAN\n5\n1\n"
        "0 0 0\n1 0 0\n2 0 0\n2 -2 0\n0 -2 0\n"
        "5 0 1 2 3 4 9\n";
    CadFileData* data = (CadFileData*)malloc(sizeof(*data));
    CadResult result;
    CHECK(data != NULL);
    if (!data) return;
    result = CadAnmCodec_Decode((const uint8_t*)source, sizeof(source) - 1,
                                CAD_FORMAT_AUTO, data);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(data->polygonCount == 1);
    /* The complete clockwise XY winding has a negative Z normal.  Looking at
       the first three collinear vertices alone would incorrectly produce 2. */
    CHECK(data->polygons[0].side == 0);
    free(data);
}

static void test_transactional_strict_failures(void) {
    static const char truncated[] =
        "3DAN\n2\n2\n0 0 0\n1 0 0\n0 0 1\n";
    static const char badIndex[] =
        "3DAN\n2\n1\n0 0 0\n1 0 0\n2 0 2 1\n";
    static const char badColor[] =
        "3DAN\n2\n1\n0 0 0\n1 0 0\n2 0 1 256\n";
    static const char trailingAfterEof[] =
        "3DAN\n2\n1\n0 0 0\n1 0 0\n2 0 1 1\n\x1a" "X";
    static const char trailingToken[] =
        "3DAN\n2\n1\n0 0 0\n1 0 0\n2 0 1 1\nwat\n";
    static const char overflowingInteger[] =
        "3DAN\n2\n1\n2147483648 0 0\n1 0 0\n2 0 1 1\n";
    const char* inputs[] = {
        truncated, badIndex, badColor, trailingAfterEof, trailingToken,
        overflowingInteger
    };
    const size_t sizes[] = {
        sizeof(truncated) - 1, sizeof(badIndex) - 1,
        sizeof(badColor) - 1, sizeof(trailingAfterEof) - 1,
        sizeof(trailingToken) - 1, sizeof(overflowingInteger) - 1
    };
    CadFileData* output = (CadFileData*)malloc(sizeof(*output));
    CadFileData* before = (CadFileData*)malloc(sizeof(*before));
    uint8_t* encoded = (uint8_t*)(uintptr_t)1;
    size_t encodedSize = 99;
    size_t i;
    CHECK(output != NULL && before != NULL);
    if (!output || !before) goto done;
    memset(output, 0xa5, sizeof(*output));
    *before = *output;
    for (i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        CadResult result = CadAnmCodec_Decode((const uint8_t*)inputs[i],
                                              sizes[i], CAD_FORMAT_AUTO,
                                              output);
        CHECK(!CadResult_IsSuccess(&result));
        CHECK(memcmp(output, before, sizeof(*output)) == 0);
    }
    {
        CadResult result = CadAnmCodec_Validate(NULL);
        CHECK(!CadResult_IsSuccess(&result));
        result = CadAnmCodec_Encode(NULL, CAD_FORMAT_ANM_3DAN,
                                    &encoded, &encodedSize);
        CHECK(!CadResult_IsSuccess(&result));
        CHECK(encoded == NULL);
        CHECK(encodedSize == 0);
    }
done:
    free(output);
    free(before);
}

static void test_quantization_and_game_range_warnings(void) {
    static const char source[] =
        "3DAN\n3\n1\n0 0 0\n1 0 0\n0 1 0\n3 0 1 2 9\n";
    CadFileData* data = (CadFileData*)malloc(sizeof(*data));
    CadResult result;
    uint8_t* encoded = NULL;
    size_t encodedSize = 0;
    int16_t first;
    CHECK(data != NULL);
    if (!data) return;
    result = CadAnmCodec_Decode((const uint8_t*)source, sizeof(source) - 1,
                                CAD_FORMAT_AUTO, data);
    CHECK(CadResult_IsSuccess(&result));
    first = data->animationIndices[0].frame[0];
    data->animationPoints[first].pointx = 0.5;
    data->animationPoints[first].pointy = -0.5;
    data->animationPoints[first].pointz = 128.25;
    result = CadAnmCodec_Validate(data);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(result.warningCount >= 2);
    result = CadAnmCodec_Encode(data, CAD_FORMAT_AUTO,
                                &encoded, &encodedSize);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(result.format == CAD_FORMAT_ANM_3DAN);
    CHECK(encoded != NULL);
    if (encoded) {
        const char expected[] = "1 -1 128\r\n";
        size_t i;
        int found = 0;
        for (i = 0; i + sizeof(expected) - 1 <= encodedSize; ++i)
            if (memcmp(encoded + i, expected, sizeof(expected) - 1) == 0) {
                found = 1;
                break;
            }
        CHECK(found);
    }
    CadCodec_FreeBuffer(encoded);
    free(data);
}

static void test_track_deduplication_uses_every_frame(void) {
    static const char source[] =
        "3DAN\n2\n2\n0 0 0\n0 0 0\n1 0 0\n2 0 0\n2 0 1 4\n";
    static const char expectedHeader[] = "3DAN\r\n2\r\n2\r\n";
    CadFileData* data = (CadFileData*)malloc(sizeof(*data));
    CadResult result;
    uint8_t* encoded = NULL;
    size_t encodedSize = 0;
    CHECK(data != NULL);
    if (!data) return;
    result = CadAnmCodec_Decode((const uint8_t*)source, sizeof(source) - 1,
                                CAD_FORMAT_AUTO, data);
    CHECK(CadResult_IsSuccess(&result));
    result = CadAnmCodec_Encode(data, CAD_FORMAT_ANM_3DAN,
                                &encoded, &encodedSize);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(encodedSize >= sizeof(expectedHeader) - 1);
    CHECK(encodedSize >= sizeof(expectedHeader) - 1 &&
          memcmp(encoded, expectedHeader, sizeof(expectedHeader) - 1) == 0);
    CadCodec_FreeBuffer(encoded);
    free(data);
}

static void test_static_faces_repeat_across_animation(void) {
    static const char animated[] =
        "3DAN\n3\n2\n0 0 0\n1 0 0\n0 1 0\n"
        "0 0 1\n1 0 1\n0 1 1\n3 0 1 2 3\n";
    CadFileData* data = (CadFileData*)malloc(sizeof(*data));
    CadFileData* roundTrip = (CadFileData*)malloc(sizeof(*roundTrip));
    CadResult result;
    uint8_t* encoded = NULL;
    size_t encodedSize = 0;
    int16_t frame0;
    int16_t frame1;
    CHECK(data != NULL && roundTrip != NULL);
    if (!data || !roundTrip) goto done;
    result = CadAnmCodec_Decode((const uint8_t*)animated,
                                sizeof(animated) - 1,
                                CAD_FORMAT_AUTO, data);
    CHECK(CadResult_IsSuccess(&result));
    data->points[3].flags = 2;
    data->points[3].nextPoint = 4;
    data->points[3].pointx = 10.0;
    data->points[3].pointy = 20.0;
    data->points[3].pointz = 30.0;
    data->points[4].flags = 2;
    data->points[4].nextPoint = -1;
    data->points[4].pointx = -10.0;
    data->points[4].pointy = -20.0;
    data->points[4].pointz = -30.0;
    data->polygons[0].nextPolygon = 1;
    data->polygons[1].flags = 1;
    data->polygons[1].nextPolygon = -1;
    data->polygons[1].firstPoint = 3;
    data->polygons[1].animation = -1;
    data->polygons[1].both = -1;
    data->polygons[1].side = 2;
    data->polygons[1].color = 77;
    data->polygons[1].npoints = 2;
    data->pointCount = 5;
    data->polygonCount = 2;
    result = CadAnmCodec_Encode(data, CAD_FORMAT_ANM_3DAN,
                                &encoded, &encodedSize);
    CHECK(CadResult_IsSuccess(&result));
    result = CadAnmCodec_Decode(encoded, encodedSize, CAD_FORMAT_AUTO,
                                roundTrip);
    CHECK(CadResult_IsSuccess(&result));
    frame0 = roundTrip->animationIndices[1].frame[0];
    frame1 = roundTrip->animationIndices[1].frame[1];
    CHECK(frame0 >= 0 && frame1 >= 0);
    CHECK(roundTrip->animationPoints[frame0].pointx ==
          roundTrip->animationPoints[frame1].pointx);
    CHECK(roundTrip->animationPoints[frame0].pointy ==
          roundTrip->animationPoints[frame1].pointy);
done:
    CadCodec_FreeBuffer(encoded);
    free(data);
    free(roundTrip);
}

static TestBuffer make_capacity_fixture(int facePointCount) {
    TestBuffer buffer;
    int frame;
    int face;
    int point;
    memset(&buffer, 0, sizeof(buffer));
    buffer_format(&buffer, "3DAN\r\n3\r\n16\r\n");
    for (frame = 0; frame < 16; ++frame)
        for (point = 0; point < 3; ++point)
            buffer_format(&buffer, "%d %d %d\r\n", point, frame, 0);
    for (face = 0; face < CAD_MAX_ANIMATION_INDICES; ++face) {
        int count = face == 0 ? facePointCount : 2;
        buffer_format(&buffer, "%d", count);
        for (point = 0; point < count; ++point)
            buffer_format(&buffer, " %d", point % 3);
        buffer_format(&buffer, " %d\r\n", face & 255);
    }
    return buffer;
}

static void test_frame_and_face_limits(void) {
    TestBuffer frames;
    TestBuffer face;
    CadFileData* data = (CadFileData*)malloc(sizeof(*data));
    CadResult result;
    int frameIndex;
    int pointIndex;
    memset(&frames, 0, sizeof(frames));
    memset(&face, 0, sizeof(face));
    CHECK(data != NULL);
    if (!data) return;
    buffer_format(&frames, "3DAN\n2\n64\n");
    for (frameIndex = 0; frameIndex < 64; ++frameIndex) {
        buffer_format(&frames, "%d %d %d\n", -frameIndex, 0, frameIndex);
        buffer_format(&frames, "%d %d %d\n", 1, frameIndex, 0);
    }
    buffer_format(&frames, "2 0 1 5\n");
    result = CadAnmCodec_Decode(frames.bytes, frames.size,
                                CAD_FORMAT_AUTO, data);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(data->animationIndices[0].frame[63] >= 0);
    if (data->animationIndices[0].frame[63] >= 0)
        CHECK(data->animationPoints[
                  data->animationIndices[0].frame[63]].pointx == -63.0);

    buffer_format(&face, "3DGI\r\n16\r\n1\r\n");
    for (pointIndex = 0; pointIndex < 16; ++pointIndex)
        buffer_format(&face, "%d %d %d\r\n",
                      pointIndex - 8, pointIndex & 1, 0);
    buffer_format(&face, "16");
    for (pointIndex = 0; pointIndex < 16; ++pointIndex)
        buffer_format(&face, " %d", pointIndex);
    buffer_format(&face, " 255\r\n");
    {
        const uint8_t eof = 0x1a;
        buffer_append(&face, &eof, 1);
    }
    result = CadAnmCodec_Decode(face.bytes, face.size,
                                CAD_FORMAT_AUTO, data);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(data->polygons[0].npoints == CAD_MAX_FACE_POINTS);
    CHECK(data->polygons[0].color == 255);
    free(frames.bytes);
    free(face.bytes);
    free(data);
}

static void test_capacity_boundary(void) {
    CadFileData* data = (CadFileData*)malloc(sizeof(*data));
    TestBuffer exact = make_capacity_fixture(2);
    TestBuffer excess = make_capacity_fixture(3);
    CadResult result;
    CHECK(data != NULL && exact.bytes != NULL && excess.bytes != NULL);
    if (!data || !exact.bytes || !excess.bytes) goto done;
    result = CadAnmCodec_Decode(exact.bytes, exact.size,
                                CAD_FORMAT_AUTO, data);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(data->animationPointCount == CAD_MAX_ANIMATION_POINTS);
    CHECK(data->animationIndexCount == CAD_MAX_ANIMATION_INDICES);
    result = CadAnmCodec_Decode(excess.bytes, excess.size,
                                CAD_FORMAT_AUTO, data);
    CHECK(!CadResult_IsSuccess(&result));
done:
    free(exact.bytes);
    free(excess.bytes);
    free(data);
}

int main(void) {
    test_decode_and_semantic_round_trip();
    test_legacy_header_and_format_selection();
    test_recovered_side_bits();
    test_collinear_leading_vertices_reconstruct_side();
    test_transactional_strict_failures();
    test_quantization_and_game_range_warnings();
    test_track_deduplication_uses_every_frame();
    test_static_faces_repeat_across_animation();
    test_frame_and_face_limits();
    test_capacity_boundary();
    if (failures) {
        fprintf(stderr, "%d ANM codec test(s) failed\n", failures);
        return 1;
    }
    puts("ANM codec tests passed");
    return 0;
}
