#pragma once

/* ============================================================================
   cad_core.h
   Core CAD data management and operations
   ============================================================================ */

#include "cad_file.h"
#include <stddef.h>
#include <stdint.h>

/* ----------------------------------------------------------------------------
   Constants
   ---------------------------------------------------------------------------- */
#define INVALID_INDEX -1

/* ----------------------------------------------------------------------------
   Edit modes
   ---------------------------------------------------------------------------- */
typedef enum {
    CAD_MODE_SELECT_POINT = 0,
    CAD_MODE_SELECT_POLYGON = 1,
    CAD_MODE_EDIT_POINT = 2,
    CAD_MODE_EDIT_POLYGON = 11
} CadEditMode;

/* ----------------------------------------------------------------------------
   Selection state
   ---------------------------------------------------------------------------- */
typedef struct {
    int16_t selectedPoints[CAD_MAX_POINTS];
    int16_t selectedPolygons[CAD_MAX_POLYGONS];
    int pointCount;
    int polygonCount;
} CadSelection;

/* ----------------------------------------------------------------------------
   Core CAD state
   ---------------------------------------------------------------------------- */
typedef struct {
    CadFileData data;
    
    /* Current state */
    CadEditMode editMode;
    int selectModeFlag;  /* 1 = point selection, 0 = polygon selection */
    
    /* Selection */
    CadSelection selection;
    
    /* Active editing */
    int16_t newPoint;         /* Most recently registered point */
    int16_t newPolygon;       /* Most recently registered polygon */
    int16_t rootPolygon;      /* Previously registered polygon */
    int16_t creatingPoint;   /* Previously registered point */
    int16_t firstPoint;       /* First point */
    
    /* Dirty flag */
    int isDirty;             /* Has unsaved changes */
} CadCore;

/* ----------------------------------------------------------------------------
   Core initialization and cleanup
   ---------------------------------------------------------------------------- */
void CadCore_Init(CadCore* core);
void CadCore_Destroy(CadCore* core);
void CadCore_Clear(CadCore* core);

/* ----------------------------------------------------------------------------
   File operations
   ---------------------------------------------------------------------------- */
int CadCore_LoadFile(CadCore* core, const char* filename);
int CadCore_SaveFile(CadCore* core, const char* filename);

/* ----------------------------------------------------------------------------
   Point operations
   ---------------------------------------------------------------------------- */
int16_t CadCore_AddPoint(CadCore* core, double x, double y, double z);
int CadCore_DeletePoint(CadCore* core, int16_t pointIndex);
CadPoint* CadCore_GetPoint(CadCore* core, int16_t index);
int CadCore_IsPointValid(CadCore* core, int16_t index);

/* ----------------------------------------------------------------------------
   Polygon operations
   ---------------------------------------------------------------------------- */
int16_t CadCore_AddPolygon(CadCore* core, int16_t firstPoint, uint8_t color, uint8_t npoints);
int CadCore_DeletePolygon(CadCore* core, int16_t polygonIndex);
CadPolygon* CadCore_GetPolygon(CadCore* core, int16_t index);
int CadCore_IsPolygonValid(CadCore* core, int16_t index);
int CadCore_AddPointToPolygon(CadCore* core, int16_t polygonIndex, int16_t pointIndex);

/* ----------------------------------------------------------------------------
   Object operations
   ---------------------------------------------------------------------------- */
int16_t CadCore_AddObject(CadCore* core, int16_t parentObject, double ox, double oy, double oz);
int CadCore_DeleteObject(CadCore* core, int16_t objectIndex);
CadObject* CadCore_GetObject(CadCore* core, int16_t index);
int CadCore_IsObjectValid(CadCore* core, int16_t index);

/* ----------------------------------------------------------------------------
   Selection operations
   ---------------------------------------------------------------------------- */
void CadCore_ClearSelection(CadCore* core);
void CadCore_SelectPoint(CadCore* core, int16_t pointIndex);
void CadCore_SelectPolygon(CadCore* core, int16_t polygonIndex);
void CadCore_DeselectPoint(CadCore* core, int16_t pointIndex);
void CadCore_DeselectPolygon(CadCore* core, int16_t polygonIndex);
int CadCore_IsPointSelected(const CadCore* core, int16_t pointIndex);
int CadCore_IsPolygonSelected(const CadCore* core, int16_t polygonIndex);
void CadCore_SelectAll(CadCore* core);

/* ----------------------------------------------------------------------------
   Edit mode
   ---------------------------------------------------------------------------- */
void CadCore_SetEditMode(CadCore* core, CadEditMode mode);
CadEditMode CadCore_GetEditMode(CadCore* core);

/* ----------------------------------------------------------------------------
   Linked list helpers
   ---------------------------------------------------------------------------- */
int16_t CadCore_GetFirstPointOfPolygon(CadCore* core, int16_t polygonIndex);
int16_t CadCore_GetNextPoint(CadCore* core, int16_t pointIndex);
int16_t CadCore_GetNextPolygon(CadCore* core, int16_t polygonIndex);
int16_t CadCore_GetFirstPolygonOfObject(CadCore* core, int16_t objectIndex);

/* ----------------------------------------------------------------------------
   Validation
   ---------------------------------------------------------------------------- */
int CadCore_ValidatePolygon(CadCore* core, int16_t polygonIndex);
int CadCore_ValidatePoint(CadCore* core, int16_t pointIndex);

/* Validate every active record and all object/polygon/point/animation links.
   diagnostic may be NULL; otherwise it receives a short first-error message. */
int CadCore_ValidateDocument(const CadCore* core, char* diagnostic,
                             size_t diagnosticCapacity);

/* Recompute high-water counts, selection arrays and the last root polygon.
   RepairTopology also removes dangling links and restores reciprocal face
   pairing; it is intended for explicit recovery/import paths, not rendering. */
void CadCore_RebuildDerivedState(CadCore* core);
int CadCore_RepairTopology(CadCore* core);

/* ----------------------------------------------------------------------------
   Statistics
   ---------------------------------------------------------------------------- */
int CadCore_GetActivePointCount(CadCore* core);
int CadCore_GetActivePolygonCount(CadCore* core);
int CadCore_GetActiveObjectCount(CadCore* core);

/* ----------------------------------------------------------------------------
   Merge detection
   ---------------------------------------------------------------------------- */

/* Convert coordinate to integer (round half up) - matches original Convert() function */
int CadCore_ConvertCoordinate(double coord);

/* Check if coordinates are merged (all coordinates are integers) */
int CadCore_AreCoordinatesMerged(CadCore* core);

/* Check if points are merged (no duplicate points at same grid location) */
int CadCore_ArePointsMerged(CadCore* core);

/* Remove consecutive and closing linked-chain vertices that resolve to the
   same recovered integer grid position. Polygons reduced below the recovered
   two-point minimum are removed. Returns the number of point records removed. */
int CadCore_MergePolygonPoints(CadCore* core);

/* Check if all merge operations have been applied */
int CadCore_IsFullyMerged(CadCore* core);

/* Check if a point is connected to any polygon (not orphaned) */
int CadCore_IsPointConnected(const CadCore* core, int16_t pointIndex);


