#pragma once

/* ============================================================================
   cad_file.h
   CAD file format definitions for 3DCadGui
   
   Based on the original IWAMOTO CAD (NEWS) file format
   ============================================================================ */

#include <stdint.h>

/* ----------------------------------------------------------------------------
   Minimum counts
   ---------------------------------------------------------------------------- */
#define CAD_MIN_FACE_POINTS    2    /* min face points */


/* ----------------------------------------------------------------------------
   Maximum counts
   ---------------------------------------------------------------------------- */
#define CAD_MAX_OBJECTS     256     /* max objects */
#define CAD_MAX_POINTS     1024    /* max points */
#define CAD_MAX_POLYGONS   1024    /* max polygons */
#define CAD_MAX_FACE_POINTS 12      /* max face points */

/* ----------------------------------------------------------------------------
   File format tags
   ---------------------------------------------------------------------------- */
#define CAD_TAG_OBJECT     0       /* Object record */
#define CAD_TAG_POLYGON    1       /* Polygon record */
#define CAD_TAG_POINT      2       /* Point record */

/* ----------------------------------------------------------------------------
   Point record (vertex)
   ---------------------------------------------------------------------------- */
typedef struct {
    uint8_t  flags;          /* Flags */
    uint8_t  selectFlag;     /* Selection flag */
    int16_t  nextPoint;      /* Index to next point in polygon (-1 = end) */
    double   pointx;         /* X coordinate */
    double   pointy;         /* Y coordinate */
    double   pointz;         /* Z coordinate */
} CadPoint;

/* ----------------------------------------------------------------------------
   Polygon record (face)
   ---------------------------------------------------------------------------- */
typedef struct {
    uint8_t  flags;          /* Flags */
    uint8_t  selectFlag;     /* Selection flag */
    int16_t  nextPolygon;    /* Index to next polygon in same group (-1 = end) */
    int16_t  firstPoint;     /* Index to first vertex of polygon */
    int16_t  animation;      /* Animation frame index */
    int16_t  both;           /* Opposite side index (double-sided polygon) */
    uint8_t  side;           /* Front/back flag */
    uint8_t  color;          /* Polygon color */
    uint8_t  npoints;        /* Vertex count */
} CadPolygon;

/* ----------------------------------------------------------------------------
   Object record (hierarchical object)
   ---------------------------------------------------------------------------- */
typedef struct {
    uint8_t  flags;          /* Flags */
    uint8_t  selectFlag;     /* Selection flag */
    int16_t  parentObject;   /* Index to parent object (-1 = root) */
    int16_t  nextBrother;    /* Index to next sibling object (-1 = end) */
    int16_t  childObject;    /* Index to first child object (-1 = none) */
    int16_t  firstPolygon;   /* Index to first polygon (-1 = none) */
    double   offsetx;        /* Offset X relative to parent */
    double   offsety;        /* Offset Y relative to parent */
    double   offsetz;        /* Offset Z relative to parent */
} CadObject;

/* ----------------------------------------------------------------------------
   CAD file data structure
   ---------------------------------------------------------------------------- */
typedef struct {
    CadObject  objects[CAD_MAX_OBJECTS];
    CadPolygon polygons[CAD_MAX_POLYGONS];
    CadPoint   points[CAD_MAX_POINTS];
    
    int objectCount;
    int polygonCount;
    int pointCount;
} CadFileData;

/* ----------------------------------------------------------------------------
   Function prototypes
   ---------------------------------------------------------------------------- */

/* Load a .cad file */
int CadFile_Load(const char* filename, CadFileData* data);

/* Save a .cad file */
int CadFile_Save(const char* filename, const CadFileData* data);

/* Initialize empty CAD data */
void CadFile_Init(CadFileData* data);

/* Clear all data */
void CadFile_Clear(CadFileData* data);

/* Get point by index (returns NULL if invalid) */
CadPoint* CadFile_GetPoint(CadFileData* data, int16_t index);

/* Get polygon by index (returns NULL if invalid) */
CadPolygon* CadFile_GetPolygon(CadFileData* data, int16_t index);

/* Get object by index (returns NULL if invalid) */
CadObject* CadFile_GetObject(CadFileData* data, int16_t index);


