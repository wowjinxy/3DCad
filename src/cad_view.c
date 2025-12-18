#define _CRT_SECURE_NO_WARNINGS

#include "cad_view.h"
#include "render_gl.h"
#include <math.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ----------------------------------------------------------------------------
   View initialization
   ---------------------------------------------------------------------------- */

void CadView_Init(CadView* view, CadViewType type) {
    if (!view) return;
    view->type = type;
    view->zoom = 1.0;
    view->pan_x = 0.0;
    view->pan_y = 0.0;
    view->rot_x = 0.0;
    view->rot_y = 0.0;
    view->wireframe = 1; /* Default to wireframe */
}

void CadView_Reset(CadView* view) {
    if (!view) return;
    view->zoom = 1.0;
    view->pan_x = 0.0;
    view->pan_y = 0.0;
    view->rot_x = 0.0;
    view->rot_y = 0.0;
}

/* ----------------------------------------------------------------------------
   View transformations
   ---------------------------------------------------------------------------- */

void CadView_SetZoom(CadView* view, double zoom) {
    if (!view) return;
    if (zoom < 0.1) zoom = 0.1;
    if (zoom > 100.0) zoom = 100.0;
    view->zoom = zoom;
}

void CadView_Pan(CadView* view, double dx, double dy) {
    if (!view) return;
    view->pan_x += dx;
    view->pan_y += dy;
}

void CadView_Rotate(CadView* view, double dx, double dy) {
    if (!view) return;
    view->rot_x += dx;
    view->rot_y += dy;
    /* Clamp rotation */
    if (view->rot_x > 90.0) view->rot_x = 90.0;
    if (view->rot_x < -90.0) view->rot_x = -90.0;
}

/* ----------------------------------------------------------------------------
   3D to 2D projection
   ---------------------------------------------------------------------------- */

void CadView_ProjectPoint(const CadView* view, double x, double y, double z, 
                         int* out_x, int* out_y, int viewport_w, int viewport_h) {
    if (!view || !out_x || !out_y) return;
    
    double px, py, pz;
    
    /* Apply rotation for 3D view */
    if (view->type == CAD_VIEW_3D) {
        double rx = view->rot_x * M_PI / 180.0;
        double ry = view->rot_y * M_PI / 180.0;
        
        /* Rotate around X axis */
        double y1 = y * cos(rx) - z * sin(rx);
        double z1 = y * sin(rx) + z * cos(rx);
        
        /* Rotate around Y axis */
        px = x * cos(ry) + z1 * sin(ry);
        py = y1;
        pz = -x * sin(ry) + z1 * cos(ry);
    } else {
        /* Orthographic projections */
        switch (view->type) {
        case CAD_VIEW_TOP:
            px = x;
            py = -z;  /* Y becomes depth */
            pz = y;
            break;
        case CAD_VIEW_FRONT:
            /* Front view: looking from front, X is left/right, Y is up/down, Z is depth */
            px = x;  /* X is horizontal (left/right) */
            py = -y; /* Y is vertical (up/down) */
            pz = z;  /* Z is depth */
            break;
        case CAD_VIEW_RIGHT:
            /* Right view: looking from right, Z is forward/back, Y is up/down, X is depth */
            px = z;  /* Z becomes horizontal (forward/back) */
            py = -y; /* Y is vertical (up/down) */
            pz = -x; /* X is depth (negative because we're looking from right) */
            break;
        default:
            px = x;
            py = y;
            pz = z;
            break;
        }
    }
    
    /* Apply zoom and pan */
    px = px * view->zoom + view->pan_x;
    py = py * view->zoom + view->pan_y;
    
    /* Convert to viewport coordinates (center origin) */
    *out_x = (int)(viewport_w / 2 + px);
    *out_y = (int)(viewport_h / 2 - py); /* Flip Y for screen coordinates */
}

/* ----------------------------------------------------------------------------
   Rendering
   ---------------------------------------------------------------------------- */

void CadView_Render(const CadView* view, const CadCore* core, 
                    int viewport_x, int viewport_y, int viewport_w, int viewport_h, int win_h) {
    if (!view || !core) return;
    
    /* Set viewport */
    rg_set_viewport_tl(viewport_x, viewport_y, viewport_w, viewport_h, win_h);
    
    /* Clear background */
    RG_Color white = { 255, 255, 255, 255 };
    rg_fill_rect(0, 0, viewport_w, viewport_h, white);
    
    /* Draw grid (optional, for reference) */
    RG_Color grid = { 200, 200, 200, 255 };
    int center_x = viewport_w / 2;
    int center_y = viewport_h / 2;
    rg_line(0, center_y, viewport_w, center_y, grid);
    rg_line(center_x, 0, center_x, viewport_h, grid);
    
    /* Draw points and polygons */
    const CadFileData* data = &core->data;
    
    /* Draw polygons */
    RG_Color black = { 0, 0, 0, 255 }; /* Black for wireframe */
    
    for (int i = 0; i < data->polygonCount; i++) {
        CadPolygon* poly = CadCore_GetPolygon((CadCore*)core, i);
        if (!poly || poly->flags == 0) continue;
        
        /* Get first point */
        int16_t point_idx = poly->firstPoint;
        if (point_idx < 0 || point_idx >= CAD_MAX_POINTS) continue;
        
        CadPoint* first_pt = CadCore_GetPoint((CadCore*)core, point_idx);
        if (!first_pt) continue;
        
        int first_x, first_y;
        CadView_ProjectPoint(view, first_pt->pointx, first_pt->pointy, first_pt->pointz,
                            &first_x, &first_y, viewport_w, viewport_h);
        
        int prev_x = first_x;
        int prev_y = first_y;
        
        /* Draw polygon edges */
        int16_t current = point_idx;
        int count = 0;
        while (current >= 0 && current < CAD_MAX_POINTS && count < poly->npoints) {
            CadPoint* pt = CadCore_GetPoint((CadCore*)core, current);
            if (!pt) break;
            
            int x, y;
            CadView_ProjectPoint(view, pt->pointx, pt->pointy, pt->pointz,
                                &x, &y, viewport_w, viewport_h);
            
            /* Draw line from previous point */
            if (count > 0) {
                rg_line(prev_x, prev_y, x, y, black);
            }
            
            prev_x = x;
            prev_y = y;
            current = pt->nextPoint;
            count++;
        }
        
        /* Close polygon */
        if (count > 2) {
            rg_line(prev_x, prev_y, first_x, first_y, black);
        }
    }
    
    /* Draw selected points (highlight) */
    for (int i = 0; i < core->selection.pointCount; i++) {
        int16_t idx = core->selection.selectedPoints[i];
        if (idx < 0) continue;
        
        CadPoint* pt = CadCore_GetPoint((CadCore*)core, idx);
        if (!pt) continue;
        
        int x, y;
        CadView_ProjectPoint(view, pt->pointx, pt->pointy, pt->pointz,
                            &x, &y, viewport_w, viewport_h);
        
        /* Draw small circle/square for selected point */
        RG_Color red = { 255, 0, 0, 255 }; /* Red for selected */
        int size = 4;
        rg_fill_rect(x - size, y - size, size * 2, size * 2, red);
    }
}

