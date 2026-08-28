#include "cad_animation.h"
#include "cad_core.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                             \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

static int16_t append_triangle(CadCore* core, double offset) {
    int16_t a = CadCore_AddPoint(core, offset, 0.0, 0.0);
    int16_t b = CadCore_AddPoint(core, offset + 1.0, 0.0, 0.0);
    int16_t c = CadCore_AddPoint(core, offset, 1.0, 0.0);
    CHECK(a >= 0 && b >= 0 && c >= 0);
    core->data.points[a].nextPoint = b;
    core->data.points[b].nextPoint = c;
    return CadCore_AddPolygon(core, a, 7, 3);
}

static int16_t append_polygon(CadCore* core, int pointCount, double offset) {
    int16_t first = -1;
    int16_t previous = -1;
    int ordinal;
    for (ordinal = 0; ordinal < pointCount; ++ordinal) {
        int16_t point = CadCore_AddPoint(core, offset + ordinal,
                                        (double)(ordinal & 1), 0.0);
        if (point < 0) return -1;
        if (previous >= 0) core->data.points[previous].nextPoint = point;
        else first = point;
        previous = point;
    }
    return CadCore_AddPolygon(core, first, 3, (uint8_t)pointCount);
}

static CadPosition pose_point(const CadFileData* data, double frame,
                              int interpolation, int16_t point) {
    CadPose pose;
    CadPoseSample sample;
    CadPosition output = { 999.0, 999.0, 999.0 };
    CadResult result;
    CadAnimationInfo info;
    result = CadAnimation_Inspect(data, &info);
    CHECK(CadResult_IsSuccess(&result));
    sample = CadPoseSample_FromFrame(frame,
                                     info.frameCount ? info.frameCount : 1,
                                     0, interpolation);
    result = CadPose_Evaluate(data, sample, &pose);
    CHECK(CadResult_IsSuccess(&result));
    if (result.status == CAD_STATUS_OK) output = pose.points[point];
    return output;
}

static CadAffineTransform translation(double x, double y, double z) {
    CadAffineTransform transform;
    memset(&transform, 0, sizeof(transform));
    transform.matrix[0][0] = 1.0;
    transform.matrix[1][1] = 1.0;
    transform.matrix[2][2] = 1.0;
    transform.translation.x = x;
    transform.translation.y = y;
    transform.translation.z = z;
    return transform;
}

static void test_lifecycle_and_pose(void) {
    CadCore core;
    CadAnimationInfo info;
    CadResult result;
    int16_t first;
    int16_t second;
    int16_t selectedFace[1];
    int16_t selectedPoint[1];
    CadPosition position;
    CadPosition displayed;
    CadPose pose;
    CadFileData* baked;
    CadAffineTransform transform;

    CadCore_Init(&core);
    baked = (CadFileData*)malloc(sizeof(*baked));
    CHECK(baked != NULL);
    first = append_triangle(&core, 0.0);
    second = append_triangle(&core, 20.0);
    CHECK(first == 0 && second == 1);
    selectedFace[0] = first;
    result = CadAnimation_Create(&core.data, selectedFace, 1, 4);
    CHECK(CadResult_IsSuccess(&result));
    result = CadAnimation_Inspect(&core.data, &info);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(info.frameCount == 4);
    CHECK(info.animatedFaceCount == 1);
    CHECK(info.staticFaceCount == 1);
    CHECK(info.attachedAnimationPointCount == 12);
    CHECK(info.editable && info.topologyLocked);
    CHECK(core.data.animationIndexCount == 1);

    position.x = 10.0;
    position.y = 0.0;
    position.z = 0.0;
    result = CadAnimation_SetPoint(&core.data, 1, 0, position);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(fabs(core.data.points[0].pointx) < 1e-12);
    displayed = pose_point(&core.data, 0.5, 1, 0);
    CHECK(fabs(displayed.x - 5.0) < 1e-12);

    position.x = 4.0;
    result = CadAnimation_SetPoint(&core.data, 0, 0, position);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(fabs(core.data.points[0].pointx - 4.0) < 1e-12);

    selectedPoint[0] = 0;
    transform = translation(0.0, 0.0, 2.0);
    result = CadAnimation_Transform(&core.data, 1,
                                    CAD_ANIMATION_CURRENT_FRAME,
                                    selectedPoint, 1, &transform);
    CHECK(CadResult_IsSuccess(&result));
    displayed = pose_point(&core.data, 1.0, 0, 0);
    CHECK(fabs(displayed.x - 10.0) < 1e-12);
    CHECK(fabs(displayed.z - 2.0) < 1e-12);
    CHECK(fabs(core.data.points[0].pointz) < 1e-12);

    transform = translation(0.0, 1.0, 0.0);
    result = CadAnimation_Transform(&core.data, 1,
                                    CAD_ANIMATION_ALL_FRAMES,
                                    selectedPoint, 1, &transform);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(fabs(core.data.points[0].pointy - 1.0) < 1e-12);
    displayed = pose_point(&core.data, 3.0, 0, 0);
    CHECK(fabs(displayed.y - 1.0) < 1e-12);

    result = CadAnimation_CopyFrame(&core.data, 1, 2, selectedPoint, 1);
    CHECK(CadResult_IsSuccess(&result));
    displayed = pose_point(&core.data, 2.0, 0, 0);
    CHECK(fabs(displayed.x - 10.0) < 1e-12);
    CHECK(fabs(displayed.z - 2.0) < 1e-12);

    selectedFace[0] = second;
    result = CadAnimation_AddFaces(&core.data, selectedFace, 1);
    CHECK(CadResult_IsSuccess(&result));
    result = CadAnimation_Inspect(&core.data, &info);
    CHECK(info.animatedFaceCount == 2 && info.staticFaceCount == 0);
    displayed = pose_point(&core.data, 3.0, 0, 3);
    CHECK(fabs(displayed.x - 20.0) < 1e-12);

    result = CadAnimation_SetFrameCount(&core.data, 5);
    CHECK(CadResult_IsSuccess(&result));
    result = CadAnimation_DuplicateFrame(&core.data, 1, 2);
    CHECK(CadResult_IsSuccess(&result));
    result = CadAnimation_Inspect(&core.data, &info);
    CHECK(info.frameCount == 6);
    displayed = pose_point(&core.data, 2.0, 0, 0);
    CHECK(fabs(displayed.x - 10.0) < 1e-12);
    CHECK(fabs(displayed.z - 2.0) < 1e-12);
    result = CadAnimation_InsertFrame(&core.data, 3, 0);
    CHECK(CadResult_IsSuccess(&result));
    result = CadAnimation_Inspect(&core.data, &info);
    CHECK(info.frameCount == 7);
    result = CadAnimation_DeleteFrame(&core.data, 3);
    CHECK(CadResult_IsSuccess(&result));
    result = CadAnimation_SetFrameCount(&core.data, 3);
    CHECK(CadResult_IsSuccess(&result));
    result = CadAnimation_Inspect(&core.data, &info);
    CHECK(info.frameCount == 3);

    result = CadPose_Evaluate(&core.data,
        CadPoseSample_FromFrame(0.5, 3, 0, 1), &pose);
    CHECK(CadResult_IsSuccess(&result));
    result = CadAnimation_MakeStaticCopy(&core.data, &pose, baked);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(!CadAnimation_HasAny(baked));
    CHECK(baked->polygons[first].animation == -1);
    CHECK(fabs(baked->points[0].pointx - 7.0) < 1e-12);

    free(baked);
    CadCore_Destroy(&core);
}

static void test_transactional_failures(void) {
    CadCore core;
    CadFileData before;
    CadResult result;
    CadAffineTransform transform;
    int16_t face;
    int16_t point = 0;
    CadCore_Init(&core);
    face = append_triangle(&core, 0.0);
    result = CadAnimation_Create(&core.data, &face, 1, 2);
    CHECK(CadResult_IsSuccess(&result));
    before = core.data;
    transform = translation(1.0, 0.0, 0.0);
    transform.matrix[0][0] = NAN;
    result = CadAnimation_Transform(&core.data, 0,
                                    CAD_ANIMATION_CURRENT_FRAME,
                                    &point, 1, &transform);
    CHECK(!CadResult_IsSuccess(&result));
    CHECK(memcmp(&before, &core.data, sizeof(before)) == 0);
    point = 1;
    transform = translation(DBL_MAX, 0.0, 0.0);
    transform.matrix[0][0] = DBL_MAX;
    result = CadAnimation_Transform(&core.data, 0,
                                    CAD_ANIMATION_CURRENT_FRAME,
                                    &point, 1, &transform);
    CHECK(!CadResult_IsSuccess(&result));
    CHECK(memcmp(&before, &core.data, sizeof(before)) == 0);
    result = CadAnimation_DeleteFrame(&core.data, 8);
    CHECK(!CadResult_IsSuccess(&result));
    CHECK(memcmp(&before, &core.data, sizeof(before)) == 0);
    CadCore_Destroy(&core);
}

static void test_unattached_records_are_preserved(void) {
    CadCore core;
    CadAnimationInfo info;
    CadResult result;
    int frame;
    int16_t face;
    CadCore_Init(&core);
    face = append_triangle(&core, 0.0);
    core.data.animationIndices[0].flags = 1;
    for (frame = 0; frame < CAD_ANIMATION_FRAMES; ++frame)
        core.data.animationIndices[0].frame[frame] = -1;
    core.data.animationPoints[0].flags = 2;
    core.data.animationPoints[0].nextPoint = -1;
    core.data.animationIndexCount = 1;
    core.data.animationPointCount = 1;
    result = CadAnimation_Inspect(&core.data, &info);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(info.unattachedIndexCount == 1);
    CHECK(info.unattachedPointCount == 1);
    CHECK(!info.editable && info.topologyLocked);
    result = CadAnimation_Create(&core.data, &face, 1, 2);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(core.data.animationIndices[0].flags);
    CHECK(core.data.animationPoints[0].flags);
    CHECK(core.data.polygons[face].animation == 1);
    result = CadAnimation_Inspect(&core.data, &info);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(info.unattachedIndexCount == 1);
    CHECK(info.unattachedPointCount == 1);
    CadCore_Destroy(&core);
}

static void test_default_and_capacity(void) {
    CadCore core;
    CadAnimationInfo info;
    CadResult result;
    int16_t face;
    CadCore_Init(&core);
    face = append_triangle(&core, 0.0);
    result = CadAnimation_Create(&core.data, &face, 1, 0);
    CHECK(CadResult_IsSuccess(&result));
    result = CadAnimation_Inspect(&core.data, &info);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(info.frameCount == 16);
    CHECK(info.maximumFrameCount == 64);
    result = CadAnimation_SetFrameCount(&core.data, 64);
    CHECK(CadResult_IsSuccess(&result));
    result = CadAnimation_InsertFrame(&core.data, 64, 63);
    CHECK(!CadResult_IsSuccess(&result));
    CadCore_Destroy(&core);
}

static void test_record_capacity_boundaries(void) {
    CadCore core;
    CadResult result;
    CadAnimationInfo info;
    CadFileData* before;
    int16_t faces[CAD_MAX_ANIMATION_INDICES];
    int16_t extraFace = CAD_MAX_ANIMATION_INDICES;
    int index;
    CadCore_Init(&core);
    before = (CadFileData*)malloc(sizeof(*before));
    CHECK(before != NULL);
    for (index = 0; index < CAD_MAX_ANIMATION_INDICES + 1; ++index) {
        int16_t face = append_polygon(&core, 2, index * 4.0);
        CHECK(face == index);
        if (index < CAD_MAX_ANIMATION_INDICES) faces[index] = face;
    }
    *before = core.data;
    result = CadAnimation_Create(&core.data, NULL, 0, 1);
    CHECK(!CadResult_IsSuccess(&result));
    CHECK(memcmp(before, &core.data, sizeof(*before)) == 0);
    result = CadAnimation_Create(&core.data, faces,
                                 CAD_MAX_ANIMATION_INDICES, 1);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(core.data.animationIndexCount == CAD_MAX_ANIMATION_INDICES);
    *before = core.data;
    result = CadAnimation_AddFaces(&core.data, &extraFace, 1);
    CHECK(!CadResult_IsSuccess(&result));
    CHECK(memcmp(before, &core.data, sizeof(*before)) == 0);
    free(before);
    CadCore_Destroy(&core);

    CadCore_Init(&core);
    for (index = 0; index < 64; ++index)
        CHECK(append_polygon(&core, CAD_MAX_FACE_POINTS,
                             index * 32.0) == index);
    result = CadAnimation_Create(&core.data, NULL, 0, 8);
    CHECK(CadResult_IsSuccess(&result));
    result = CadAnimation_Inspect(&core.data, &info);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(info.frameCount == 8);
    CHECK(info.maximumFrameCount == 8);
    CHECK(core.data.animationPointCount == CAD_MAX_ANIMATION_POINTS);
    result = CadAnimation_SetFrameCount(&core.data, 9);
    CHECK(!CadResult_IsSuccess(&result));
    CadCore_Destroy(&core);
}

static void test_session_clock(void) {
    CadCore core;
    CadResult result;
    CadAnimationSession session;
    CadScene scene;
    CadPosition position;
    CadFileData* unchanged;
    int16_t face;
    CadCore_Init(&core);
    unchanged = (CadFileData*)malloc(sizeof(*unchanged));
    CHECK(unchanged != NULL);
    face = append_triangle(&core, 0.0);
    result = CadAnimation_Create(&core.data, &face, 1, 3);
    CHECK(CadResult_IsSuccess(&result));
    position.y = position.z = 0.0;
    position.x = 0.0;
    result = CadAnimation_SetPoint(&core.data, 0, 0, position);
    CHECK(CadResult_IsSuccess(&result));
    position.x = 10.0;
    result = CadAnimation_SetPoint(&core.data, 1, 0, position);
    CHECK(CadResult_IsSuccess(&result));
    position.x = 20.0;
    result = CadAnimation_SetPoint(&core.data, 2, 0, position);
    CHECK(CadResult_IsSuccess(&result));

    CadAnimationSession_Init(&session);
    result = CadAnimationSession_Rebuild(&session, &core.data);
    CHECK(CadResult_IsSuccess(&result));
    *unchanged = core.data;
    CHECK(session.fps == 12.0 && session.interpolation && !session.loop);
    CadAnimationSession_Seek(&session, 0.49);
    CadAnimationSession_EndScrub(&session);
    CHECK(session.currentFrame == 0 && session.previewFrame == 0.0);
    CadAnimationSession_Seek(&session, 0.51);
    CadAnimationSession_EndScrub(&session);
    CHECK(session.currentFrame == 1 && session.previewFrame == 1.0);
    CadAnimationSession_SetFrame(&session, 0);
    CadAnimationSession_Play(&session, 10.0);
    result = CadAnimationSession_Evaluate(&session, &core.data, 10.125,
                                          &scene);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(scene.pose->sample.frameA == 1);
    CHECK(scene.pose->sample.frameB == 2);
    CHECK(fabs(scene.pose->sample.alpha - 0.5) < 1e-12);
    CHECK(fabs(scene.pose->points[0].x - 15.0) < 1e-12);
    CadAnimationSession_Pause(&session, 10.125);
    CHECK(!session.playing && session.currentFrame == 2);
    CadAnimationSession_Stop(&session);
    CHECK(session.currentFrame == 0);

    session.loop = 1;
    CadAnimationSession_SetFrame(&session, 2);
    CadAnimationSession_Play(&session, 20.0);
    result = CadAnimationSession_Evaluate(&session, &core.data,
                                          20.0 + 1.0 / 24.0, &scene);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(scene.pose->sample.frameA == 2);
    CHECK(scene.pose->sample.frameB == 0);
    CHECK(fabs(scene.pose->sample.alpha - 0.5) < 1e-9);
    CHECK(fabs(scene.pose->points[0].x - 10.0) < 1e-9);

    session.loop = 0;
    CadAnimationSession_SetFrame(&session, 0);
    CadAnimationSession_Play(&session, 30.0);
    result = CadAnimationSession_Evaluate(&session, &core.data, 31.0,
                                          &scene);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(!session.playing && session.currentFrame == 2);
    CHECK(fabs(scene.pose->points[0].x - 20.0) < 1e-12);
    CHECK(memcmp(unchanged, &core.data, sizeof(*unchanged)) == 0);
    free(unchanged);
    CadCore_Destroy(&core);
}

static void test_interpolation_quarters(void) {
    CadCore core;
    CadResult result;
    CadPosition position = { 0.0, 0.0, 0.0 };
    CadPosition sampled;
    int16_t face;
    CadCore_Init(&core);
    face = append_triangle(&core, 0.0);
    result = CadAnimation_Create(&core.data, &face, 1, 2);
    CHECK(CadResult_IsSuccess(&result));
    result = CadAnimation_SetPoint(&core.data, 0, 0, position);
    CHECK(CadResult_IsSuccess(&result));
    position.x = 8.0;
    result = CadAnimation_SetPoint(&core.data, 1, 0, position);
    CHECK(CadResult_IsSuccess(&result));
    sampled = pose_point(&core.data, 0.25, 1, 0);
    CHECK(fabs(sampled.x - 2.0) < 1e-12);
    sampled = pose_point(&core.data, 0.50, 1, 0);
    CHECK(fabs(sampled.x - 4.0) < 1e-12);
    sampled = pose_point(&core.data, 0.75, 1, 0);
    CHECK(fabs(sampled.x - 6.0) < 1e-12);
    {
        CadPose pose;
        CadPoseSample sample = CadPoseSample_FromFrame(1.75, 2, 1, 1);
        result = CadPose_Evaluate(&core.data, sample, &pose);
        CHECK(CadResult_IsSuccess(&result));
        CHECK(sample.frameA == 1 && sample.frameB == 0);
        CHECK(fabs(sample.alpha - 0.75) < 1e-12);
        CHECK(fabs(pose.points[0].x - 2.0) < 1e-12);
    }
    CadCore_Destroy(&core);
}

static void test_pose_uses_shared_small_polygon_normal(void) {
    const double edge = 2.0e-8;
    CadCore core;
    CadPose pose;
    CadResult result;
    int16_t first;
    int16_t second;
    int16_t third;
    int16_t face;
    CadCore_Init(&core);
    first = CadCore_AddPoint(&core, 0.0, 0.0, 0.0);
    second = CadCore_AddPoint(&core, edge, 0.0, 0.0);
    third = CadCore_AddPoint(&core, 0.0, edge, 0.0);
    CHECK(first >= 0 && second >= 0 && third >= 0);
    core.data.points[first].nextPoint = second;
    core.data.points[second].nextPoint = third;
    face = CadCore_AddPolygon(&core, first, 5, 3);
    CHECK(face >= 0);
    result = CadPose_Evaluate(
        &core.data, CadPoseSample_FromFrame(0.0, 1, 0, 1), &pose);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(pose.faceNormalValid[face]);
    CHECK(fabs(pose.faceNormals[face].x) < 1e-12);
    CHECK(fabs(pose.faceNormals[face].y) < 1e-12);
    CHECK(fabs(pose.faceNormals[face].z - 1.0) < 1e-12);
    CHECK(pose.faceSide[face] == 2);
    CadCore_Destroy(&core);
}

int main(void) {
    test_lifecycle_and_pose();
    test_transactional_failures();
    test_unattached_records_are_preserved();
    test_default_and_capacity();
    test_record_capacity_boundaries();
    test_session_clock();
    test_interpolation_quarters();
    test_pose_uses_shared_small_polygon_normal();
    if (failures) {
        fprintf(stderr, "%d animation test(s) failed\n", failures);
        return 1;
    }
    puts("animation tests passed");
    return 0;
}
