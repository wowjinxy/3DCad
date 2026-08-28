#pragma once

/* ============================================================================
   cad_view.h
   CAD view rendering and camera system
   ============================================================================ */

#include "cad_core.h"
#include <stdint.h>

/* ----------------------------------------------------------------------------
   View types
   ---------------------------------------------------------------------------- */
typedef enum {
    CAD_VIEW_TOP = 0,
    CAD_VIEW_FRONT = 1,
    CAD_VIEW_RIGHT = 2,
    CAD_VIEW_3D = 3
} CadViewType;

/* ----------------------------------------------------------------------------
   View state
   ---------------------------------------------------------------------------- */
typedef struct {
    CadViewType type;
    double zoom;           /* Zoom factor */
    double pan_x, pan_y;   /* Pan offset */
    double rot_x, rot_y;   /* Rotation (for 3D view) */
    int wireframe;         /* 1 = wireframe, 0 = solid */
} CadView;

/* ----------------------------------------------------------------------------
   View initialization
   ---------------------------------------------------------------------------- */
void CadView_Init(CadView* view, CadViewType type);
void CadView_Reset(CadView* view);

/* ----------------------------------------------------------------------------
   View transformations
   ---------------------------------------------------------------------------- */
void CadView_SetZoom(CadView* view, double zoom);
void CadView_Pan(CadView* view, double dx, double dy);
void CadView_Rotate(CadView* view, double dx, double dy);
void CadView_Pan3DVertical(CadView* view, double dy); /* Pan 3D view up/down relative to current angle */

/* ----------------------------------------------------------------------------
   3D to 2D projection
   ---------------------------------------------------------------------------- */
void CadView_ProjectPoint(const CadView* view, double x, double y, double z, 
                         int* out_x, int* out_y, int viewport_w, int viewport_h);

/* ----------------------------------------------------------------------------
   Point selection (find nearest point to screen coordinates)
   Returns point index or -1 if none found within threshold
   ---------------------------------------------------------------------------- */
int16_t CadView_FindNearestPoint(const CadView* view, const CadCore* core,
                                 int screen_x, int screen_y,
                                 int viewport_x, int viewport_y,
                                 int viewport_w, int viewport_h,
                                 int threshold_pixels);

/* ----------------------------------------------------------------------------
   Find all points at the same location as the nearest point
   Fills out_indices array with point indices (up to max_count)
   Returns number of points found, or 0 if none found
   ---------------------------------------------------------------------------- */
int CadView_FindPointsAtLocation(const CadView* view, const CadCore* core,
                                 int screen_x, int screen_y,
                                 int viewport_x, int viewport_y,
                                 int viewport_w, int viewport_h,
                                 int threshold_pixels,
                                 double world_threshold,
                                 int16_t* out_indices, int max_count);

/* ----------------------------------------------------------------------------
   Unproject screen delta to 3D world delta
   Converts screen space movement (dx, dy in pixels) to 3D world space movement
   ---------------------------------------------------------------------------- */
void CadView_UnprojectDelta(const CadView* view, int screen_dx, int screen_dy,
                            int viewport_w, int viewport_h,
                            double* out_dx, double* out_dy, double* out_dz);

/* ----------------------------------------------------------------------------
   Unproject screen point to 3D world coordinates
   Converts screen coordinates (relative to viewport) to 3D world coordinates
   For orthographic views, projects onto the view plane at z=0
   ---------------------------------------------------------------------------- */
void CadView_UnprojectPoint(const CadView* view, int screen_x, int screen_y,
                            int viewport_w, int viewport_h,
                            double* out_x, double* out_y, double* out_z);

/* ----------------------------------------------------------------------------
   Rendering
   ---------------------------------------------------------------------------- */
/* Pixel bounds select the framebuffer region; logical dimensions keep model
   projection aligned with DPI-independent GUI input coordinates. */
void CadView_Render(const CadView* view, const CadCore* core,
                    int pixel_x, int pixel_y, int pixel_w, int pixel_h,
                    int framebuffer_h, int logical_w, int logical_h);

