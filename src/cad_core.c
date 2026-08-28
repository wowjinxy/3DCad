#define _CRT_SECURE_NO_WARNINGS

#include "cad_core.h"
#include "cad_codec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define INVALID_INDEX -1

/* ----------------------------------------------------------------------------
   Initialization and cleanup
   ---------------------------------------------------------------------------- */

void CadCore_Init(CadCore* core) {
    if (!core) return;
    
    memset(core, 0, sizeof(CadCore));
    CadFile_Init(&core->data);
    
    core->editMode = CAD_MODE_SELECT_POINT;
    core->selectModeFlag = 1;  /* Default to point selection */
    core->newPoint = INVALID_INDEX;
    core->newPolygon = INVALID_INDEX;
    core->rootPolygon = INVALID_INDEX;
    core->creatingPoint = INVALID_INDEX;
    core->firstPoint = INVALID_INDEX;
    
    /* Initialize selection arrays to invalid */
    for (int i = 0; i < CAD_MAX_POINTS; i++) {
        core->selection.selectedPoints[i] = INVALID_INDEX;
    }
    for (int i = 0; i < CAD_MAX_POLYGONS; i++) {
        core->selection.selectedPolygons[i] = INVALID_INDEX;
    }
}

void CadCore_Destroy(CadCore* core) {
    if (!core) return;
    CadCore_Clear(core);
}

void CadCore_Clear(CadCore* core) {
    if (!core) return;
    CadFile_Clear(&core->data);
    CadCore_ClearSelection(core);
    core->isDirty = 0;
    core->newPoint = INVALID_INDEX;
    core->newPolygon = INVALID_INDEX;
    core->rootPolygon = INVALID_INDEX;
    core->creatingPoint = INVALID_INDEX;
    core->firstPoint = INVALID_INDEX;
}

/* ----------------------------------------------------------------------------
   File operations
   ---------------------------------------------------------------------------- */

int CadCore_LoadFile(CadCore* core, const char* filename) {
    CadFileData* loaded;
    if (!core || !filename) return 0;
    loaded = (CadFileData*)malloc(sizeof(*loaded));
    if (!loaded) return 0;
    if (!CadFile_Load(filename, loaded)) {
        free(loaded);
        return 0;
    }
    CadCore_Clear(core);
    core->data = *loaded;
    free(loaded);
    CadCore_RebuildDerivedState(core);
    core->isDirty = 0;
    return 1;
}

int CadCore_SaveFile(CadCore* core, const char* filename) {
    if (!core || !filename) return 0;
    
    if (!CadFile_Save(filename, &core->data)) {
        return 0;
    }
    
    core->isDirty = 0;
    return 1;
}

/* ----------------------------------------------------------------------------
   Point operations
   ---------------------------------------------------------------------------- */

int16_t CadCore_AddPoint(CadCore* core, double x, double y, double z) {
    if (!core) return INVALID_INDEX;
    
    /* Find first free slot */
    for (int16_t i = 0; i < CAD_MAX_POINTS; i++) {
        if (core->data.points[i].flags == 0) {
            CadPoint* pt = &core->data.points[i];
            /* The original editor used flag 2 for committed geometry points. */
            pt->flags = 2;
            pt->selectFlag = 0;
            pt->nextPoint = INVALID_INDEX;
            pt->pointx = x;
            pt->pointy = y;
            pt->pointz = z;
            
            if (i >= core->data.pointCount) {
                core->data.pointCount = i + 1;
            }
            
            core->newPoint = i;
            core->isDirty = 1;
            return i;
        }
    }
    
    return INVALID_INDEX; /* No free slots */
}

int CadCore_DeletePoint(CadCore* core, int16_t pointIndex) {
    int polygonIndex;
    if (!core || !CadCore_IsPointValid(core, pointIndex)) return 0;
    CadCore_DeselectPoint(core, pointIndex);
    for (polygonIndex = 0; polygonIndex < CAD_MAX_POLYGONS; ++polygonIndex) {
        CadPolygon* polygon = &core->data.polygons[polygonIndex];
        int16_t current;
        int16_t previous = INVALID_INDEX;
        int steps = 0;
        if (!polygon->flags) continue;
        current = polygon->firstPoint;
        while (current != INVALID_INDEX && steps < polygon->npoints) {
            int16_t next = core->data.points[current].nextPoint;
            if (current == pointIndex) {
                if (previous == INVALID_INDEX) polygon->firstPoint = next;
                else core->data.points[previous].nextPoint = next;
                if (polygon->npoints) polygon->npoints--;
                if (polygon->npoints < CAD_MIN_FACE_POINTS)
                    CadCore_DeletePolygon(core, (int16_t)polygonIndex);
                break;
            }
            previous = current;
            current = next;
            steps++;
        }
    }
    memset(&core->data.points[pointIndex], 0,
           sizeof(core->data.points[pointIndex]));
    core->data.points[pointIndex].nextPoint = INVALID_INDEX;
    CadCore_RebuildDerivedState(core);
    core->isDirty = 1;
    return 1;
}

CadPoint* CadCore_GetPoint(CadCore* core, int16_t index) {
    if (!core || !CadCore_IsPointValid(core, index)) return NULL;
    return &core->data.points[index];
}

int CadCore_IsPointValid(CadCore* core, int16_t index) {
    if (!core || index < 0 || index >= CAD_MAX_POINTS) return 0;
    return core->data.points[index].flags != 0;
}

/* ----------------------------------------------------------------------------
   Polygon operations
   ---------------------------------------------------------------------------- */

static int point_has_polygon_owner(const CadCore* core, int16_t pointIndex) {
    int polygonIndex;
    if (!core || pointIndex < 0 || pointIndex >= CAD_MAX_POINTS) return 0;
    for (polygonIndex = 0; polygonIndex < CAD_MAX_POLYGONS; ++polygonIndex) {
        const CadPolygon* polygon = &core->data.polygons[polygonIndex];
        int16_t current;
        int steps = 0;
        if (!polygon->flags) continue;
        current = polygon->firstPoint;
        while (current != INVALID_INDEX && steps++ < CAD_MAX_POINTS) {
            if (current < 0 || current >= CAD_MAX_POINTS ||
                !core->data.points[current].flags) break;
            if (current == pointIndex) return 1;
            current = core->data.points[current].nextPoint;
        }
    }
    return 0;
}

static int validate_unowned_point_chain(const CadCore* core,
                                        int16_t firstPoint,
                                        uint8_t npoints) {
    uint8_t visited[CAD_MAX_POINTS];
    int16_t current = firstPoint;
    int count = 0;
    if (!core || npoints < CAD_MIN_FACE_POINTS ||
        npoints > CAD_MAX_FACE_POINTS) return 0;
    memset(visited, 0, sizeof(visited));
    while (current != INVALID_INDEX && count < npoints) {
        if (current < 0 || current >= CAD_MAX_POINTS ||
            !core->data.points[current].flags || visited[current] ||
            point_has_polygon_owner(core, current)) return 0;
        visited[current] = 1;
        current = core->data.points[current].nextPoint;
        count++;
    }
    return count == npoints && current == INVALID_INDEX;
}

int16_t CadCore_AddPolygon(CadCore* core, int16_t firstPoint, uint8_t color, uint8_t npoints) {
    if (!core || !CadCore_IsPointValid(core, firstPoint) ||
        !validate_unowned_point_chain(core, firstPoint, npoints)) {
        return INVALID_INDEX;
    }
    
    /* Find first free slot */
    for (int16_t i = 0; i < CAD_MAX_POLYGONS; i++) {
        if (core->data.polygons[i].flags == 0) {
            CadPolygon* poly = &core->data.polygons[i];
            poly->flags = 1;
            poly->selectFlag = 0;
            poly->nextPolygon = INVALID_INDEX;
            poly->firstPoint = firstPoint;
            poly->animation = INVALID_INDEX;
            poly->both = INVALID_INDEX;
            poly->side = 0;
            poly->color = color;
            poly->npoints = npoints;
            
            if (i >= core->data.polygonCount) {
                core->data.polygonCount = i + 1;
            }
            
            /* Static editing is rooted in object zero, matching 3Ddraw. */
            if (!core->data.objects[0].flags) {
                CadObject* root = &core->data.objects[0];
                root->flags = 1;
                root->selectFlag = 0;
                root->parentObject = INVALID_INDEX;
                root->nextBrother = INVALID_INDEX;
                root->childObject = INVALID_INDEX;
                root->firstPolygon = i;
                root->offsetx = root->offsety = root->offsetz = 0.0;
                if (core->data.objectCount < 1) core->data.objectCount = 1;
            } else if (core->data.objects[0].firstPolygon == INVALID_INDEX) {
                core->data.objects[0].firstPolygon = i;
            } else {
                int16_t tail = core->data.objects[0].firstPolygon;
                int steps = 0;
                while (core->data.polygons[tail].nextPolygon != INVALID_INDEX &&
                       steps++ < CAD_MAX_POLYGONS)
                    tail = core->data.polygons[tail].nextPolygon;
                if (steps >= CAD_MAX_POLYGONS) {
                    memset(poly, 0, sizeof(*poly));
                    poly->nextPolygon = poly->firstPoint = poly->animation =
                        poly->both = INVALID_INDEX;
                    return INVALID_INDEX;
                }
                core->data.polygons[tail].nextPolygon = i;
            }
            core->rootPolygon = i;
            core->newPolygon = i;
            core->isDirty = 1;
            return i;
        }
    }
    
    return INVALID_INDEX; /* No free slots */
}

int CadCore_DeletePolygon(CadCore* core, int16_t polygonIndex) {
    CadPolygon oldPolygon;
    int objectIndex;
    int16_t point;
    int count;
    if (!core || !CadCore_IsPolygonValid(core, polygonIndex)) return 0;
    oldPolygon = core->data.polygons[polygonIndex];
    CadCore_DeselectPolygon(core, polygonIndex);
    for (objectIndex = 0; objectIndex < CAD_MAX_OBJECTS; ++objectIndex) {
        CadObject* object = &core->data.objects[objectIndex];
        int16_t current;
        int16_t previous = INVALID_INDEX;
        int steps = 0;
        if (!object->flags) continue;
        current = object->firstPolygon;
        while (current != INVALID_INDEX && steps++ < CAD_MAX_POLYGONS) {
            if (current < 0 || current >= CAD_MAX_POLYGONS ||
                !core->data.polygons[current].flags) break;
            if (current == polygonIndex) {
                if (previous == INVALID_INDEX)
                    object->firstPolygon = oldPolygon.nextPolygon;
                else
                    core->data.polygons[previous].nextPolygon =
                        oldPolygon.nextPolygon;
                break;
            }
            previous = current;
            current = core->data.polygons[current].nextPolygon;
        }
    }
    if (oldPolygon.both != INVALID_INDEX &&
        CadCore_IsPolygonValid(core, oldPolygon.both) &&
        core->data.polygons[oldPolygon.both].both == polygonIndex)
        core->data.polygons[oldPolygon.both].both = INVALID_INDEX;
    point = oldPolygon.firstPoint;
    for (count = 0; point != INVALID_INDEX && count < oldPolygon.npoints; ++count) {
        if (point < 0 || point >= CAD_MAX_POINTS ||
            !core->data.points[point].flags) break;
        int16_t next = core->data.points[point].nextPoint;
        CadCore_DeselectPoint(core, point);
        memset(&core->data.points[point], 0, sizeof(core->data.points[point]));
        core->data.points[point].nextPoint = INVALID_INDEX;
        point = next;
    }
    memset(&core->data.polygons[polygonIndex], 0,
           sizeof(core->data.polygons[polygonIndex]));
    core->data.polygons[polygonIndex].nextPolygon = INVALID_INDEX;
    core->data.polygons[polygonIndex].firstPoint = INVALID_INDEX;
    core->data.polygons[polygonIndex].animation = INVALID_INDEX;
    core->data.polygons[polygonIndex].both = INVALID_INDEX;
    CadCore_RebuildDerivedState(core);
    core->isDirty = 1;
    return 1;
}

CadPolygon* CadCore_GetPolygon(CadCore* core, int16_t index) {
    if (!core || !CadCore_IsPolygonValid(core, index)) return NULL;
    return &core->data.polygons[index];
}

int CadCore_IsPolygonValid(CadCore* core, int16_t index) {
    if (!core || index < 0 || index >= CAD_MAX_POLYGONS) return 0;
    return core->data.polygons[index].flags != 0;
}

int CadCore_AddPointToPolygon(CadCore* core, int16_t polygonIndex, int16_t pointIndex) {
    if (!core || !CadCore_IsPolygonValid(core, polygonIndex) || !CadCore_IsPointValid(core, pointIndex)) {
        return 0;
    }
    
    CadPolygon* poly = &core->data.polygons[polygonIndex];
    if (poly->npoints < CAD_MIN_FACE_POINTS ||
        poly->npoints >= CAD_MAX_FACE_POINTS ||
        core->data.points[pointIndex].nextPoint != INVALID_INDEX ||
        point_has_polygon_owner(core, pointIndex)) return 0;
    
    /* Find the last point in the polygon's chain */
    int16_t current = poly->firstPoint;
    {
        uint8_t visited[CAD_MAX_POINTS];
        int steps = 0;
        memset(visited, 0, sizeof(visited));
        while (current != INVALID_INDEX && steps < poly->npoints) {
            if (current < 0 || current >= CAD_MAX_POINTS ||
                !core->data.points[current].flags || visited[current]) return 0;
            visited[current] = 1;
            if (steps + 1 == poly->npoints) break;
            current = core->data.points[current].nextPoint;
            steps++;
        }
        if (current == INVALID_INDEX || steps + 1 != poly->npoints ||
            core->data.points[current].nextPoint != INVALID_INDEX) return 0;
        core->data.points[current].nextPoint = pointIndex;
        poly->npoints++;
    }
    
    core->isDirty = 1;
    return 1;
}

/* ----------------------------------------------------------------------------
   Object operations
   ---------------------------------------------------------------------------- */

int16_t CadCore_AddObject(CadCore* core, int16_t parentObject, double ox, double oy, double oz) {
    if (!core || (parentObject != INVALID_INDEX &&
                  !CadCore_IsObjectValid(core, parentObject)))
        return INVALID_INDEX;
    
    /* Find first free slot */
    for (int16_t i = 0; i < CAD_MAX_OBJECTS; i++) {
        if (core->data.objects[i].flags == 0) {
            CadObject* obj = &core->data.objects[i];
            obj->flags = 1;
            obj->selectFlag = 0;
            obj->parentObject = parentObject;
            obj->nextBrother = INVALID_INDEX;
            obj->childObject = INVALID_INDEX;
            obj->firstPolygon = INVALID_INDEX;
            obj->offsetx = ox;
            obj->offsety = oy;
            obj->offsetz = oz;
            
            if (i >= core->data.objectCount) {
                core->data.objectCount = i + 1;
            }
            if (parentObject != INVALID_INDEX) {
                CadObject* parent = &core->data.objects[parentObject];
                if (parent->childObject == INVALID_INDEX) {
                    parent->childObject = i;
                } else {
                    int16_t sibling = parent->childObject;
                    int steps = 0;
                    while (core->data.objects[sibling].nextBrother != INVALID_INDEX &&
                           steps++ < CAD_MAX_OBJECTS)
                        sibling = core->data.objects[sibling].nextBrother;
                    if (steps >= CAD_MAX_OBJECTS) {
                        memset(obj, 0, sizeof(*obj));
                        obj->parentObject = obj->nextBrother = obj->childObject =
                            obj->firstPolygon = INVALID_INDEX;
                        return INVALID_INDEX;
                    }
                    core->data.objects[sibling].nextBrother = i;
                }
            }
            
            core->isDirty = 1;
            return i;
        }
    }
    
    return INVALID_INDEX;
}

static void delete_object_record(CadCore* core, int16_t objectIndex) {
    CadObject oldObject;
    int16_t polygon;
    int i;
    if (!core || !CadCore_IsObjectValid(core, objectIndex)) return;
    oldObject = core->data.objects[objectIndex];
    /* Detach from its parent's child/sibling chain. */
    if (oldObject.parentObject != INVALID_INDEX &&
        CadCore_IsObjectValid(core, oldObject.parentObject)) {
        CadObject* parent = &core->data.objects[oldObject.parentObject];
        int16_t current = parent->childObject;
        int16_t previous = INVALID_INDEX;
        int steps = 0;
        while (current != INVALID_INDEX && steps++ < CAD_MAX_OBJECTS) {
            if (current < 0 || current >= CAD_MAX_OBJECTS ||
                !core->data.objects[current].flags) break;
            if (current == objectIndex) {
                if (previous == INVALID_INDEX)
                    parent->childObject = oldObject.nextBrother;
                else
                    core->data.objects[previous].nextBrother =
                        oldObject.nextBrother;
                break;
            }
            previous = current;
            current = core->data.objects[current].nextBrother;
        }
    }
    polygon = oldObject.firstPolygon;
    for (i = 0; polygon != INVALID_INDEX && i < CAD_MAX_POLYGONS; ++i) {
        if (polygon < 0 || polygon >= CAD_MAX_POLYGONS ||
            !core->data.polygons[polygon].flags) break;
        int16_t next = core->data.polygons[polygon].nextPolygon;
        CadCore_DeletePolygon(core, polygon);
        polygon = next;
    }
    memset(&core->data.objects[objectIndex], 0,
           sizeof(core->data.objects[objectIndex]));
    core->data.objects[objectIndex].parentObject = INVALID_INDEX;
    core->data.objects[objectIndex].nextBrother = INVALID_INDEX;
    core->data.objects[objectIndex].childObject = INVALID_INDEX;
    core->data.objects[objectIndex].firstPolygon = INVALID_INDEX;
    /* Remove stale links from malformed/recovered hierarchies too. */
    for (i = 0; i < CAD_MAX_OBJECTS; ++i) {
        if (!core->data.objects[i].flags) continue;
        if (core->data.objects[i].parentObject == objectIndex)
            core->data.objects[i].parentObject = INVALID_INDEX;
        if (core->data.objects[i].nextBrother == objectIndex)
            core->data.objects[i].nextBrother = oldObject.nextBrother;
        if (core->data.objects[i].childObject == objectIndex)
            core->data.objects[i].childObject = oldObject.nextBrother;
    }
}

int CadCore_DeleteObject(CadCore* core, int16_t objectIndex) {
    uint8_t included[CAD_MAX_OBJECTS];
    int16_t pending[CAD_MAX_OBJECTS];
    int16_t order[CAD_MAX_OBJECTS];
    int pendingCount = 0;
    int orderCount = 0;
    int i;
    if (!core || !CadCore_IsObjectValid(core, objectIndex)) return 0;
    memset(included, 0, sizeof(included));
    included[objectIndex] = 1;
    pending[pendingCount++] = objectIndex;
    while (pendingCount) {
        int16_t current = pending[--pendingCount];
        int16_t child;
        int steps = 0;
        order[orderCount++] = current;
        child = core->data.objects[current].childObject;
        while (child != INVALID_INDEX && steps++ < CAD_MAX_OBJECTS) {
            int16_t next;
            if (child < 0 || child >= CAD_MAX_OBJECTS ||
                !core->data.objects[child].flags) break;
            next = core->data.objects[child].nextBrother;
            if (!included[child] && pendingCount < CAD_MAX_OBJECTS) {
                included[child] = 1;
                pending[pendingCount++] = child;
            }
            child = next;
        }
    }
    for (i = orderCount - 1; i >= 0; --i)
        delete_object_record(core, order[i]);
    CadCore_RebuildDerivedState(core);
    core->isDirty = 1;
    return 1;
}

CadObject* CadCore_GetObject(CadCore* core, int16_t index) {
    if (!core || !CadCore_IsObjectValid(core, index)) return NULL;
    return &core->data.objects[index];
}

int CadCore_IsObjectValid(CadCore* core, int16_t index) {
    if (!core || index < 0 || index >= CAD_MAX_OBJECTS) return 0;
    return core->data.objects[index].flags != 0;
}

/* ----------------------------------------------------------------------------
   Selection operations
   ---------------------------------------------------------------------------- */

void CadCore_ClearSelection(CadCore* core) {
    if (!core) return;
    
    /* Clear all selection flags */
    for (int i = 0; i < core->data.pointCount; i++) {
        if (CadCore_IsPointValid(core, (int16_t)i)) {
            core->data.points[i].selectFlag = 0;
        }
    }
    for (int i = 0; i < core->data.polygonCount; i++) {
        if (CadCore_IsPolygonValid(core, (int16_t)i)) {
            core->data.polygons[i].selectFlag = 0;
        }
    }
    
    /* Clear selection arrays */
    for (int i = 0; i < CAD_MAX_POINTS; i++) {
        core->selection.selectedPoints[i] = INVALID_INDEX;
    }
    for (int i = 0; i < CAD_MAX_POLYGONS; i++) {
        core->selection.selectedPolygons[i] = INVALID_INDEX;
    }
    
    core->selection.pointCount = 0;
    core->selection.polygonCount = 0;
}

void CadCore_SelectPoint(CadCore* core, int16_t pointIndex) {
    if (!core || !CadCore_IsPointValid(core, pointIndex)) return;
    if (CadCore_IsPointSelected(core, pointIndex)) return; /* Already selected */
    
    core->data.points[pointIndex].selectFlag = 1;
    
    /* Add to selection array */
    if (core->selection.pointCount < CAD_MAX_POINTS) {
        core->selection.selectedPoints[core->selection.pointCount++] = pointIndex;
    }
}

void CadCore_SelectPolygon(CadCore* core, int16_t polygonIndex) {
    if (!core || !CadCore_IsPolygonValid(core, polygonIndex)) return;
    if (CadCore_IsPolygonSelected(core, polygonIndex)) return; /* Already selected */
    
    core->data.polygons[polygonIndex].selectFlag = 1;
    
    /* Add to selection array */
    if (core->selection.polygonCount < CAD_MAX_POLYGONS) {
        core->selection.selectedPolygons[core->selection.polygonCount++] = polygonIndex;
    }
}

void CadCore_DeselectPoint(CadCore* core, int16_t pointIndex) {
    if (!core || pointIndex < 0 || pointIndex >= CAD_MAX_POINTS) return;
    
    core->data.points[pointIndex].selectFlag = 0;
    
    /* Remove from selection array */
    for (int i = 0; i < core->selection.pointCount; i++) {
        if (core->selection.selectedPoints[i] == pointIndex) {
            /* Shift remaining elements */
            for (int j = i; j < core->selection.pointCount - 1; j++) {
                core->selection.selectedPoints[j] = core->selection.selectedPoints[j + 1];
            }
            core->selection.selectedPoints[core->selection.pointCount - 1] = INVALID_INDEX;
            core->selection.pointCount--;
            break;
        }
    }
}

void CadCore_DeselectPolygon(CadCore* core, int16_t polygonIndex) {
    if (!core || polygonIndex < 0 || polygonIndex >= CAD_MAX_POLYGONS) return;
    
    core->data.polygons[polygonIndex].selectFlag = 0;
    
    /* Remove from selection array */
    for (int i = 0; i < core->selection.polygonCount; i++) {
        if (core->selection.selectedPolygons[i] == polygonIndex) {
            /* Shift remaining elements */
            for (int j = i; j < core->selection.polygonCount - 1; j++) {
                core->selection.selectedPolygons[j] = core->selection.selectedPolygons[j + 1];
            }
            core->selection.selectedPolygons[core->selection.polygonCount - 1] = INVALID_INDEX;
            core->selection.polygonCount--;
            break;
        }
    }
}

int CadCore_IsPointSelected(const CadCore* core, int16_t pointIndex) {
    if (!core || pointIndex < 0 || pointIndex >= CAD_MAX_POINTS ||
        !core->data.points[pointIndex].flags) return 0;
    return core->data.points[pointIndex].selectFlag != 0;
}

int CadCore_IsPolygonSelected(const CadCore* core, int16_t polygonIndex) {
    if (!core || polygonIndex < 0 || polygonIndex >= CAD_MAX_POLYGONS ||
        !core->data.polygons[polygonIndex].flags) return 0;
    return core->data.polygons[polygonIndex].selectFlag != 0;
}

void CadCore_SelectAll(CadCore* core) {
    if (!core) return;
    
    if (core->selectModeFlag == 1) {
        /* Select all points */
        for (int i = 0; i < core->data.pointCount; i++) {
            if (CadCore_IsPointValid(core, (int16_t)i)) {
                CadCore_SelectPoint(core, (int16_t)i);
            }
        }
    } else {
        /* Select all polygons */
        for (int i = 0; i < core->data.polygonCount; i++) {
            if (CadCore_IsPolygonValid(core, (int16_t)i)) {
                CadCore_SelectPolygon(core, (int16_t)i);
            }
        }
    }
}

/* ----------------------------------------------------------------------------
   Edit mode
   ---------------------------------------------------------------------------- */

void CadCore_SetEditMode(CadCore* core, CadEditMode mode) {
    if (!core) return;
    core->editMode = mode;
    
    /* Update selectModeFlag based on mode */
    if (mode == CAD_MODE_SELECT_POINT || mode == CAD_MODE_EDIT_POINT) {
        core->selectModeFlag = 1;
    } else if (mode == CAD_MODE_SELECT_POLYGON || mode == CAD_MODE_EDIT_POLYGON) {
        core->selectModeFlag = 0;
    }
}

CadEditMode CadCore_GetEditMode(CadCore* core) {
    if (!core) return CAD_MODE_SELECT_POINT;
    return core->editMode;
}

/* ----------------------------------------------------------------------------
   Linked list helpers
   ---------------------------------------------------------------------------- */

int16_t CadCore_GetFirstPointOfPolygon(CadCore* core, int16_t polygonIndex) {
    if (!core || !CadCore_IsPolygonValid(core, polygonIndex)) return INVALID_INDEX;
    return core->data.polygons[polygonIndex].firstPoint;
}

int16_t CadCore_GetNextPoint(CadCore* core, int16_t pointIndex) {
    if (!core || !CadCore_IsPointValid(core, pointIndex)) return INVALID_INDEX;
    return core->data.points[pointIndex].nextPoint;
}

int16_t CadCore_GetNextPolygon(CadCore* core, int16_t polygonIndex) {
    if (!core || !CadCore_IsPolygonValid(core, polygonIndex)) return INVALID_INDEX;
    return core->data.polygons[polygonIndex].nextPolygon;
}

int16_t CadCore_GetFirstPolygonOfObject(CadCore* core, int16_t objectIndex) {
    if (!core || !CadCore_IsObjectValid(core, objectIndex)) return INVALID_INDEX;
    return core->data.objects[objectIndex].firstPolygon;
}

/* ----------------------------------------------------------------------------
   Validation
   ---------------------------------------------------------------------------- */

int CadCore_ValidatePolygon(CadCore* core, int16_t polygonIndex) {
    if (!core || !CadCore_IsPolygonValid(core, polygonIndex)) return 0;
    
    CadPolygon* poly = &core->data.polygons[polygonIndex];
    
    /* Check minimum vertex count */
    if (poly->npoints < CAD_MIN_FACE_POINTS ||
        poly->npoints > CAD_MAX_FACE_POINTS) return 0;
    
    /* Verify all points in chain are valid */
    int16_t current = poly->firstPoint;
    int count = 0;
    while (current != INVALID_INDEX && count < poly->npoints) {
        if (!CadCore_IsPointValid(core, current)) return 0;
        current = core->data.points[current].nextPoint;
        count++;
    }
    
    /* The recovered format uses an exact, non-circular point chain. */
    return count == poly->npoints && current == INVALID_INDEX;
}

int CadCore_ValidatePoint(CadCore* core, int16_t pointIndex) {
    if (!core || !CadCore_IsPointValid(core, pointIndex)) return 0;
    return 1; /* Point is valid if it exists */
}

int CadCore_ValidateDocument(const CadCore* core, char* diagnostic,
                             size_t diagnosticCapacity) {
    CadResult result;
    if (diagnostic && diagnosticCapacity) diagnostic[0] = '\0';
    if (!core) {
        if (diagnostic && diagnosticCapacity)
            snprintf(diagnostic, diagnosticCapacity, "CAD core is NULL");
        return 0;
    }
    result = CadCodec_Validate(&core->data);
    if (!CadResult_IsSuccess(&result)) {
        if (diagnostic && diagnosticCapacity) {
            const char* message = result.diagnosticCount
                ? result.diagnostics[0].message
                : CadStatus_Name(result.status);
            snprintf(diagnostic, diagnosticCapacity, "%s", message);
        }
        return 0;
    }
    return 1;
}

void CadCore_RebuildDerivedState(CadCore* core) {
    int i;
    int16_t polygon;
    int steps = 0;
    if (!core) return;
    core->data.objectCount = 0;
    core->data.polygonCount = 0;
    core->data.pointCount = 0;
    core->data.animationIndexCount = 0;
    core->data.animationPointCount = 0;
    core->selection.pointCount = 0;
    core->selection.polygonCount = 0;
    for (i = 0; i < CAD_MAX_POINTS; ++i)
        core->selection.selectedPoints[i] = INVALID_INDEX;
    for (i = 0; i < CAD_MAX_POLYGONS; ++i)
        core->selection.selectedPolygons[i] = INVALID_INDEX;

    for (i = 0; i < CAD_MAX_OBJECTS; ++i)
        if (core->data.objects[i].flags) core->data.objectCount = i + 1;
    for (i = 0; i < CAD_MAX_POLYGONS; ++i) {
        if (!core->data.polygons[i].flags) continue;
        core->data.polygonCount = i + 1;
        if (core->data.polygons[i].selectFlag &&
            core->selection.polygonCount < CAD_MAX_POLYGONS)
            core->selection.selectedPolygons[
                core->selection.polygonCount++] = (int16_t)i;
    }
    for (i = 0; i < CAD_MAX_POINTS; ++i) {
        if (!core->data.points[i].flags) continue;
        core->data.pointCount = i + 1;
        if (core->data.points[i].selectFlag &&
            core->selection.pointCount < CAD_MAX_POINTS)
            core->selection.selectedPoints[
                core->selection.pointCount++] = (int16_t)i;
    }
    for (i = 0; i < CAD_MAX_ANIMATION_INDICES; ++i)
        if (core->data.animationIndices[i].flags)
            core->data.animationIndexCount = i + 1;
    for (i = 0; i < CAD_MAX_ANIMATION_POINTS; ++i)
        if (core->data.animationPoints[i].flags)
            core->data.animationPointCount = i + 1;

    core->rootPolygon = INVALID_INDEX;
    if (core->data.objects[0].flags) {
        polygon = core->data.objects[0].firstPolygon;
        while (polygon != INVALID_INDEX && polygon >= 0 &&
               polygon < CAD_MAX_POLYGONS &&
               core->data.polygons[polygon].flags &&
               steps++ < CAD_MAX_POLYGONS) {
            core->rootPolygon = polygon;
            polygon = core->data.polygons[polygon].nextPolygon;
        }
    }
    if (core->newPoint < 0 || core->newPoint >= CAD_MAX_POINTS ||
        !core->data.points[core->newPoint].flags)
        core->newPoint = INVALID_INDEX;
    if (core->newPolygon < 0 || core->newPolygon >= CAD_MAX_POLYGONS ||
        !core->data.polygons[core->newPolygon].flags)
        core->newPolygon = INVALID_INDEX;
}

int CadCore_RepairTopology(CadCore* core) {
    uint8_t polygonOwner[CAD_MAX_POLYGONS];
    int changed = 0;
    int i;
    if (!core) return 0;
    memset(polygonOwner, 0, sizeof(polygonOwner));

    for (i = 0; i < CAD_MAX_POINTS; ++i) {
        CadPoint* point = &core->data.points[i];
        if (!point->flags) continue;
        if (point->flags != 2) { point->flags = 2; changed = 1; }
        if (point->nextPoint != INVALID_INDEX &&
            (point->nextPoint < 0 || point->nextPoint >= CAD_MAX_POINTS ||
             !core->data.points[point->nextPoint].flags)) {
            point->nextPoint = INVALID_INDEX;
            changed = 1;
        }
    }
    for (i = 0; i < CAD_MAX_POLYGONS; ++i) {
        CadPolygon* polygon = &core->data.polygons[i];
        int16_t current;
        int16_t previous = INVALID_INDEX;
        int count = 0;
        uint8_t visited[CAD_MAX_POINTS];
        if (!polygon->flags) continue;
        if (polygon->nextPolygon != INVALID_INDEX &&
            (polygon->nextPolygon < 0 || polygon->nextPolygon >= CAD_MAX_POLYGONS ||
             !core->data.polygons[polygon->nextPolygon].flags)) {
            polygon->nextPolygon = INVALID_INDEX; changed = 1;
        }
        if (polygon->both != INVALID_INDEX &&
            (polygon->both < 0 || polygon->both >= CAD_MAX_POLYGONS ||
             !core->data.polygons[polygon->both].flags || polygon->both == i)) {
            polygon->both = INVALID_INDEX; changed = 1;
        }
        if (polygon->animation != INVALID_INDEX &&
            (polygon->animation < 0 ||
             polygon->animation >= CAD_MAX_ANIMATION_INDICES ||
             !core->data.animationIndices[polygon->animation].flags)) {
            polygon->animation = INVALID_INDEX; changed = 1;
        }
        memset(visited, 0, sizeof(visited));
        current = polygon->firstPoint;
        while (current != INVALID_INDEX && count < CAD_MAX_FACE_POINTS &&
               current >= 0 && current < CAD_MAX_POINTS &&
               core->data.points[current].flags && !visited[current]) {
            visited[current] = 1;
            previous = current;
            current = core->data.points[current].nextPoint;
            count++;
        }
        if (previous != INVALID_INDEX && current != INVALID_INDEX) {
            core->data.points[previous].nextPoint = INVALID_INDEX;
            changed = 1;
        }
        if (polygon->npoints != count) {
            polygon->npoints = (uint8_t)count;
            changed = 1;
        }
        if (count < CAD_MIN_FACE_POINTS) {
            polygon->flags = 0;
            polygon->selectFlag = 0;
            polygon->nextPolygon = polygon->firstPoint =
                polygon->animation = polygon->both = INVALID_INDEX;
            polygon->npoints = 0;
            changed = 1;
        }
    }
    /* Restore reciprocal paired-face relationships when unambiguous. */
    for (i = 0; i < CAD_MAX_POLYGONS; ++i) {
        CadPolygon* polygon = &core->data.polygons[i];
        if (!polygon->flags || polygon->both == INVALID_INDEX) continue;
        if (core->data.polygons[polygon->both].both == INVALID_INDEX) {
            core->data.polygons[polygon->both].both = (int16_t)i;
            changed = 1;
        } else if (core->data.polygons[polygon->both].both != i) {
            polygon->both = INVALID_INDEX;
            changed = 1;
        }
    }
    for (i = 0; i < CAD_MAX_OBJECTS; ++i) {
        CadObject* object = &core->data.objects[i];
        int16_t polygon;
        int16_t previous = INVALID_INDEX;
        int steps = 0;
        if (!object->flags) continue;
#define CLEAR_BAD_OBJECT_LINK(field) \
        do { \
            int16_t link = object->field; \
            if (link != INVALID_INDEX && \
                (link < 0 || link >= CAD_MAX_OBJECTS || \
                 !core->data.objects[link].flags || link == i)) { \
                object->field = INVALID_INDEX; changed = 1; \
            } \
        } while (0)
        CLEAR_BAD_OBJECT_LINK(parentObject);
        CLEAR_BAD_OBJECT_LINK(nextBrother);
        CLEAR_BAD_OBJECT_LINK(childObject);
#undef CLEAR_BAD_OBJECT_LINK
        if (object->firstPolygon != INVALID_INDEX &&
            (object->firstPolygon < 0 ||
             object->firstPolygon >= CAD_MAX_POLYGONS ||
             !core->data.polygons[object->firstPolygon].flags)) {
            object->firstPolygon = INVALID_INDEX; changed = 1;
        }
        polygon = object->firstPolygon;
        while (polygon != INVALID_INDEX && steps++ < CAD_MAX_POLYGONS) {
            int16_t next = core->data.polygons[polygon].nextPolygon;
            if (polygonOwner[polygon]) {
                if (previous == INVALID_INDEX) object->firstPolygon = INVALID_INDEX;
                else core->data.polygons[previous].nextPolygon = INVALID_INDEX;
                changed = 1;
                break;
            }
            polygonOwner[polygon] = 1;
            previous = polygon;
            polygon = next;
        }
        if (steps >= CAD_MAX_POLYGONS && polygon != INVALID_INDEX &&
            previous != INVALID_INDEX) {
            core->data.polygons[previous].nextPolygon = INVALID_INDEX;
            changed = 1;
        }
    }
    /* Any recovered orphan faces become part of the traditional root object. */
    for (i = 0; i < CAD_MAX_POLYGONS; ++i) {
        int16_t tail;
        int steps = 0;
        if (!core->data.polygons[i].flags || polygonOwner[i]) continue;
        if (!core->data.objects[0].flags) {
            CadObject* root = &core->data.objects[0];
            root->flags = 1;
            root->parentObject = root->nextBrother = root->childObject =
                root->firstPolygon = INVALID_INDEX;
            root->offsetx = root->offsety = root->offsetz = 0.0;
            changed = 1;
        }
        if (core->data.objects[0].firstPolygon == INVALID_INDEX) {
            core->data.objects[0].firstPolygon = (int16_t)i;
        } else {
            tail = core->data.objects[0].firstPolygon;
            while (core->data.polygons[tail].nextPolygon != INVALID_INDEX &&
                   steps++ < CAD_MAX_POLYGONS)
                tail = core->data.polygons[tail].nextPolygon;
            if (steps < CAD_MAX_POLYGONS)
                core->data.polygons[tail].nextPolygon = (int16_t)i;
        }
        core->data.polygons[i].nextPolygon = INVALID_INDEX;
        polygonOwner[i] = 1;
        changed = 1;
    }
    CadCore_RebuildDerivedState(core);
    if (changed) core->isDirty = 1;
    return CadCore_ValidateDocument(core, NULL, 0);
}

/* ----------------------------------------------------------------------------
   Statistics
   ---------------------------------------------------------------------------- */

int CadCore_GetActivePointCount(CadCore* core) {
    if (!core) return 0;
    
    int count = 0;
    for (int i = 0; i < core->data.pointCount; i++) {
        if (CadCore_IsPointValid(core, (int16_t)i)) count++;
    }
    return count;
}

int CadCore_GetActivePolygonCount(CadCore* core) {
    if (!core) return 0;
    
    int count = 0;
    for (int i = 0; i < core->data.polygonCount; i++) {
        if (CadCore_IsPolygonValid(core, (int16_t)i)) count++;
    }
    return count;
}

int CadCore_GetActiveObjectCount(CadCore* core) {
    if (!core) return 0;
    
    int count = 0;
    for (int i = 0; i < core->data.objectCount; i++) {
        if (CadCore_IsObjectValid(core, (int16_t)i)) count++;
    }
    return count;
}

/* ----------------------------------------------------------------------------
   Merge detection
   ---------------------------------------------------------------------------- */

/* Convert coordinate to integer (round half up) - matches original Convert() function */
int CadCore_ConvertCoordinate(double coord) {
    double fractional;
    double integer;
    fractional = coord - (integer = (double)(int)coord);
    
    if (coord >= 0.0) {
        if (fractional >= 0.5) integer += 1.0;
    } else {
        if (fractional <= -0.5) integer -= 1.0;
    }
    
    return (int)integer;
}

static int convert_coordinate(double coord) {
    return CadCore_ConvertCoordinate(coord);
}

/* Check if coordinates are merged (all coordinates are integers) */
int CadCore_AreCoordinatesMerged(CadCore* core) {
    if (!core) return 0;
    
    /* Check all valid points */
    for (int i = 0; i < core->data.pointCount && i < CAD_MAX_POINTS; i++) {
        CadPoint* pt = &core->data.points[i];
        if (pt->flags == 0) continue; /* Skip invalid points */
        
        /* Check if coordinates are integers (within epsilon) */
        double x = pt->pointx;
        double y = pt->pointy;
        double z = pt->pointz;
        
        /* Convert and check if result matches original (within small epsilon) */
        int conv_x = convert_coordinate(x);
        int conv_y = convert_coordinate(y);
        int conv_z = convert_coordinate(z);
        
        const double epsilon = 1e-9;
        if (fabs(x - (double)conv_x) > epsilon ||
            fabs(y - (double)conv_y) > epsilon ||
            fabs(z - (double)conv_z) > epsilon) {
            return 0; /* Found non-integer coordinate */
        }
    }
    
    /* Also check object offsets */
    for (int i = 0; i < core->data.objectCount && i < CAD_MAX_OBJECTS; i++) {
        CadObject* obj = &core->data.objects[i];
        if (obj->flags == 0) continue; /* Skip invalid objects */
        
        double ox = obj->offsetx;
        double oy = obj->offsety;
        double oz = obj->offsetz;
        
        int conv_ox = convert_coordinate(ox);
        int conv_oy = convert_coordinate(oy);
        int conv_oz = convert_coordinate(oz);
        
        const double epsilon = 1e-9;
        if (fabs(ox - (double)conv_ox) > epsilon ||
            fabs(oy - (double)conv_oy) > epsilon ||
            fabs(oz - (double)conv_oz) > epsilon) {
            return 0; /* Found non-integer offset */
        }
    }
    
    return 1; /* All coordinates are integers */
}

/* Check if points are merged (no duplicate points at same grid location) */
int CadCore_ArePointsMerged(CadCore* core) {
    if (!core) return 0;
    
    /* For each polygon, check for consecutive duplicate points */
    for (int poly_idx = 0; poly_idx < core->data.polygonCount && poly_idx < CAD_MAX_POLYGONS; poly_idx++) {
        CadPolygon* poly = &core->data.polygons[poly_idx];
        if (poly->flags == 0) continue; /* Skip invalid polygons */
        
        int16_t point = poly->firstPoint;
        if (point == INVALID_INDEX) continue;
        
        int count = poly->npoints;
        int16_t visited[64];
        int visited_count = 0;
        
        /* Declare variables for point traversal */
        int16_t current;
        int checked;
        
        /* Check first point against last point (closed polygon check) */
        if (count > 1) {
            int16_t first_point = poly->firstPoint;
            int16_t last_point = point;
            
            /* Find last point */
            current = point;
            checked = 0;
            while (current != INVALID_INDEX && current < CAD_MAX_POINTS && checked < count) {
                CadPoint* pt = &core->data.points[current];
                if (pt->flags == 0) break;
                last_point = current;
                current = pt->nextPoint;
                checked++;
            }
            
            if (first_point != INVALID_INDEX && last_point != INVALID_INDEX &&
                first_point < CAD_MAX_POINTS && last_point < CAD_MAX_POINTS) {
                CadPoint* first_pt = &core->data.points[first_point];
                CadPoint* last_pt = &core->data.points[last_point];
                
                if (first_pt->flags != 0 && last_pt->flags != 0) {
                    int first_x = convert_coordinate(first_pt->pointx);
                    int first_y = convert_coordinate(first_pt->pointy);
                    int first_z = convert_coordinate(first_pt->pointz);
                    int last_x = convert_coordinate(last_pt->pointx);
                    int last_y = convert_coordinate(last_pt->pointy);
                    int last_z = convert_coordinate(last_pt->pointz);
                    
                    if (first_x == last_x && first_y == last_y && first_z == last_z) {
                        return 0; /* Found duplicate: first and last point are same */
                    }
                }
            }
        }
        
        /* Check consecutive points in polygon */
        current = point;
        int16_t prev_point = INVALID_INDEX;
        checked = 0;
        
        while (current != INVALID_INDEX && current < CAD_MAX_POINTS && checked < count) {
            /* Cycle detection */
            int already_visited = 0;
            for (int v = 0; v < visited_count && v < 64; v++) {
                if (visited[v] == current) {
                    already_visited = 1;
                    break;
                }
            }
            if (already_visited) break;
            if (visited_count < 64) {
                visited[visited_count++] = current;
            }
            
            CadPoint* pt = &core->data.points[current];
            if (pt->flags == 0) break;
            
            if (prev_point != INVALID_INDEX && prev_point < CAD_MAX_POINTS) {
                CadPoint* prev_pt = &core->data.points[prev_point];
                
                /* Check if converted coordinates match */
                int prev_x = convert_coordinate(prev_pt->pointx);
                int prev_y = convert_coordinate(prev_pt->pointy);
                int prev_z = convert_coordinate(prev_pt->pointz);
                int curr_x = convert_coordinate(pt->pointx);
                int curr_y = convert_coordinate(pt->pointy);
                int curr_z = convert_coordinate(pt->pointz);
                
                if (prev_x == curr_x && prev_y == curr_y && prev_z == curr_z) {
                    return 0; /* Found duplicate consecutive points */
                }
            }
            
            prev_point = current;
            current = pt->nextPoint;
            checked++;
            if (checked > 1000) break; /* Safety limit */
        }
    }
    
    return 1; /* No duplicate points found */
}

/* Check if all merge operations have been applied */
int CadCore_IsFullyMerged(CadCore* core) {
    if (!core) return 0;
    
    return CadCore_AreCoordinatesMerged(core) && CadCore_ArePointsMerged(core);
}

/* ----------------------------------------------------------------------------
   Check if a point is connected to any polygon
   ---------------------------------------------------------------------------- */
int CadCore_IsPointConnected(const CadCore* core, int16_t pointIndex) {
    if (!core || pointIndex < 0 || pointIndex >= CAD_MAX_POINTS) return 0;
    if (!core->data.points[pointIndex].flags) return 0;
    
    /* Check all polygons to see if this point is used */
    for (int i = 0; i < core->data.polygonCount; i++) {
        const CadPolygon* poly = &core->data.polygons[i];
        if (!poly->flags) continue;
        if (poly->npoints < 2) continue;
        
        /* Traverse the polygon's point linked list */
        int16_t current = poly->firstPoint;
        int visited_count = 0;
        int16_t visited[64]; /* Cycle detection */
        
        while (current >= 0 && current < CAD_MAX_POINTS && visited_count < 64) {
            /* Check for cycles */
            int already_visited = 0;
            for (int v = 0; v < visited_count; v++) {
                if (visited[v] == current) {
                    already_visited = 1;
                    break;
                }
            }
            if (already_visited) break;
            visited[visited_count++] = current;
            
            /* If this point matches, it's connected */
            if (current == pointIndex) {
                return 1;
            }
            
            const CadPoint* pt = &core->data.points[current];
            if (!pt->flags) break;
            
            current = pt->nextPoint;
        }
    }
    
    return 0; /* Not found in any polygon */
}

