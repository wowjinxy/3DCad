#pragma once

/* ============================================================================
   cad_view.h
   CAD view rendering and camera system
   ============================================================================ */

#include "cad_animation.h"
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
    double rot_z;          /* Camera roll (for 3D view) */
    double camera_distance;/* Perspective camera distance */
    double focal_length;   /* Perspective focal length */
    int wireframe;         /* 1 = wireframe, 0 = solid */
    int show_grid;         /* Draw the construction grid */
    uint8_t palette_rgba[256][4]; /* Optional indexed preview palette */
    int palette_valid;
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
void CadView_RotateRoll(CadView* view, double degrees);
void CadView_Pan3DVertical(CadView* view, double dy); /* Pan 3D view up/down relative to current angle */
/* Fit a finite axis-aligned world-space box inside the viewport.  The 3D
   camera returns to its home orientation; orthographic view orientation is
   unchanged. */
int CadView_FrameBounds(CadView* view,
                        double min_x, double min_y, double min_z,
                        double max_x, double max_y, double max_z,
                        int viewport_w, int viewport_h);
/* Fit bounds without changing the current 3D orbit orientation or roll.
   Orthographic views behave identically to CadView_FrameBounds. */
int CadView_FrameBoundsPreserveOrientation(
    CadView* view,
    double min_x, double min_y, double min_z,
    double max_x, double max_y, double max_z,
    int viewport_w, int viewport_h);
void CadView_SetPalette(CadView* view, const uint8_t* rgba256x4);
void CadView_ClearPalette(CadView* view);

/* ----------------------------------------------------------------------------
   3D to 2D projection
   ---------------------------------------------------------------------------- */
void CadView_ProjectPoint(const CadView* view, double x, double y, double z, 
                         int* out_x, int* out_y, int viewport_w, int viewport_h);

/* Extended projection used for depth sorting and near-plane clipping.
   Returns zero when a point is behind the 3D camera near plane. */
int CadView_ProjectPointDepth(const CadView* view, double x, double y, double z,
                              double* out_x, double* out_y, double* out_depth,
                              int viewport_w, int viewport_h);

/* Map the positive camera depth returned above to the normalized depth-buffer
   range.  Nearer geometry produces smaller values.  This is shared by the
   solid renderer and depth-aware editor overlays. */
double CadView_NormalizedDepth(double camera_depth);

/* ----------------------------------------------------------------------------
   Point selection (find nearest point to screen coordinates)
   Returns point index or -1 if none found within threshold
   ---------------------------------------------------------------------------- */
int16_t CadView_FindNearestPoint(const CadView* view, const CadCore* core,
                                 int screen_x, int screen_y,
                                 int viewport_x, int viewport_y,
                                 int viewport_w, int viewport_h,
                                 int threshold_pixels);

/* Pose-aware counterpart used by the editor.  Point IDs remain the stable
   static topology IDs, while projection and depth come exclusively from the
   immutable pose carried by scene. */
int16_t CadView_FindNearestScenePoint(const CadView* view,
                                      const CadScene* scene,
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

int CadView_FindScenePointsAtLocation(const CadView* view,
                                      const CadScene* scene,
                                      int screen_x, int screen_y,
                                      int viewport_x, int viewport_y,
                                      int viewport_w, int viewport_h,
                                      int threshold_pixels,
                                      double world_threshold,
                                      int16_t* out_indices, int max_count);

/* Find the nearest visible polygon edge/interior in screen space. */
int16_t CadView_FindNearestPolygon(const CadView* view, const CadCore* core,
                                   int screen_x, int screen_y,
                                   int viewport_x, int viewport_y,
                                   int viewport_w, int viewport_h,
                                   int threshold_pixels);

int16_t CadView_FindNearestScenePolygon(const CadView* view,
                                        const CadScene* scene,
                                        int screen_x, int screen_y,
                                        int viewport_x, int viewport_y,
                                        int viewport_w, int viewport_h,
                                        int threshold_pixels);

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

/* Render the same immutable pose used for picking and coordinate feedback.
   CadView_Render remains available for static/core-only callers. */
void CadView_RenderScene(const CadView* view, const CadScene* scene,
                         int pixel_x, int pixel_y, int pixel_w, int pixel_h,
                         int framebuffer_h, int logical_w, int logical_h);

