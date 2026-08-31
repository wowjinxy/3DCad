#include "cad_animation.h"
#include "cad_geometry.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAD_ANIMATION_DEFAULT_FRAMES 16

static CadResult animation_error(CadStatus status, int tag, int index,
                                 const char* format, ...) {
    CadResult result = CadResult_Ok(CAD_FORMAT_X11_STREAM);
    CadDiagnostic* diagnostic = &result.diagnostics[0];
    va_list args;
    result.status = status;
    result.errorCount = 1;
    result.diagnosticCount = 1;
    diagnostic->severity = CAD_DIAGNOSTIC_ERROR;
    diagnostic->code = status;
    diagnostic->byteOffset = 0;
    diagnostic->recordTag = tag;
    diagnostic->recordIndex = index;
    va_start(args, format);
    vsnprintf(diagnostic->message, sizeof(diagnostic->message), format, args);
    va_end(args);
    return result;
}

static int active_polygon(const CadFileData* data, int index) {
    return data && index >= 0 && index < CAD_MAX_POLYGONS &&
           data->polygons[index].flags != 0;
}

static int active_point(const CadFileData* data, int index) {
    return data && index >= 0 && index < CAD_MAX_POINTS &&
           data->points[index].flags != 0;
}

static int active_animation_index(const CadFileData* data, int index) {
    return data && index >= 0 && index < CAD_MAX_ANIMATION_INDICES &&
           data->animationIndices[index].flags != 0;
}

static int active_animation_point(const CadFileData* data, int index) {
    return data && index >= 0 && index < CAD_MAX_ANIMATION_POINTS &&
           data->animationPoints[index].flags != 0;
}

static int count_frames(const CadAnimationIndex* animation) {
    int frame = 0;
    if (!animation || !animation->flags) return 0;
    while (frame < CAD_ANIMATION_FRAMES && animation->frame[frame] != -1)
        ++frame;
    return frame;
}

static int get_static_chain(const CadFileData* data, int polygonIndex,
                            int16_t output[CAD_MAX_FACE_POINTS]) {
    const CadPolygon* polygon;
    int16_t point;
    int count;
    if (!active_polygon(data, polygonIndex)) return 0;
    polygon = &data->polygons[polygonIndex];
    point = polygon->firstPoint;
    for (count = 0; count < polygon->npoints; ++count) {
        if (!active_point(data, point)) return 0;
        output[count] = point;
        point = data->points[point].nextPoint;
    }
    return point == -1 ? count : 0;
}

static int get_animation_chain(const CadFileData* data, int polygonIndex,
                               int frame,
                               int16_t output[CAD_MAX_FACE_POINTS]) {
    const CadPolygon* polygon;
    const CadAnimationIndex* animation;
    int16_t point;
    int count;
    if (!active_polygon(data, polygonIndex) || frame < 0 ||
        frame >= CAD_ANIMATION_FRAMES) return 0;
    polygon = &data->polygons[polygonIndex];
    if (!active_animation_index(data, polygon->animation)) return 0;
    animation = &data->animationIndices[polygon->animation];
    point = animation->frame[frame];
    for (count = 0; count < polygon->npoints; ++count) {
        if (!active_animation_point(data, point)) return 0;
        output[count] = point;
        point = data->animationPoints[point].nextPoint;
    }
    return point == -1 ? count : 0;
}

static void rebuild_animation_counts(CadFileData* data) {
    int i;
    if (!data) return;
    data->animationIndexCount = 0;
    data->animationPointCount = 0;
    for (i = 0; i < CAD_MAX_ANIMATION_INDICES; ++i)
        if (data->animationIndices[i].flags)
            data->animationIndexCount = i + 1;
    for (i = 0; i < CAD_MAX_ANIMATION_POINTS; ++i)
        if (data->animationPoints[i].flags)
            data->animationPointCount = i + 1;
}

static int find_free_animation_index(const CadFileData* data) {
    int index;
    for (index = 0; index < CAD_MAX_ANIMATION_INDICES; ++index)
        if (!data->animationIndices[index].flags) return index;
    return -1;
}

static int find_free_animation_point(const CadFileData* data) {
    int index;
    for (index = 0; index < CAD_MAX_ANIMATION_POINTS; ++index)
        if (!data->animationPoints[index].flags) return index;
    return -1;
}

static int available_animation_indices(const CadFileData* data) {
    int index;
    int count = 0;
    for (index = 0; index < CAD_MAX_ANIMATION_INDICES; ++index)
        if (!data->animationIndices[index].flags) ++count;
    return count;
}

static int available_animation_points(const CadFileData* data) {
    int index;
    int count = 0;
    for (index = 0; index < CAD_MAX_ANIMATION_POINTS; ++index)
        if (!data->animationPoints[index].flags) ++count;
    return count;
}

static void initialize_animation_index(CadAnimationIndex* animation) {
    int frame;
    memset(animation, 0, sizeof(*animation));
    animation->flags = 1;
    for (frame = 0; frame < CAD_ANIMATION_FRAMES; ++frame)
        animation->frame[frame] = -1;
}

static void clear_animation_point(CadAnimationPoint* point) {
    memset(point, 0, sizeof(*point));
    point->nextPoint = -1;
}

static int allocate_chain(CadFileData* data, const CadPosition* positions,
                          int count, int16_t* firstPoint) {
    int16_t allocated[CAD_MAX_FACE_POINTS];
    int ordinal;
    if (!data || !positions || !firstPoint || count < CAD_MIN_FACE_POINTS ||
        count > CAD_MAX_FACE_POINTS) return 0;
    for (ordinal = 0; ordinal < count; ++ordinal) {
        int index = find_free_animation_point(data);
        CadAnimationPoint* point;
        if (index < 0) return 0;
        allocated[ordinal] = (int16_t)index;
        point = &data->animationPoints[index];
        clear_animation_point(point);
        point->flags = 2;
        point->pointx = positions[ordinal].x;
        point->pointy = positions[ordinal].y;
        point->pointz = positions[ordinal].z;
        if (ordinal > 0)
            data->animationPoints[allocated[ordinal - 1]].nextPoint =
                allocated[ordinal];
    }
    *firstPoint = allocated[0];
    return 1;
}

static void free_chain(CadFileData* data, int16_t firstPoint, int count) {
    int ordinal;
    int16_t point = firstPoint;
    for (ordinal = 0; ordinal < count && point != -1; ++ordinal) {
        int16_t next;
        if (!active_animation_point(data, point)) break;
        next = data->animationPoints[point].nextPoint;
        clear_animation_point(&data->animationPoints[point]);
        point = next;
    }
}

static int static_positions(const CadFileData* data, int polygonIndex,
                            CadPosition output[CAD_MAX_FACE_POINTS]) {
    int16_t chain[CAD_MAX_FACE_POINTS];
    int count = get_static_chain(data, polygonIndex, chain);
    int ordinal;
    for (ordinal = 0; ordinal < count; ++ordinal) {
        const CadPoint* point = &data->points[chain[ordinal]];
        output[ordinal].x = point->pointx;
        output[ordinal].y = point->pointy;
        output[ordinal].z = point->pointz;
    }
    return count;
}

static int animation_positions(const CadFileData* data, int polygonIndex,
                               int frame,
                               CadPosition output[CAD_MAX_FACE_POINTS]) {
    int16_t chain[CAD_MAX_FACE_POINTS];
    int count = get_animation_chain(data, polygonIndex, frame, chain);
    int ordinal;
    for (ordinal = 0; ordinal < count; ++ordinal) {
        const CadAnimationPoint* point = &data->animationPoints[chain[ordinal]];
        output[ordinal].x = point->pointx;
        output[ordinal].y = point->pointy;
        output[ordinal].z = point->pointz;
    }
    return count;
}

static CadResult begin_mutation(CadFileData* data, CadFileData** backup,
                                CadAnimationInfo* information) {
    CadResult result;
    if (!data || !backup)
        return animation_error(CAD_STATUS_INVALID_ARGUMENT, -1, -1,
                               "Animation edit received a NULL argument");
    result = CadAnimation_Inspect(data, information);
    if (!CadResult_IsSuccess(&result)) return result;
    *backup = (CadFileData*)malloc(sizeof(**backup));
    if (!*backup)
        return animation_error(CAD_STATUS_OUT_OF_MEMORY, -1, -1,
                               "Not enough memory to begin animation edit");
    **backup = *data;
    return result;
}

static CadResult abort_mutation(CadFileData* data, CadFileData* backup,
                                CadResult failure) {
    if (data && backup) *data = *backup;
    free(backup);
    return failure;
}

static CadResult finish_mutation(CadFileData* data, CadFileData* backup) {
    CadResult result;
    rebuild_animation_counts(data);
    result = CadCodec_Validate(data);
    if (!CadResult_IsSuccess(&result)) {
        *data = *backup;
        free(backup);
        return result;
    }
    free(backup);
    result.format = CAD_FORMAT_X11_STREAM;
    return result;
}

static void mark_attached_records(const CadFileData* data,
                                  uint8_t ownedIndices[CAD_MAX_ANIMATION_INDICES],
                                  uint8_t ownedPoints[CAD_MAX_ANIMATION_POINTS],
                                  int* frameCount,
                                  int* animatedFaces,
                                  int* attachedPoints) {
    int polygonIndex;
    memset(ownedIndices, 0, CAD_MAX_ANIMATION_INDICES);
    memset(ownedPoints, 0, CAD_MAX_ANIMATION_POINTS);
    *frameCount = 0;
    *animatedFaces = 0;
    *attachedPoints = 0;
    for (polygonIndex = 0; polygonIndex < CAD_MAX_POLYGONS; ++polygonIndex) {
        const CadPolygon* polygon = &data->polygons[polygonIndex];
        int frames;
        int frame;
        if (!polygon->flags || polygon->animation == -1) continue;
        ownedIndices[polygon->animation] = 1;
        frames = count_frames(&data->animationIndices[polygon->animation]);
        if (!*frameCount) *frameCount = frames;
        ++*animatedFaces;
        for (frame = 0; frame < frames; ++frame) {
            int16_t point =
                data->animationIndices[polygon->animation].frame[frame];
            int ordinal;
            for (ordinal = 0; ordinal < polygon->npoints; ++ordinal) {
                if (!active_animation_point(data, point)) break;
                if (!ownedPoints[point]) {
                    ownedPoints[point] = 1;
                    ++*attachedPoints;
                }
                point = data->animationPoints[point].nextPoint;
            }
        }
    }
}

CadResult CadAnimation_Inspect(const CadFileData* data,
                               CadAnimationInfo* information) {
    CadResult result;
    CadAnimationInfo info;
    uint8_t ownedIndices[CAD_MAX_ANIMATION_INDICES];
    uint8_t ownedPoints[CAD_MAX_ANIMATION_POINTS];
    int vertexCountPerFrame = 0;
    int polygonIndex;
    int index;
    memset(&info, 0, sizeof(info));
    if (!data)
        return animation_error(CAD_STATUS_INVALID_ARGUMENT, -1, -1,
                               "Cannot inspect a NULL CAD document");
    result = CadCodec_Validate(data);
    if (!CadResult_IsSuccess(&result)) return result;
    mark_attached_records(data, ownedIndices, ownedPoints, &info.frameCount,
                          &info.animatedFaceCount,
                          &info.attachedAnimationPointCount);
    for (polygonIndex = 0; polygonIndex < CAD_MAX_POLYGONS; ++polygonIndex) {
        const CadPolygon* polygon = &data->polygons[polygonIndex];
        if (!polygon->flags) continue;
        if (polygon->animation == -1) ++info.staticFaceCount;
        else vertexCountPerFrame += polygon->npoints;
    }
    for (index = 0; index < CAD_MAX_ANIMATION_INDICES; ++index) {
        if (data->animationIndices[index].flags && !ownedIndices[index])
            ++info.unattachedIndexCount;
    }
    for (index = 0; index < CAD_MAX_ANIMATION_POINTS; ++index) {
        if (data->animationPoints[index].flags && !ownedPoints[index])
            ++info.unattachedPointCount;
    }
    info.editable = info.animatedFaceCount > 0;
    info.topologyLocked = info.editable || info.unattachedIndexCount > 0 ||
                          info.unattachedPointCount > 0;
    if (info.editable && vertexCountPerFrame > 0) {
        info.maximumFrameCount = info.frameCount +
            available_animation_points(data) / vertexCountPerFrame;
        if (info.maximumFrameCount > CAD_ANIMATION_FRAMES)
            info.maximumFrameCount = CAD_ANIMATION_FRAMES;
    } else {
        int activeFaces = 0;
        int vertices = 0;
        for (polygonIndex = 0; polygonIndex < CAD_MAX_POLYGONS;
             ++polygonIndex) {
            if (!data->polygons[polygonIndex].flags) continue;
            ++activeFaces;
            vertices += data->polygons[polygonIndex].npoints;
        }
        if (activeFaces > 0 &&
            activeFaces <= available_animation_indices(data) && vertices > 0) {
            info.maximumFrameCount =
                available_animation_points(data) / vertices;
            if (info.maximumFrameCount > CAD_ANIMATION_FRAMES)
                info.maximumFrameCount = CAD_ANIMATION_FRAMES;
        }
    }
    if (information) *information = info;
    return result;
}

int CadAnimation_HasAny(const CadFileData* data) {
    int index;
    if (!data) return 0;
    for (index = 0; index < CAD_MAX_POLYGONS; ++index)
        if (data->polygons[index].flags &&
            data->polygons[index].animation != -1) return 1;
    for (index = 0; index < CAD_MAX_ANIMATION_INDICES; ++index)
        if (data->animationIndices[index].flags) return 1;
    for (index = 0; index < CAD_MAX_ANIMATION_POINTS; ++index)
        if (data->animationPoints[index].flags) return 1;
    return 0;
}

int CadAnimation_TopologyLocked(const CadFileData* data) {
    return CadAnimation_HasAny(data);
}

static CadResult collect_target_faces(const CadFileData* data,
                                      const int16_t* requested,
                                      size_t requestedCount,
                                      int requireStatic,
                                      int16_t output[CAD_MAX_ANIMATION_INDICES],
                                      int* outputCount,
                                      int* verticesPerFrame) {
    uint8_t seen[CAD_MAX_POLYGONS];
    int count = 0;
    int vertices = 0;
    size_t cursor;
    int polygonIndex;
    memset(seen, 0, sizeof(seen));
    if (!output || !outputCount || !verticesPerFrame)
        return animation_error(CAD_STATUS_INVALID_ARGUMENT, -1, -1,
                               "Animation face selection is invalid");
    if (!requested || requestedCount == 0) {
        for (polygonIndex = 0; polygonIndex < CAD_MAX_POLYGONS;
             ++polygonIndex) {
            const CadPolygon* polygon = &data->polygons[polygonIndex];
            if (!polygon->flags) continue;
            if (requireStatic && polygon->animation != -1) continue;
            if (count >= CAD_MAX_ANIMATION_INDICES)
                return animation_error(CAD_STATUS_INDEX_OUT_OF_RANGE,
                                       CAD_TAG_POLYGON, polygonIndex,
                                       "More than %d faces cannot be animated",
                                       CAD_MAX_ANIMATION_INDICES);
            output[count++] = (int16_t)polygonIndex;
            vertices += polygon->npoints;
        }
    } else {
        if (requestedCount > CAD_MAX_ANIMATION_INDICES)
            return animation_error(CAD_STATUS_INDEX_OUT_OF_RANGE,
                                   CAD_TAG_POLYGON, -1,
                                   "More than %d faces cannot be animated",
                                   CAD_MAX_ANIMATION_INDICES);
        for (cursor = 0; cursor < requestedCount; ++cursor) {
            polygonIndex = requested[cursor];
            if (!active_polygon(data, polygonIndex))
                return animation_error(CAD_STATUS_INDEX_OUT_OF_RANGE,
                                       CAD_TAG_POLYGON, polygonIndex,
                                       "Face %d is inactive or out of range",
                                       polygonIndex);
            if (seen[polygonIndex])
                return animation_error(CAD_STATUS_INVALID_ARGUMENT,
                                       CAD_TAG_POLYGON, polygonIndex,
                                       "Face %d appears more than once",
                                       polygonIndex);
            if (requireStatic && data->polygons[polygonIndex].animation != -1)
                return animation_error(CAD_STATUS_INVALID_ARGUMENT,
                                       CAD_TAG_POLYGON, polygonIndex,
                                       "Face %d is already animated",
                                       polygonIndex);
            seen[polygonIndex] = 1;
            output[count++] = (int16_t)polygonIndex;
            vertices += data->polygons[polygonIndex].npoints;
        }
    }
    if (!count)
        return animation_error(CAD_STATUS_INVALID_ARGUMENT, CAD_TAG_POLYGON,
                               -1, "No eligible faces were selected");
    *outputCount = count;
    *verticesPerFrame = vertices;
    return CadResult_Ok(CAD_FORMAT_X11_STREAM);
}

static int sync_static_from_frame_zero(CadFileData* data) {
    int polygonIndex;
    for (polygonIndex = 0; polygonIndex < CAD_MAX_POLYGONS; ++polygonIndex) {
        const CadPolygon* polygon = &data->polygons[polygonIndex];
        int16_t staticChain[CAD_MAX_FACE_POINTS];
        int16_t animationChain[CAD_MAX_FACE_POINTS];
        int staticCount;
        int animationCount;
        int ordinal;
        if (!polygon->flags || polygon->animation == -1) continue;
        staticCount = get_static_chain(data, polygonIndex, staticChain);
        animationCount = get_animation_chain(data, polygonIndex, 0,
                                             animationChain);
        if (staticCount != polygon->npoints || animationCount != staticCount)
            return 0;
        for (ordinal = 0; ordinal < staticCount; ++ordinal) {
            CadPoint* target = &data->points[staticChain[ordinal]];
            const CadAnimationPoint* source =
                &data->animationPoints[animationChain[ordinal]];
            target->pointx = source->pointx;
            target->pointy = source->pointy;
            target->pointz = source->pointz;
        }
    }
    return 1;
}

CadResult CadAnimation_Create(CadFileData* data,
                              const int16_t* polygonIndices,
                              size_t polygonCount,
                              int frameCount) {
    CadFileData* backup = NULL;
    CadAnimationInfo info;
    CadResult result = begin_mutation(data, &backup, &info);
    int16_t targets[CAD_MAX_ANIMATION_INDICES];
    int targetCount = 0;
    int verticesPerFrame = 0;
    int capacityFrames;
    int targetCursor;
    if (!CadResult_IsSuccess(&result)) return result;
    if (info.animatedFaceCount)
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INVALID_TOPOLOGY,
                            CAD_TAG_ANIMATION_INDEX, -1,
                            "The document already has editable animation; add faces instead"));
    result = collect_target_faces(data, polygonIndices, polygonCount, 1,
                                  targets, &targetCount, &verticesPerFrame);
    if (!CadResult_IsSuccess(&result))
        return abort_mutation(data, backup, result);
    if (targetCount > available_animation_indices(data))
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INDEX_OUT_OF_RANGE,
                            CAD_TAG_ANIMATION_INDEX, -1,
                            "Animation index capacity is exhausted"));
    capacityFrames = available_animation_points(data) / verticesPerFrame;
    if (capacityFrames > CAD_ANIMATION_FRAMES)
        capacityFrames = CAD_ANIMATION_FRAMES;
    if (frameCount == 0) {
        frameCount = capacityFrames < CAD_ANIMATION_DEFAULT_FRAMES
                         ? capacityFrames : CAD_ANIMATION_DEFAULT_FRAMES;
    }
    if (frameCount < 1 || frameCount > CAD_ANIMATION_FRAMES ||
        frameCount > capacityFrames)
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INDEX_OUT_OF_RANGE,
                            CAD_TAG_ANIMATION_POINT, -1,
                            "Requested %d frames; this selection supports at most %d",
                            frameCount, capacityFrames));
    for (targetCursor = 0; targetCursor < targetCount; ++targetCursor) {
        int polygonIndex = targets[targetCursor];
        CadPolygon* polygon = &data->polygons[polygonIndex];
        CadPosition positions[CAD_MAX_FACE_POINTS];
        int animationIndex = find_free_animation_index(data);
        int frame;
        if (animationIndex < 0 ||
            static_positions(data, polygonIndex, positions) != polygon->npoints)
            return abort_mutation(data, backup,
                animation_error(CAD_STATUS_INVALID_TOPOLOGY, CAD_TAG_POLYGON,
                                polygonIndex,
                                "Face %d has an invalid static point chain",
                                polygonIndex));
        initialize_animation_index(&data->animationIndices[animationIndex]);
        polygon->animation = (int16_t)animationIndex;
        for (frame = 0; frame < frameCount; ++frame) {
            int16_t firstPoint;
            if (!allocate_chain(data, positions, polygon->npoints,
                                &firstPoint))
                return abort_mutation(data, backup,
                    animation_error(CAD_STATUS_INDEX_OUT_OF_RANGE,
                                    CAD_TAG_ANIMATION_POINT, -1,
                                    "Animation point capacity was exhausted"));
            data->animationIndices[animationIndex].frame[frame] = firstPoint;
        }
    }
    if (!sync_static_from_frame_zero(data))
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INVALID_TOPOLOGY,
                            CAD_TAG_ANIMATION_POINT, -1,
                            "Could not synchronize frame 0 with static geometry"));
    return finish_mutation(data, backup);
}

CadResult CadAnimation_AddFaces(CadFileData* data,
                                const int16_t* polygonIndices,
                                size_t polygonCount) {
    CadFileData* backup = NULL;
    CadAnimationInfo info;
    CadResult result = begin_mutation(data, &backup, &info);
    int16_t targets[CAD_MAX_ANIMATION_INDICES];
    int targetCount = 0;
    int verticesPerFrame = 0;
    int targetCursor;
    if (!CadResult_IsSuccess(&result)) return result;
    if (!info.editable)
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INVALID_TOPOLOGY,
                            CAD_TAG_ANIMATION_INDEX, -1,
                            "Create an animation before adding static faces"));
    result = collect_target_faces(data, polygonIndices, polygonCount, 1,
                                  targets, &targetCount, &verticesPerFrame);
    if (!CadResult_IsSuccess(&result))
        return abort_mutation(data, backup, result);
    if (targetCount > available_animation_indices(data))
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INDEX_OUT_OF_RANGE,
                            CAD_TAG_ANIMATION_INDEX, -1,
                            "Animation index capacity is exhausted"));
    if ((size_t)verticesPerFrame * (size_t)info.frameCount >
        (size_t)available_animation_points(data))
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INDEX_OUT_OF_RANGE,
                            CAD_TAG_ANIMATION_POINT, -1,
                            "Not enough animation points for %d face(s) across %d frames",
                            targetCount, info.frameCount));
    for (targetCursor = 0; targetCursor < targetCount; ++targetCursor) {
        int polygonIndex = targets[targetCursor];
        CadPolygon* polygon = &data->polygons[polygonIndex];
        CadPosition positions[CAD_MAX_FACE_POINTS];
        int animationIndex = find_free_animation_index(data);
        int frame;
        if (animationIndex < 0 ||
            static_positions(data, polygonIndex, positions) != polygon->npoints)
            return abort_mutation(data, backup,
                animation_error(CAD_STATUS_INVALID_TOPOLOGY, CAD_TAG_POLYGON,
                                polygonIndex,
                                "Face %d has an invalid static point chain",
                                polygonIndex));
        initialize_animation_index(&data->animationIndices[animationIndex]);
        polygon->animation = (int16_t)animationIndex;
        for (frame = 0; frame < info.frameCount; ++frame) {
            int16_t firstPoint;
            if (!allocate_chain(data, positions, polygon->npoints,
                                &firstPoint))
                return abort_mutation(data, backup,
                    animation_error(CAD_STATUS_INDEX_OUT_OF_RANGE,
                                    CAD_TAG_ANIMATION_POINT, -1,
                                    "Animation point capacity was exhausted"));
            data->animationIndices[animationIndex].frame[frame] = firstPoint;
        }
    }
    if (!sync_static_from_frame_zero(data))
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INVALID_TOPOLOGY,
                            CAD_TAG_ANIMATION_POINT, -1,
                            "Could not synchronize frame 0 with static geometry"));
    return finish_mutation(data, backup);
}

CadResult CadAnimation_SetFrameCount(CadFileData* data, int frameCount) {
    CadFileData* backup = NULL;
    CadAnimationInfo info;
    CadResult result = begin_mutation(data, &backup, &info);
    int polygonIndex;
    if (!CadResult_IsSuccess(&result)) return result;
    if (!info.editable)
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INVALID_TOPOLOGY,
                            CAD_TAG_ANIMATION_INDEX, -1,
                            "The document has no editable animation"));
    if (frameCount < 1 || frameCount > CAD_ANIMATION_FRAMES ||
        frameCount > info.maximumFrameCount)
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INDEX_OUT_OF_RANGE,
                            CAD_TAG_ANIMATION_INDEX, -1,
                            "Requested %d frames; supported range is 1..%d",
                            frameCount, info.maximumFrameCount));
    if (frameCount == info.frameCount)
        return finish_mutation(data, backup);
    for (polygonIndex = 0; polygonIndex < CAD_MAX_POLYGONS; ++polygonIndex) {
        CadPolygon* polygon = &data->polygons[polygonIndex];
        CadAnimationIndex* animation;
        int frame;
        if (!polygon->flags || polygon->animation == -1) continue;
        animation = &data->animationIndices[polygon->animation];
        if (frameCount < info.frameCount) {
            for (frame = frameCount; frame < info.frameCount; ++frame) {
                free_chain(data, animation->frame[frame], polygon->npoints);
                animation->frame[frame] = -1;
            }
        } else {
            CadPosition positions[CAD_MAX_FACE_POINTS];
            if (animation_positions(data, polygonIndex, info.frameCount - 1,
                                    positions) != polygon->npoints)
                return abort_mutation(data, backup,
                    animation_error(CAD_STATUS_INVALID_TOPOLOGY,
                                    CAD_TAG_ANIMATION_INDEX,
                                    polygon->animation,
                                    "Face %d has an invalid last frame",
                                    polygonIndex));
            for (frame = info.frameCount; frame < frameCount; ++frame) {
                int16_t firstPoint;
                if (!allocate_chain(data, positions, polygon->npoints,
                                    &firstPoint))
                    return abort_mutation(data, backup,
                        animation_error(CAD_STATUS_INDEX_OUT_OF_RANGE,
                                        CAD_TAG_ANIMATION_POINT, -1,
                                        "Animation point capacity was exhausted"));
                animation->frame[frame] = firstPoint;
            }
        }
    }
    if (!sync_static_from_frame_zero(data))
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INVALID_TOPOLOGY,
                            CAD_TAG_ANIMATION_POINT, -1,
                            "Could not synchronize frame 0 with static geometry"));
    return finish_mutation(data, backup);
}

CadResult CadAnimation_InsertFrame(CadFileData* data, int insertAt,
                                   int sourceFrame) {
    CadFileData* backup = NULL;
    CadAnimationInfo info;
    CadResult result = begin_mutation(data, &backup, &info);
    int polygonIndex;
    if (!CadResult_IsSuccess(&result)) return result;
    if (!info.editable)
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INVALID_TOPOLOGY,
                            CAD_TAG_ANIMATION_INDEX, -1,
                            "The document has no editable animation"));
    if (info.frameCount >= CAD_ANIMATION_FRAMES ||
        info.frameCount >= info.maximumFrameCount)
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INDEX_OUT_OF_RANGE,
                            CAD_TAG_ANIMATION_INDEX, -1,
                            "The animation cannot accept another frame"));
    if (insertAt < 0 || insertAt > info.frameCount)
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INDEX_OUT_OF_RANGE,
                            CAD_TAG_ANIMATION_INDEX, insertAt,
                            "Insert position %d is outside 0..%d", insertAt,
                            info.frameCount));
    if (sourceFrame < 0)
        sourceFrame = insertAt == 0 ? 0 : insertAt - 1;
    if (sourceFrame < 0 || sourceFrame >= info.frameCount)
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INDEX_OUT_OF_RANGE,
                            CAD_TAG_ANIMATION_INDEX, sourceFrame,
                            "Source frame %d is outside 0..%d", sourceFrame,
                            info.frameCount - 1));
    for (polygonIndex = 0; polygonIndex < CAD_MAX_POLYGONS; ++polygonIndex) {
        CadPolygon* polygon = &data->polygons[polygonIndex];
        CadAnimationIndex* animation;
        CadPosition positions[CAD_MAX_FACE_POINTS];
        int16_t newFirstPoint;
        int frame;
        if (!polygon->flags || polygon->animation == -1) continue;
        animation = &data->animationIndices[polygon->animation];
        if (animation_positions(data, polygonIndex, sourceFrame, positions) !=
            polygon->npoints)
            return abort_mutation(data, backup,
                animation_error(CAD_STATUS_INVALID_TOPOLOGY,
                                CAD_TAG_ANIMATION_INDEX, polygon->animation,
                                "Face %d source frame is invalid",
                                polygonIndex));
        if (!allocate_chain(data, positions, polygon->npoints,
                            &newFirstPoint))
            return abort_mutation(data, backup,
                animation_error(CAD_STATUS_INDEX_OUT_OF_RANGE,
                                CAD_TAG_ANIMATION_POINT, -1,
                                "Animation point capacity was exhausted"));
        for (frame = info.frameCount; frame > insertAt; --frame)
            animation->frame[frame] = animation->frame[frame - 1];
        animation->frame[insertAt] = newFirstPoint;
    }
    if (!sync_static_from_frame_zero(data))
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INVALID_TOPOLOGY,
                            CAD_TAG_ANIMATION_POINT, -1,
                            "Could not synchronize frame 0 with static geometry"));
    return finish_mutation(data, backup);
}

CadResult CadAnimation_DuplicateFrame(CadFileData* data, int sourceFrame,
                                      int insertAt) {
    return CadAnimation_InsertFrame(data, insertAt, sourceFrame);
}

CadResult CadAnimation_DeleteFrame(CadFileData* data, int frameIndex) {
    CadFileData* backup = NULL;
    CadAnimationInfo info;
    CadResult result = begin_mutation(data, &backup, &info);
    int polygonIndex;
    if (!CadResult_IsSuccess(&result)) return result;
    if (!info.editable)
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INVALID_TOPOLOGY,
                            CAD_TAG_ANIMATION_INDEX, -1,
                            "The document has no editable animation"));
    if (info.frameCount <= 1)
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INDEX_OUT_OF_RANGE,
                            CAD_TAG_ANIMATION_INDEX, frameIndex,
                            "An animation must retain at least one frame"));
    if (frameIndex < 0 || frameIndex >= info.frameCount)
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INDEX_OUT_OF_RANGE,
                            CAD_TAG_ANIMATION_INDEX, frameIndex,
                            "Frame %d is outside 0..%d", frameIndex,
                            info.frameCount - 1));
    for (polygonIndex = 0; polygonIndex < CAD_MAX_POLYGONS; ++polygonIndex) {
        CadPolygon* polygon = &data->polygons[polygonIndex];
        CadAnimationIndex* animation;
        int16_t deleted;
        int frame;
        if (!polygon->flags || polygon->animation == -1) continue;
        animation = &data->animationIndices[polygon->animation];
        deleted = animation->frame[frameIndex];
        for (frame = frameIndex; frame + 1 < info.frameCount; ++frame)
            animation->frame[frame] = animation->frame[frame + 1];
        animation->frame[info.frameCount - 1] = -1;
        free_chain(data, deleted, polygon->npoints);
    }
    if (!sync_static_from_frame_zero(data))
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INVALID_TOPOLOGY,
                            CAD_TAG_ANIMATION_POINT, -1,
                            "Could not synchronize frame 0 with static geometry"));
    return finish_mutation(data, backup);
}

static int build_point_mask(const CadFileData* data,
                            const int16_t* pointIndices, size_t pointCount,
                            uint8_t mask[CAD_MAX_POINTS], int* selectedCount) {
    size_t cursor;
    int index;
    memset(mask, 0, CAD_MAX_POINTS);
    *selectedCount = 0;
    if (!pointIndices || pointCount == 0) {
        for (index = 0; index < CAD_MAX_POINTS; ++index) {
            if (!data->points[index].flags) continue;
            mask[index] = 1;
            ++*selectedCount;
        }
        return 1;
    }
    if (pointCount > CAD_MAX_POINTS) return 0;
    for (cursor = 0; cursor < pointCount; ++cursor) {
        index = pointIndices[cursor];
        if (!active_point(data, index)) return 0;
        if (!mask[index]) {
            mask[index] = 1;
            ++*selectedCount;
        }
    }
    return 1;
}

static int map_base_point(const CadFileData* data, int16_t basePoint,
                          int frame, int* ownerPolygon,
                          int16_t* animationPoint) {
    int polygonIndex;
    for (polygonIndex = 0; polygonIndex < CAD_MAX_POLYGONS; ++polygonIndex) {
        const CadPolygon* polygon = &data->polygons[polygonIndex];
        int16_t staticChain[CAD_MAX_FACE_POINTS];
        int staticCount;
        int ordinal;
        if (!polygon->flags) continue;
        staticCount = get_static_chain(data, polygonIndex, staticChain);
        for (ordinal = 0; ordinal < staticCount; ++ordinal) {
            if (staticChain[ordinal] != basePoint) continue;
            if (ownerPolygon) *ownerPolygon = polygonIndex;
            if (animationPoint) *animationPoint = -1;
            if (polygon->animation != -1 && animationPoint) {
                int16_t animated[CAD_MAX_FACE_POINTS];
                if (get_animation_chain(data, polygonIndex, frame,
                                        animated) != staticCount)
                    return -1;
                *animationPoint = animated[ordinal];
            }
            return 1;
        }
    }
    if (ownerPolygon) *ownerPolygon = -1;
    if (animationPoint) *animationPoint = -1;
    return 0;
}

CadResult CadAnimation_CopyFrame(CadFileData* data, int sourceFrame,
                                 int targetFrame,
                                 const int16_t* basePointIndices,
                                 size_t basePointCount) {
    CadFileData* backup = NULL;
    CadAnimationInfo info;
    CadResult result = begin_mutation(data, &backup, &info);
    uint8_t selected[CAD_MAX_POINTS];
    int selectedCount;
    int polygonIndex;
    int copied = 0;
    if (!CadResult_IsSuccess(&result)) return result;
    if (!info.editable)
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INVALID_TOPOLOGY,
                            CAD_TAG_ANIMATION_INDEX, -1,
                            "The document has no editable animation"));
    if (sourceFrame < 0 || sourceFrame >= info.frameCount ||
        targetFrame < 0 || targetFrame >= info.frameCount)
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INDEX_OUT_OF_RANGE,
                            CAD_TAG_ANIMATION_INDEX, -1,
                            "Copy frames must be inside 0..%d",
                            info.frameCount - 1));
    if (!build_point_mask(data, basePointIndices, basePointCount, selected,
                          &selectedCount) || !selectedCount)
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INVALID_ARGUMENT, CAD_TAG_POINT, -1,
                            "The point selection is invalid or empty"));
    for (polygonIndex = 0; polygonIndex < CAD_MAX_POLYGONS; ++polygonIndex) {
        const CadPolygon* polygon = &data->polygons[polygonIndex];
        int16_t staticChain[CAD_MAX_FACE_POINTS];
        int16_t sourceChain[CAD_MAX_FACE_POINTS];
        int16_t targetChain[CAD_MAX_FACE_POINTS];
        int count;
        int ordinal;
        if (!polygon->flags || polygon->animation == -1) continue;
        count = get_static_chain(data, polygonIndex, staticChain);
        if (get_animation_chain(data, polygonIndex, sourceFrame, sourceChain) !=
                count ||
            get_animation_chain(data, polygonIndex, targetFrame, targetChain) !=
                count)
            return abort_mutation(data, backup,
                animation_error(CAD_STATUS_INVALID_TOPOLOGY,
                                CAD_TAG_ANIMATION_INDEX, polygon->animation,
                                "Face %d has a broken frame chain",
                                polygonIndex));
        for (ordinal = 0; ordinal < count; ++ordinal) {
            CadAnimationPoint* target;
            const CadAnimationPoint* source;
            if (!selected[staticChain[ordinal]]) continue;
            source = &data->animationPoints[sourceChain[ordinal]];
            target = &data->animationPoints[targetChain[ordinal]];
            target->pointx = source->pointx;
            target->pointy = source->pointy;
            target->pointz = source->pointz;
            ++copied;
        }
    }
    if (!copied)
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INVALID_ARGUMENT, CAD_TAG_POINT, -1,
                            "No selected point belongs to an animated face"));
    if (!sync_static_from_frame_zero(data))
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INVALID_TOPOLOGY,
                            CAD_TAG_ANIMATION_POINT, -1,
                            "Could not synchronize frame 0 with static geometry"));
    return finish_mutation(data, backup);
}

static int finite_position(CadPosition position) {
    return isfinite(position.x) && isfinite(position.y) &&
           isfinite(position.z);
}

CadResult CadAnimation_SetPoint(CadFileData* data, int frameIndex,
                                int16_t basePointIndex,
                                CadPosition position) {
    CadFileData* backup = NULL;
    CadAnimationInfo info;
    CadResult result = begin_mutation(data, &backup, &info);
    int owner = -1;
    int16_t animatedPoint = -1;
    int mapping;
    if (!CadResult_IsSuccess(&result)) return result;
    if (!active_point(data, basePointIndex) || !finite_position(position))
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INVALID_ARGUMENT, CAD_TAG_POINT,
                            basePointIndex,
                            "Point %d or its new coordinate is invalid",
                            basePointIndex));
    if (info.editable && (frameIndex < 0 || frameIndex >= info.frameCount))
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INDEX_OUT_OF_RANGE,
                            CAD_TAG_ANIMATION_INDEX, frameIndex,
                            "Frame %d is outside 0..%d", frameIndex,
                            info.frameCount - 1));
    if (!info.editable) frameIndex = 0;
    mapping = map_base_point(data, basePointIndex, frameIndex, &owner,
                             &animatedPoint);
    if (mapping < 0)
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INVALID_TOPOLOGY,
                            CAD_TAG_ANIMATION_POINT, -1,
                            "Point %d has a broken animation mapping",
                            basePointIndex));
    if (owner >= 0 && data->polygons[owner].animation != -1) {
        CadAnimationPoint* target;
        if (!active_animation_point(data, animatedPoint))
            return abort_mutation(data, backup,
                animation_error(CAD_STATUS_INVALID_TOPOLOGY,
                                CAD_TAG_ANIMATION_POINT, animatedPoint,
                                "Point %d has no corresponding animation point",
                                basePointIndex));
        target = &data->animationPoints[animatedPoint];
        target->pointx = position.x;
        target->pointy = position.y;
        target->pointz = position.z;
    } else {
        CadPoint* target = &data->points[basePointIndex];
        target->pointx = position.x;
        target->pointy = position.y;
        target->pointz = position.z;
    }
    if (info.editable && !sync_static_from_frame_zero(data))
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INVALID_TOPOLOGY,
                            CAD_TAG_ANIMATION_POINT, -1,
                            "Could not synchronize frame 0 with static geometry"));
    return finish_mutation(data, backup);
}

static int finite_transform(const CadAffineTransform* transform) {
    int row;
    int column;
    if (!transform || !finite_position(transform->pivot) ||
        !finite_position(transform->translation)) return 0;
    for (row = 0; row < 3; ++row)
        for (column = 0; column < 3; ++column)
            if (!isfinite(transform->matrix[row][column])) return 0;
    return 1;
}

static CadPosition apply_transform(CadPosition point,
                                   const CadAffineTransform* transform) {
    double x = point.x - transform->pivot.x;
    double y = point.y - transform->pivot.y;
    double z = point.z - transform->pivot.z;
    CadPosition result;
    result.x = transform->pivot.x + transform->translation.x +
        transform->matrix[0][0] * x + transform->matrix[0][1] * y +
        transform->matrix[0][2] * z;
    result.y = transform->pivot.y + transform->translation.y +
        transform->matrix[1][0] * x + transform->matrix[1][1] * y +
        transform->matrix[1][2] * z;
    result.z = transform->pivot.z + transform->translation.z +
        transform->matrix[2][0] * x + transform->matrix[2][1] * y +
        transform->matrix[2][2] * z;
    return result;
}

CadResult CadAnimation_Transform(CadFileData* data, int currentFrame,
                                 CadAnimationScope scope,
                                 const int16_t* basePointIndices,
                                 size_t basePointCount,
                                 const CadAffineTransform* transform) {
    CadFileData* backup = NULL;
    CadAnimationInfo info;
    CadResult result = begin_mutation(data, &backup, &info);
    uint8_t selected[CAD_MAX_POINTS];
    int selectedCount;
    int pointIndex;
    int changed = 0;
    if (!CadResult_IsSuccess(&result)) return result;
    if (!finite_transform(transform) ||
        (scope != CAD_ANIMATION_CURRENT_FRAME &&
         scope != CAD_ANIMATION_ALL_FRAMES))
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INVALID_ARGUMENT, CAD_TAG_POINT, -1,
                            "The affine transform or animation scope is invalid"));
    if (info.editable &&
        (currentFrame < 0 || currentFrame >= info.frameCount))
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INDEX_OUT_OF_RANGE,
                            CAD_TAG_ANIMATION_INDEX, currentFrame,
                            "Frame %d is outside 0..%d", currentFrame,
                            info.frameCount - 1));
    if (!build_point_mask(data, basePointIndices, basePointCount, selected,
                          &selectedCount) || !selectedCount)
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INVALID_ARGUMENT, CAD_TAG_POINT, -1,
                            "The point selection is invalid or empty"));
    for (pointIndex = 0; pointIndex < CAD_MAX_POINTS; ++pointIndex) {
        int owner = -1;
        int16_t animationPoint = -1;
        int mapping;
        if (!selected[pointIndex]) continue;
        mapping = map_base_point(data, (int16_t)pointIndex,
                                 info.editable ? currentFrame : 0,
                                 &owner, &animationPoint);
        if (mapping < 0)
            return abort_mutation(data, backup,
                animation_error(CAD_STATUS_INVALID_TOPOLOGY,
                                CAD_TAG_ANIMATION_POINT, -1,
                                "Point %d has a broken animation mapping",
                                pointIndex));
        if (owner >= 0 && data->polygons[owner].animation != -1) {
            int firstFrame = scope == CAD_ANIMATION_ALL_FRAMES ? 0
                                                               : currentFrame;
            int finalFrame = scope == CAD_ANIMATION_ALL_FRAMES
                                 ? info.frameCount : currentFrame + 1;
            int frame;
            for (frame = firstFrame; frame < finalFrame; ++frame) {
                CadAnimationPoint* target;
                CadPosition before;
                mapping = map_base_point(data, (int16_t)pointIndex, frame,
                                         NULL, &animationPoint);
                if (mapping < 0 || !active_animation_point(data,
                                                           animationPoint))
                    return abort_mutation(data, backup,
                        animation_error(CAD_STATUS_INVALID_TOPOLOGY,
                                        CAD_TAG_ANIMATION_POINT,
                                        animationPoint,
                                        "Point %d has no mapping in frame %d",
                                        pointIndex, frame));
                target = &data->animationPoints[animationPoint];
                before.x = target->pointx;
                before.y = target->pointy;
                before.z = target->pointz;
                before = apply_transform(before, transform);
                target->pointx = before.x;
                target->pointy = before.y;
                target->pointz = before.z;
                ++changed;
            }
        } else {
            CadPoint* target = &data->points[pointIndex];
            CadPosition before;
            before.x = target->pointx;
            before.y = target->pointy;
            before.z = target->pointz;
            before = apply_transform(before, transform);
            target->pointx = before.x;
            target->pointy = before.y;
            target->pointz = before.z;
            ++changed;
        }
    }
    if (!changed)
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INVALID_ARGUMENT, CAD_TAG_POINT, -1,
                            "No selected coordinate could be transformed"));
    if (info.editable && !sync_static_from_frame_zero(data))
        return abort_mutation(data, backup,
            animation_error(CAD_STATUS_INVALID_TOPOLOGY,
                            CAD_TAG_ANIMATION_POINT, -1,
                            "Could not synchronize frame 0 with static geometry"));
    return finish_mutation(data, backup);
}

static void remove_all_animation(CadFileData* data) {
    int polygonIndex;
    int index;
    int frame;
    for (polygonIndex = 0; polygonIndex < CAD_MAX_POLYGONS; ++polygonIndex)
        if (data->polygons[polygonIndex].flags)
            data->polygons[polygonIndex].animation = -1;
    memset(data->animationIndices, 0, sizeof(data->animationIndices));
    for (index = 0; index < CAD_MAX_ANIMATION_INDICES; ++index)
        for (frame = 0; frame < CAD_ANIMATION_FRAMES; ++frame)
            data->animationIndices[index].frame[frame] = -1;
    memset(data->animationPoints, 0, sizeof(data->animationPoints));
    for (index = 0; index < CAD_MAX_ANIMATION_POINTS; ++index)
        data->animationPoints[index].nextPoint = -1;
    data->animationIndexCount = 0;
    data->animationPointCount = 0;
}

CadResult CadAnimation_MakeStaticCopy(const CadFileData* source,
                                      const CadPose* displayedPose,
                                      CadFileData* output) {
    CadFileData* candidate;
    CadResult result;
    int pointIndex;
    if (!source || !displayedPose || !output)
        return animation_error(CAD_STATUS_INVALID_ARGUMENT, -1, -1,
                               "Static-pose conversion received a NULL argument");
    result = CadCodec_Validate(source);
    if (!CadResult_IsSuccess(&result)) return result;
    candidate = (CadFileData*)malloc(sizeof(*candidate));
    if (!candidate)
        return animation_error(CAD_STATUS_OUT_OF_MEMORY, -1, -1,
                               "Not enough memory to create a static copy");
    *candidate = *source;
    for (pointIndex = 0; pointIndex < CAD_MAX_POINTS; ++pointIndex) {
        CadPoint* point;
        if (!source->points[pointIndex].flags) continue;
        if (!displayedPose->pointValid[pointIndex] ||
            !finite_position(displayedPose->points[pointIndex])) {
            free(candidate);
            return animation_error(CAD_STATUS_INVALID_NUMBER, CAD_TAG_POINT,
                                   pointIndex,
                                   "Displayed pose is missing point %d",
                                   pointIndex);
        }
        point = &candidate->points[pointIndex];
        point->pointx = displayedPose->points[pointIndex].x;
        point->pointy = displayedPose->points[pointIndex].y;
        point->pointz = displayedPose->points[pointIndex].z;
    }
    remove_all_animation(candidate);
    result = CadCodec_Validate(candidate);
    if (!CadResult_IsSuccess(&result)) {
        free(candidate);
        return result;
    }
    *output = *candidate;
    free(candidate);
    result.format = CAD_FORMAT_X11_STREAM;
    return result;
}

CadPoseSample CadPoseSample_FromFrame(double framePosition, int frameCount,
                                      int loop, int interpolation) {
    CadPoseSample sample;
    double lower;
    memset(&sample, 0, sizeof(sample));
    if (frameCount < 1) frameCount = 1;
    if (frameCount > CAD_ANIMATION_FRAMES)
        frameCount = CAD_ANIMATION_FRAMES;
    if (!isfinite(framePosition)) framePosition = 0.0;
    if (loop && frameCount > 1) {
        framePosition = fmod(framePosition, (double)frameCount);
        if (framePosition < 0.0) framePosition += frameCount;
    } else {
        if (framePosition < 0.0) framePosition = 0.0;
        if (framePosition > frameCount - 1)
            framePosition = (double)(frameCount - 1);
    }
    lower = floor(framePosition);
    sample.frameA = (int)lower;
    sample.frameB = sample.frameA;
    sample.alpha = 0.0;
    if (interpolation && frameCount > 1) {
        sample.alpha = framePosition - lower;
        if (sample.frameA + 1 < frameCount)
            sample.frameB = sample.frameA + 1;
        else if (loop)
            sample.frameB = 0;
        if (sample.alpha <= 1e-12 || sample.frameA == sample.frameB) {
            sample.alpha = 0.0;
            sample.frameB = sample.frameA;
        }
    }
    sample.interpolated = sample.alpha > 0.0 &&
                          sample.frameA != sample.frameB;
    return sample;
}

static void clear_point_map(
    int16_t map[CAD_ANIMATION_FRAMES][CAD_MAX_POINTS]) {
    int frame;
    int point;
    for (frame = 0; frame < CAD_ANIMATION_FRAMES; ++frame)
        for (point = 0; point < CAD_MAX_POINTS; ++point)
            map[frame][point] = -1;
}

static int build_point_map(
    const CadFileData* data,
    int16_t map[CAD_ANIMATION_FRAMES][CAD_MAX_POINTS]) {
    int polygonIndex;
    clear_point_map(map);
    for (polygonIndex = 0; polygonIndex < CAD_MAX_POLYGONS; ++polygonIndex) {
        const CadPolygon* polygon = &data->polygons[polygonIndex];
        int16_t staticChain[CAD_MAX_FACE_POINTS];
        int count;
        int frames;
        int frame;
        if (!polygon->flags || polygon->animation == -1) continue;
        count = get_static_chain(data, polygonIndex, staticChain);
        if (count != polygon->npoints) return 0;
        frames = count_frames(&data->animationIndices[polygon->animation]);
        for (frame = 0; frame < frames; ++frame) {
            int16_t animationChain[CAD_MAX_FACE_POINTS];
            int ordinal;
            if (get_animation_chain(data, polygonIndex, frame,
                                    animationChain) != count) return 0;
            for (ordinal = 0; ordinal < count; ++ordinal)
                map[frame][staticChain[ordinal]] = animationChain[ordinal];
        }
    }
    return 1;
}

static void derive_pose_faces(const CadFileData* data, CadPose* pose) {
    int polygonIndex;
    memset(pose->faceNormals, 0, sizeof(pose->faceNormals));
    memset(pose->faceNormalValid, 0, sizeof(pose->faceNormalValid));
    memset(pose->faceSide, 0, sizeof(pose->faceSide));
    for (polygonIndex = 0; polygonIndex < CAD_MAX_POLYGONS; ++polygonIndex) {
        const CadPolygon* polygon = &data->polygons[polygonIndex];
        int16_t chain[CAD_MAX_FACE_POINTS];
        double coordinates[CAD_MAX_FACE_POINTS][3];
        double areaNormal[3];
        CadPosition normal = { 0.0, 0.0, 0.0 };
        int count;
        int ordinal;
        double length;
        if (!polygon->flags || polygon->npoints < 3) continue;
        count = get_static_chain(data, polygonIndex, chain);
        if (count != polygon->npoints) continue;
        for (ordinal = 0; ordinal < count; ++ordinal) {
            const CadPosition* current = &pose->points[chain[ordinal]];
            if (!pose->pointValid[chain[ordinal]]) break;
            coordinates[ordinal][0] = current->x;
            coordinates[ordinal][1] = current->y;
            coordinates[ordinal][2] = current->z;
        }
        if (ordinal != count ||
            !CadGeometry_ComputePolygonNormal(coordinates[0], count,
                                              areaNormal))
            continue;
        length = sqrt(areaNormal[0] * areaNormal[0] +
                      areaNormal[1] * areaNormal[1] +
                      areaNormal[2] * areaNormal[2]);
        if (!isfinite(length) || length <= 0.0) continue;
        normal.x = areaNormal[0];
        normal.y = areaNormal[1];
        normal.z = areaNormal[2];
        normal.x /= length;
        normal.y /= length;
        normal.z /= length;
        pose->faceNormals[polygonIndex] = normal;
        pose->faceNormalValid[polygonIndex] = 1;
        pose->faceSide[polygonIndex] =
            (uint8_t)((normal.y < 0.0 ? 1 : 0) |
                      (normal.z >= 0.0 ? 2 : 0) |
                      (normal.x < 0.0 ? 4 : 0));
    }
}

static CadResult evaluate_with_map(
    const CadFileData* data, CadPoseSample sample,
    const int16_t* map,
    uint64_t generation, CadPose* output) {
    int pointIndex;
    if (!data || !map || !output)
        return animation_error(CAD_STATUS_INVALID_ARGUMENT, -1, -1,
                               "Pose evaluation received a NULL argument");
    memset(output, 0, sizeof(*output));
    output->sample = sample;
    output->generation = generation;
    for (pointIndex = 0; pointIndex < CAD_MAX_POINTS; ++pointIndex) {
        const CadPoint* base = &data->points[pointIndex];
        CadPosition position;
        int16_t pointA;
        int16_t pointB;
        if (!base->flags) continue;
        position.x = base->pointx;
        position.y = base->pointy;
        position.z = base->pointz;
        pointA = sample.frameA >= 0 &&
                         sample.frameA < CAD_ANIMATION_FRAMES
                     ? map[sample.frameA * CAD_MAX_POINTS + pointIndex] : -1;
        pointB = sample.frameB >= 0 &&
                         sample.frameB < CAD_ANIMATION_FRAMES
                     ? map[sample.frameB * CAD_MAX_POINTS + pointIndex] : -1;
        if (active_animation_point(data, pointA)) {
            const CadAnimationPoint* a = &data->animationPoints[pointA];
            position.x = a->pointx;
            position.y = a->pointy;
            position.z = a->pointz;
            if (sample.interpolated && active_animation_point(data, pointB)) {
                const CadAnimationPoint* b = &data->animationPoints[pointB];
                position.x += (b->pointx - position.x) * sample.alpha;
                position.y += (b->pointy - position.y) * sample.alpha;
                position.z += (b->pointz - position.z) * sample.alpha;
            }
        }
        if (!finite_position(position))
            return animation_error(CAD_STATUS_INVALID_NUMBER, CAD_TAG_POINT,
                                   pointIndex,
                                   "Pose evaluation produced a non-finite point");
        output->points[pointIndex] = position;
        output->pointValid[pointIndex] = 1;
    }
    derive_pose_faces(data, output);
    return CadResult_Ok(CAD_FORMAT_X11_STREAM);
}

CadResult CadPose_Evaluate(const CadFileData* data, CadPoseSample sample,
                           CadPose* output) {
    CadResult result;
    int16_t (*map)[CAD_MAX_POINTS];
    CadAnimationInfo info;
    if (!data || !output)
        return animation_error(CAD_STATUS_INVALID_ARGUMENT, -1, -1,
                               "Pose evaluation received a NULL argument");
    result = CadAnimation_Inspect(data, &info);
    if (!CadResult_IsSuccess(&result)) return result;
    if (info.frameCount > 0 &&
        (sample.frameA < 0 || sample.frameA >= info.frameCount ||
         sample.frameB < 0 || sample.frameB >= info.frameCount ||
         !isfinite(sample.alpha) || sample.alpha < 0.0 ||
         sample.alpha > 1.0))
        return animation_error(CAD_STATUS_INDEX_OUT_OF_RANGE,
                               CAD_TAG_ANIMATION_INDEX, -1,
                               "Pose sample is outside the animation range");
    map = (int16_t (*)[CAD_MAX_POINTS])malloc(
        sizeof(int16_t) * CAD_ANIMATION_FRAMES * CAD_MAX_POINTS);
    if (!map)
        return animation_error(CAD_STATUS_OUT_OF_MEMORY, -1, -1,
                               "Not enough memory to evaluate the pose");
    if (!build_point_map(data, map)) {
        free(map);
        return animation_error(CAD_STATUS_INVALID_TOPOLOGY,
                               CAD_TAG_ANIMATION_POINT, -1,
                               "Could not build the pose point mapping");
    }
    result = evaluate_with_map(data, sample, map[0], 1, output);
    free(map);
    return result;
}

CadResult CadScene_Build(const CadFileData* data, CadPose* poseStorage,
                         CadPoseSample sample, CadScene* output) {
    CadResult result;
    if (!output)
        return animation_error(CAD_STATUS_INVALID_ARGUMENT, -1, -1,
                               "Scene output is NULL");
    result = CadPose_Evaluate(data, sample, poseStorage);
    if (!CadResult_IsSuccess(&result)) return result;
    output->topology = data;
    output->pose = poseStorage;
    output->generation = poseStorage->generation;
    return result;
}

int CadScene_GetPoint(const CadScene* scene, int16_t pointIndex,
                      CadPosition* output) {
    if (!scene || !scene->topology || !scene->pose || !output ||
        pointIndex < 0 || pointIndex >= CAD_MAX_POINTS ||
        !scene->pose->pointValid[pointIndex]) return 0;
    *output = scene->pose->points[pointIndex];
    return 1;
}

void CadAnimationSession_Init(CadAnimationSession* session) {
    if (!session) return;
    memset(session, 0, sizeof(*session));
    session->fps = 12.0;
    session->frameCount = 1;
    session->interpolation = 1;
    clear_point_map(session->animationPointForBasePoint);
}

CadResult CadAnimationSession_Rebuild(CadAnimationSession* session,
                                      const CadFileData* data) {
    CadAnimationInfo info;
    CadResult result;
    if (!session || !data)
        return animation_error(CAD_STATUS_INVALID_ARGUMENT, -1, -1,
                               "Animation session rebuild received a NULL argument");
    result = CadAnimation_Inspect(data, &info);
    if (!CadResult_IsSuccess(&result)) {
        session->cacheValid = 0;
        return result;
    }
    if (!build_point_map(data, session->animationPointForBasePoint)) {
        session->cacheValid = 0;
        return animation_error(CAD_STATUS_INVALID_TOPOLOGY,
                               CAD_TAG_ANIMATION_POINT, -1,
                               "Could not build the animation point cache");
    }
    session->frameCount = info.frameCount > 0 ? info.frameCount : 1;
    if (session->currentFrame >= session->frameCount)
        session->currentFrame = session->frameCount - 1;
    if (session->currentFrame < 0) session->currentFrame = 0;
    if (!isfinite(session->previewFrame) || session->previewFrame < 0.0)
        session->previewFrame = (double)session->currentFrame;
    if (session->previewFrame > session->frameCount - 1)
        session->previewFrame = (double)(session->frameCount - 1);
    session->playing = 0;
    session->cacheValid = 1;
    ++session->cacheGeneration;
    return result;
}

static int clamp_frame(const CadAnimationSession* session, int frame) {
    int count = session && session->frameCount > 0 ? session->frameCount : 1;
    if (frame < 0) frame = 0;
    if (frame >= count) frame = count - 1;
    return frame;
}

void CadAnimationSession_SetFrame(CadAnimationSession* session,
                                  int frameIndex) {
    if (!session) return;
    session->playing = 0;
    session->currentFrame = clamp_frame(session, frameIndex);
    session->previewFrame = (double)session->currentFrame;
}

void CadAnimationSession_Seek(CadAnimationSession* session,
                              double framePosition) {
    if (!session || !isfinite(framePosition)) return;
    session->playing = 0;
    if (session->loop && session->frameCount > 1) {
        framePosition = fmod(framePosition, (double)session->frameCount);
        if (framePosition < 0.0) framePosition += session->frameCount;
    } else {
        if (framePosition < 0.0) framePosition = 0.0;
        if (framePosition > session->frameCount - 1)
            framePosition = (double)(session->frameCount - 1);
    }
    session->previewFrame = framePosition;
}

void CadAnimationSession_EndScrub(CadAnimationSession* session) {
    int nearest;
    if (!session) return;
    nearest = (int)floor(session->previewFrame + 0.5);
    if (session->loop && session->frameCount > 1) {
        nearest %= session->frameCount;
        if (nearest < 0) nearest += session->frameCount;
    } else {
        nearest = clamp_frame(session, nearest);
    }
    session->currentFrame = nearest;
    session->previewFrame = (double)nearest;
}

void CadAnimationSession_Play(CadAnimationSession* session,
                              double nowSeconds) {
    if (!session || !isfinite(nowSeconds) || session->playing) return;
    session->currentFrame = clamp_frame(session, session->currentFrame);
    session->previewFrame = (double)session->currentFrame;
    session->playbackStartFrame = session->currentFrame;
    session->lastClockSeconds = nowSeconds;
    session->playing = session->frameCount > 1;
}

static void advance_session(CadAnimationSession* session, double nowSeconds) {
    double elapsed;
    double fps;
    if (!session || !session->playing || !isfinite(nowSeconds)) return;
    elapsed = nowSeconds - session->lastClockSeconds;
    if (!isfinite(elapsed) || elapsed < 0.0) elapsed = 0.0;
    session->lastClockSeconds = nowSeconds;
    fps = isfinite(session->fps) && session->fps > 0.0 ? session->fps : 12.0;
    session->previewFrame += elapsed * fps;
    if (session->loop && session->frameCount > 1) {
        session->previewFrame =
            fmod(session->previewFrame, (double)session->frameCount);
        if (session->previewFrame < 0.0)
            session->previewFrame += session->frameCount;
        session->currentFrame = (int)floor(session->previewFrame);
    } else if (session->previewFrame >= session->frameCount - 1) {
        session->previewFrame = (double)(session->frameCount - 1);
        session->currentFrame = session->frameCount - 1;
        session->playing = 0;
    } else {
        session->currentFrame = (int)floor(session->previewFrame);
    }
}

void CadAnimationSession_Pause(CadAnimationSession* session,
                               double nowSeconds) {
    if (!session) return;
    advance_session(session, nowSeconds);
    session->playing = 0;
    CadAnimationSession_EndScrub(session);
}

void CadAnimationSession_Stop(CadAnimationSession* session) {
    if (!session) return;
    session->playing = 0;
    session->currentFrame = clamp_frame(session, session->playbackStartFrame);
    session->previewFrame = (double)session->currentFrame;
}

void CadAnimationSession_BeginEdit(CadAnimationSession* session,
                                   double nowSeconds) {
    if (!session) return;
    if (session->playing)
        CadAnimationSession_Pause(session, nowSeconds);
    else
        CadAnimationSession_EndScrub(session);
}

CadResult CadAnimationSession_Evaluate(CadAnimationSession* session,
                                       const CadFileData* data,
                                       double nowSeconds,
                                       CadScene* output) {
    CadPoseSample sample;
    CadResult result;
    if (!session || !data || !output)
        return animation_error(CAD_STATUS_INVALID_ARGUMENT, -1, -1,
                               "Animation session evaluation received a NULL argument");
    if (!session->cacheValid) {
        result = CadAnimationSession_Rebuild(session, data);
        if (!CadResult_IsSuccess(&result)) return result;
    }
    advance_session(session, nowSeconds);
    sample = CadPoseSample_FromFrame(session->previewFrame,
                                     session->frameCount, session->loop,
                                     session->interpolation);
    ++session->poseGeneration;
    result = evaluate_with_map(data, sample,
                               session->animationPointForBasePoint[0],
                               session->poseGeneration, &session->pose);
    if (!CadResult_IsSuccess(&result)) return result;
    output->topology = data;
    output->pose = &session->pose;
    output->generation = session->poseGeneration;
    return result;
}
