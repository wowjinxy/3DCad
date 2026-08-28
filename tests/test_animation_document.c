#include "cad_animation.h"
#include "cad_document.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", \
                __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static int16_t add_triangle(CadCore* core) {
    int16_t a = CadCore_AddPoint(core, 0.0, 0.0, 0.0);
    int16_t b = CadCore_AddPoint(core, 1.0, 0.0, 0.0);
    int16_t c = CadCore_AddPoint(core, 0.0, 1.0, 0.0);
    if (a < 0 || b < 0 || c < 0) return -1;
    core->data.points[a].nextPoint = b;
    core->data.points[b].nextPoint = c;
    return CadCore_AddPolygon(core, a, 4, 3);
}

static double displayed_x(const CadDocument* document, double frame) {
    CadPose pose;
    CadAnimationInfo info;
    CadResult result = CadAnimation_Inspect(&document->core.data, &info);
    CHECK(CadResult_IsSuccess(&result));
    result = CadPose_Evaluate(
        &document->core.data,
        CadPoseSample_FromFrame(frame, info.frameCount, 0, 1), &pose);
    CHECK(CadResult_IsSuccess(&result));
    return pose.points[0].x;
}

static void test_frame_operations_are_named_transactions(void) {
    CadDocument document;
    CadAnimationInfo info;
    CadAffineTransform transform;
    CadPose pose;
    CadFileData* baked = (CadFileData*)malloc(sizeof(*baked));
    CadResult result;
    CadPosition position = {10.0, 0.0, 0.0};
    int16_t point = 0;
    int16_t face;
    CHECK(baked != NULL);
    if (!baked) return;
    CadDocument_Init(&document);

    result = CadDocument_BeginEditNamed(&document, "Create Triangle");
    CHECK(CadResult_IsSuccess(&result));
    face = add_triangle(&document.core);
    CHECK(face >= 0);
    result = CadAnimation_Create(&document.core.data, &face, 1, 3);
    CHECK(CadResult_IsSuccess(&result));
    result = CadDocument_CommitEdit(&document);
    CHECK(CadResult_IsSuccess(&result));

    result = CadDocument_BeginEditNamed(&document, "Set Frame Count");
    CHECK(CadResult_IsSuccess(&result));
    result = CadAnimation_SetFrameCount(&document.core.data, 4);
    CHECK(CadResult_IsSuccess(&result));
    result = CadDocument_CommitEdit(&document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(strcmp(CadDocument_GetUndoLabel(&document), "Set Frame Count") == 0);
    result = CadDocument_Undo(&document);
    CHECK(CadResult_IsSuccess(&result));
    result = CadAnimation_Inspect(&document.core.data, &info);
    CHECK(CadResult_IsSuccess(&result) && info.frameCount == 3);
    result = CadDocument_Redo(&document);
    CHECK(CadResult_IsSuccess(&result));

    result = CadDocument_BeginEditNamed(&document, "Duplicate Frame");
    CHECK(CadResult_IsSuccess(&result));
    result = CadAnimation_DuplicateFrame(&document.core.data, 1, 2);
    CHECK(CadResult_IsSuccess(&result));
    result = CadDocument_CommitEdit(&document);
    CHECK(CadResult_IsSuccess(&result));
    result = CadAnimation_Inspect(&document.core.data, &info);
    CHECK(info.frameCount == 5);
    result = CadDocument_Undo(&document);
    CHECK(CadResult_IsSuccess(&result));
    result = CadAnimation_Inspect(&document.core.data, &info);
    CHECK(info.frameCount == 4);
    result = CadDocument_Redo(&document);
    CHECK(CadResult_IsSuccess(&result));

    result = CadDocument_BeginEditNamed(&document, "Insert Frame");
    CHECK(CadResult_IsSuccess(&result));
    result = CadAnimation_InsertFrame(&document.core.data, 3, 0);
    CHECK(CadResult_IsSuccess(&result));
    result = CadDocument_CommitEdit(&document);
    CHECK(CadResult_IsSuccess(&result));
    result = CadAnimation_Inspect(&document.core.data, &info);
    CHECK(info.frameCount == 6);

    result = CadDocument_BeginEditNamed(&document, "Delete Frame");
    CHECK(CadResult_IsSuccess(&result));
    result = CadAnimation_DeleteFrame(&document.core.data, 3);
    CHECK(CadResult_IsSuccess(&result));
    result = CadDocument_CommitEdit(&document);
    CHECK(CadResult_IsSuccess(&result));
    result = CadAnimation_Inspect(&document.core.data, &info);
    CHECK(info.frameCount == 5);
    result = CadDocument_Undo(&document);
    CHECK(CadResult_IsSuccess(&result));
    result = CadAnimation_Inspect(&document.core.data, &info);
    CHECK(info.frameCount == 6);
    result = CadDocument_Redo(&document);
    CHECK(CadResult_IsSuccess(&result));

    result = CadDocument_BeginEditNamed(&document, "Set Source Pose");
    CHECK(CadResult_IsSuccess(&result));
    result = CadAnimation_SetPoint(&document.core.data, 1, point, position);
    CHECK(CadResult_IsSuccess(&result));
    result = CadDocument_CommitEdit(&document);
    CHECK(CadResult_IsSuccess(&result));
    result = CadDocument_BeginEditNamed(&document, "Copy Complete Pose");
    CHECK(CadResult_IsSuccess(&result));
    result = CadAnimation_CopyFrame(&document.core.data, 1, 4, NULL, 0);
    CHECK(CadResult_IsSuccess(&result));
    result = CadDocument_CommitEdit(&document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(fabs(displayed_x(&document, 4.0) - 10.0) < 1e-12);
    result = CadDocument_Undo(&document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(fabs(displayed_x(&document, 4.0)) < 1e-12);
    result = CadDocument_Redo(&document);
    CHECK(CadResult_IsSuccess(&result));

    memset(&transform, 0, sizeof(transform));
    transform.matrix[0][0] = 1.0;
    transform.matrix[1][1] = 1.0;
    transform.matrix[2][2] = 1.0;
    transform.translation.x = 1.0;
    result = CadDocument_BeginEditNamed(&document, "Move All Frames");
    CHECK(CadResult_IsSuccess(&result));
    result = CadAnimation_Transform(&document.core.data, 1,
                                    CAD_ANIMATION_ALL_FRAMES,
                                    &point, 1, &transform);
    CHECK(CadResult_IsSuccess(&result));
    result = CadDocument_CommitEdit(&document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(fabs(displayed_x(&document, 0.0) - 1.0) < 1e-12);
    CHECK(fabs(displayed_x(&document, 1.0) - 11.0) < 1e-12);
    result = CadDocument_Undo(&document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(fabs(displayed_x(&document, 0.0)) < 1e-12);
    result = CadDocument_Redo(&document);
    CHECK(CadResult_IsSuccess(&result));

    result = CadAnimation_Inspect(&document.core.data, &info);
    CHECK(CadResult_IsSuccess(&result));
    result = CadPose_Evaluate(
        &document.core.data,
        CadPoseSample_FromFrame(0.5, info.frameCount, 0, 1), &pose);
    CHECK(CadResult_IsSuccess(&result));
    result = CadAnimation_MakeStaticCopy(&document.core.data, &pose, baked);
    CHECK(CadResult_IsSuccess(&result));
    result = CadDocument_BeginEditNamed(&document,
                                        "Make Static Copy from Displayed Pose");
    CHECK(CadResult_IsSuccess(&result));
    document.core.data = *baked;
    CadCore_RebuildDerivedState(&document.core);
    result = CadDocument_CommitEdit(&document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(!CadAnimation_HasAny(&document.core.data));
    CHECK(fabs(document.core.data.points[0].pointx - 6.0) < 1e-12);
    result = CadDocument_Undo(&document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(CadAnimation_HasAny(&document.core.data));
    result = CadDocument_Redo(&document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(!CadAnimation_HasAny(&document.core.data));

    CadDocument_Destroy(&document);
    free(baked);
}

static void test_preview_does_not_change_document_history(void) {
    CadDocument document;
    CadAnimationSession session;
    CadAnimationInfo info;
    CadScene scene;
    CadPosition changed = { 8.0, 0.0, 0.0 };
    CadResult result;
    int16_t face;
    uint64_t previewRevision;
    unsigned previewHistory;

    CadDocument_Init(&document);
    result = CadDocument_BeginEditNamed(&document, "Create Triangle");
    CHECK(CadResult_IsSuccess(&result));
    face = add_triangle(&document.core);
    CHECK(face >= 0);
    result = CadDocument_CommitEdit(&document);
    CHECK(CadResult_IsSuccess(&result));

    result = CadDocument_BeginEditNamed(&document, "Create Animation");
    CHECK(CadResult_IsSuccess(&result));
    result = CadAnimation_Create(&document.core.data, NULL, 0, 3);
    CHECK(CadResult_IsSuccess(&result));
    result = CadDocument_CommitEdit(&document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(strcmp(CadDocument_GetUndoLabel(&document), "Create Animation") == 0);
    result = CadAnimation_Inspect(&document.core.data, &info);
    CHECK(CadResult_IsSuccess(&result) && info.frameCount == 3);
    result = CadDocument_Undo(&document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(!CadAnimation_HasAny(&document.core.data));
    result = CadDocument_Redo(&document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(CadAnimation_HasAny(&document.core.data));

    result = CadDocument_BeginEditNamed(&document, "Edit Frame 1");
    CHECK(CadResult_IsSuccess(&result));
    result = CadAnimation_SetPoint(&document.core.data, 1, 0, changed);
    CHECK(CadResult_IsSuccess(&result));
    result = CadDocument_CommitEdit(&document);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(fabs(document.core.data.points[0].pointx) < 1e-12);

    CadAnimationSession_Init(&session);
    result = CadAnimationSession_Rebuild(&session, &document.core.data);
    CHECK(CadResult_IsSuccess(&result));
    CadAnimationSession_Seek(&session, 0.5);
    previewRevision = document.revision;
    previewHistory = document.historyCount;
    result = CadAnimationSession_Evaluate(&session, &document.core.data,
                                          10.0, &scene);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(fabs(scene.pose->points[0].x - 4.0) < 1e-12);
    CHECK(document.revision == previewRevision);
    CHECK(document.historyCount == previewHistory);
    CHECK(document.isDirty);

    result = CadDocument_Undo(&document);
    CHECK(CadResult_IsSuccess(&result));
    CadAnimationSession_Rebuild(&session, &document.core.data);
    CadAnimationSession_SetFrame(&session, 1);
    result = CadAnimationSession_Evaluate(&session, &document.core.data,
                                          10.0, &scene);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(fabs(scene.pose->points[0].x) < 1e-12);
    CadDocument_Destroy(&document);
}

int main(void) {
    test_preview_does_not_change_document_history();
    test_frame_operations_are_named_transactions();

    if (failures) {
        fprintf(stderr, "%d animation document test(s) failed\n", failures);
        return 1;
    }
    puts("All animation document tests passed.");
    return 0;
}
