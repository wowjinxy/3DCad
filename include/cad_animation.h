#pragma once

/* SDL/Win32-independent fixed-topology animation authoring and pose preview.
   The routines operate directly on the recovered X11 tags 3/4 representation;
   they do not introduce timing or interpolation data into the document. */

#include "cad_codec.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum CadAnimationScope {
    CAD_ANIMATION_CURRENT_FRAME = 0,
    CAD_ANIMATION_ALL_FRAMES = 1
} CadAnimationScope;

typedef struct CadPosition {
    double x;
    double y;
    double z;
} CadPosition;

/* Applied as pivot + matrix * (position - pivot) + translation. */
typedef struct CadAffineTransform {
    double matrix[3][3];
    CadPosition pivot;
    CadPosition translation;
} CadAffineTransform;

typedef struct CadAnimationInfo {
    int frameCount;
    int animatedFaceCount;
    int staticFaceCount;
    int attachedAnimationPointCount;
    int unattachedIndexCount;
    int unattachedPointCount;
    int maximumFrameCount;
    int editable;
    int topologyLocked;
} CadAnimationInfo;

typedef struct CadPoseSample {
    int frameA;
    int frameB;
    double alpha;
    int interpolated;
} CadPoseSample;

/* Positions retain stable static point IDs.  Normals and side bits are
   derived from the displayed pose and never written back by evaluation. */
typedef struct CadPose {
    CadPoseSample sample;
    CadPosition points[CAD_MAX_POINTS];
    uint8_t pointValid[CAD_MAX_POINTS];
    CadPosition faceNormals[CAD_MAX_POLYGONS];
    uint8_t faceNormalValid[CAD_MAX_POLYGONS];
    uint8_t faceSide[CAD_MAX_POLYGONS];
    uint64_t generation;
} CadPose;

typedef struct CadScene {
    const CadFileData* topology;
    const CadPose* pose;
    uint64_t generation;
} CadScene;

/* The session owns all playback and pose scratch storage.  Rebuild its cache
   after a document mutation or replacement. */
typedef struct CadAnimationSession {
    double fps;
    double previewFrame;
    double lastClockSeconds;
    int frameCount;
    int currentFrame;
    int playbackStartFrame;
    int playing;
    int loop;
    int interpolation;
    int allFrames;
    int cacheValid;
    uint64_t cacheGeneration;
    uint64_t poseGeneration;
    int16_t animationPointForBasePoint[CAD_ANIMATION_FRAMES][CAD_MAX_POINTS];
    CadPose pose;
} CadAnimationSession;

/* Inspection treats attached, uniformly-sized animation chains as editable
   while reporting unattached records which codecs must preserve verbatim. */
CadResult CadAnimation_Inspect(const CadFileData* data,
                               CadAnimationInfo* information);
int CadAnimation_HasAny(const CadFileData* data);
int CadAnimation_TopologyLocked(const CadFileData* data);

/* A NULL/empty polygon list means all active faces.  A frameCount of zero
   selects min(16, the capacity maximum); explicit counts must fit 1..64. */
CadResult CadAnimation_Create(CadFileData* data,
                              const int16_t* polygonIndices,
                              size_t polygonCount,
                              int frameCount);
CadResult CadAnimation_AddFaces(CadFileData* data,
                                const int16_t* polygonIndices,
                                size_t polygonCount);
CadResult CadAnimation_SetFrameCount(CadFileData* data, int frameCount);
CadResult CadAnimation_InsertFrame(CadFileData* data, int insertAt,
                                   int sourceFrame);
CadResult CadAnimation_DuplicateFrame(CadFileData* data, int sourceFrame,
                                      int insertAt);
CadResult CadAnimation_DeleteFrame(CadFileData* data, int frameIndex);

/* A NULL/empty base-point list copies the complete pose. */
CadResult CadAnimation_CopyFrame(CadFileData* data, int sourceFrame,
                                 int targetFrame,
                                 const int16_t* basePointIndices,
                                 size_t basePointCount);
CadResult CadAnimation_SetPoint(CadFileData* data, int frameIndex,
                                int16_t basePointIndex,
                                CadPosition position);
CadResult CadAnimation_Transform(CadFileData* data, int currentFrame,
                                 CadAnimationScope scope,
                                 const int16_t* basePointIndices,
                                 size_t basePointCount,
                                 const CadAffineTransform* transform);

/* Produces an animation-free copy from the exact supplied pose.  output may
   alias source; replacement occurs only after validation succeeds. */
CadResult CadAnimation_MakeStaticCopy(const CadFileData* source,
                                      const CadPose* displayedPose,
                                      CadFileData* output);

CadPoseSample CadPoseSample_FromFrame(double framePosition, int frameCount,
                                      int loop, int interpolation);
CadResult CadPose_Evaluate(const CadFileData* data, CadPoseSample sample,
                           CadPose* output);
CadResult CadScene_Build(const CadFileData* data, CadPose* poseStorage,
                         CadPoseSample sample, CadScene* output);
int CadScene_GetPoint(const CadScene* scene, int16_t pointIndex,
                      CadPosition* output);

void CadAnimationSession_Init(CadAnimationSession* session);
CadResult CadAnimationSession_Rebuild(CadAnimationSession* session,
                                      const CadFileData* data);
void CadAnimationSession_SetFrame(CadAnimationSession* session,
                                  int frameIndex);
void CadAnimationSession_Seek(CadAnimationSession* session,
                              double framePosition);
void CadAnimationSession_EndScrub(CadAnimationSession* session);
void CadAnimationSession_Play(CadAnimationSession* session,
                              double nowSeconds);
void CadAnimationSession_Pause(CadAnimationSession* session,
                               double nowSeconds);
void CadAnimationSession_Stop(CadAnimationSession* session);
void CadAnimationSession_BeginEdit(CadAnimationSession* session,
                                   double nowSeconds);
CadResult CadAnimationSession_Evaluate(CadAnimationSession* session,
                                       const CadFileData* data,
                                       double nowSeconds,
                                       CadScene* output);

#ifdef __cplusplus
}
#endif
