#define _CRT_SECURE_NO_WARNINGS

#include "cad_core.h"
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
    if (!core || !filename) return 0;
    
    CadCore_Clear(core);
    
    if (!CadFile_Load(filename, &core->data)) {
        return 0;
    }
    
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
            pt->flags = 1;
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
    if (!core || !CadCore_IsPointValid(core, pointIndex)) return 0;
    
    /* Mark as deleted (set flags to 0) */
    core->data.points[pointIndex].flags = 0;
    core->data.points[pointIndex].selectFlag = 0;
    
    /* Remove from selection if selected */
    CadCore_DeselectPoint(core, pointIndex);
    
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

int16_t CadCore_AddPolygon(CadCore* core, int16_t firstPoint, uint8_t color, uint8_t npoints) {
    if (!core || !CadCore_IsPointValid(core, firstPoint) || npoints < 2) {
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
            poly->animation = 0;
            poly->both = INVALID_INDEX;
            poly->side = 0;
            poly->color = color;
            poly->npoints = npoints;
            
            if (i >= core->data.polygonCount) {
                core->data.polygonCount = i + 1;
            }
            
            core->newPolygon = i;
            core->isDirty = 1;
            return i;
        }
    }
    
    return INVALID_INDEX; /* No free slots */
}

int CadCore_DeletePolygon(CadCore* core, int16_t polygonIndex) {
    if (!core || !CadCore_IsPolygonValid(core, polygonIndex)) return 0;
    
    /* Mark as deleted */
    core->data.polygons[polygonIndex].flags = 0;
    core->data.polygons[polygonIndex].selectFlag = 0;
    
    /* Remove from selection if selected */
    CadCore_DeselectPolygon(core, polygonIndex);
    
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
    
    /* Find the last point in the polygon's chain */
    int16_t current = poly->firstPoint;
    if (current == INVALID_INDEX) {
        /* First point */
        poly->firstPoint = pointIndex;
        poly->npoints = 1;
    } else {
        /* Traverse to end of chain */
        while (core->data.points[current].nextPoint != INVALID_INDEX) {
            current = core->data.points[current].nextPoint;
        }
        /* Link new point */
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
    if (!core) return INVALID_INDEX;
    
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
            
            core->isDirty = 1;
            return i;
        }
    }
    
    return INVALID_INDEX;
}

int CadCore_DeleteObject(CadCore* core, int16_t objectIndex) {
    if (!core || !CadCore_IsObjectValid(core, objectIndex)) return 0;
    
    /* Mark as deleted */
    core->data.objects[objectIndex].flags = 0;
    core->data.objects[objectIndex].selectFlag = 0;
    
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
    if (!core || !CadCore_IsPointValid(core, pointIndex)) return;
    
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
    if (!core || !CadCore_IsPolygonValid(core, polygonIndex)) return;
    
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

int CadCore_IsPointSelected(CadCore* core, int16_t pointIndex) {
    if (!core || !CadCore_IsPointValid(core, pointIndex)) return 0;
    return core->data.points[pointIndex].selectFlag != 0;
}

int CadCore_IsPolygonSelected(CadCore* core, int16_t polygonIndex) {
    if (!core || !CadCore_IsPolygonValid(core, polygonIndex)) return 0;
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
    if (poly->npoints < 2) return 0;
    
    /* Verify all points in chain are valid */
    int16_t current = poly->firstPoint;
    int count = 0;
    while (current != INVALID_INDEX && count < poly->npoints) {
        if (!CadCore_IsPointValid(core, current)) return 0;
        current = core->data.points[current].nextPoint;
        count++;
    }
    
    /* Check if we got the expected number of points */
    return count == poly->npoints;
}

int CadCore_ValidatePoint(CadCore* core, int16_t pointIndex) {
    if (!core || !CadCore_IsPointValid(core, pointIndex)) return 0;
    return 1; /* Point is valid if it exists */
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
int CadCore_IsPointConnected(CadCore* core, int16_t pointIndex) {
    if (!core || pointIndex < 0 || pointIndex >= CAD_MAX_POINTS) return 0;
    if (!CadCore_IsPointValid(core, pointIndex)) return 0;
    
    /* Check all polygons to see if this point is used */
    for (int i = 0; i < core->data.polygonCount; i++) {
        CadPolygon* poly = CadCore_GetPolygon(core, (int16_t)i);
        if (!poly || poly->flags == 0) continue;
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
            
            CadPoint* pt = CadCore_GetPoint(core, current);
            if (!pt || pt->flags == 0) break;
            
            current = pt->nextPoint;
        }
    }
    
    return 0; /* Not found in any polygon */
}

