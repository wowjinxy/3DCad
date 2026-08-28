#define _CRT_SECURE_NO_WARNINGS

#include "cad_view.h"
#include "render_gl.h"

#include <SDL3/SDL_opengl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

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

void CadView_Pan3DVertical(CadView* view, double dy) {
    if (!view || view->type != CAD_VIEW_3D) return;
    
    /* Pan up/down relative to current rotation angle */
    /* Convert rotation to radians */
    double rx = view->rot_x * M_PI / 180.0;
    double ry = view->rot_y * M_PI / 180.0;
    
    /* Calculate up vector in world space based on rotation */
    /* The up vector in view space is (0, 1, 0), we need to rotate it */
    double up_x = -sin(ry) * sin(rx);
    double up_y = cos(rx);
    
    /* Apply panning along the up vector */
    /* Scale the movement and apply to pan */
    double pan_scale = 1.0 / view->zoom; /* Adjust for zoom level */
    view->pan_x += up_x * dy * pan_scale;
    view->pan_y += up_y * dy * pan_scale;
}

/* ----------------------------------------------------------------------------
   3D to 2D projection
   ---------------------------------------------------------------------------- */

void CadView_ProjectPoint(const CadView* view, double x, double y, double z, 
                         int* out_x, int* out_y, int viewport_w, int viewport_h) {
    if (!view || !out_x || !out_y) return;
    
    double px, py;
    
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
    } else {
        /* Orthographic projections */
        switch (view->type) {
        case CAD_VIEW_TOP:
            px = x;
            py = -z;  /* Y becomes depth */
            break;
        case CAD_VIEW_FRONT:
            /* Front view: looking from front, X is left/right, Y is up/down, Z is depth */
            px = x;  /* X is horizontal (left/right) */
            py = y;  /* Y is vertical (up/down, flipped) */
            break;
        case CAD_VIEW_RIGHT:
            /* Right view: looking from right, Z is forward/back, Y is up/down, X is depth */
            px = z;  /* Z becomes horizontal (forward/back) */
            py = y;  /* Y is vertical (up/down, flipped) */
            break;
        default:
            px = x;
            py = y;
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
                    int pixel_x, int pixel_y, int pixel_w, int pixel_h,
                    int framebuffer_h, int logical_w, int logical_h)
{
    if (!view || !core || pixel_w <= 0 || pixel_h <= 0 ||
        logical_w <= 0 || logical_h <= 0) return;

    /* -----------------------------
       Viewport + scissor (top-left UI coords)
       ----------------------------- */
    rg_set_viewport_tl(pixel_x, pixel_y, pixel_w, pixel_h, framebuffer_h,
                       logical_w, logical_h);

    glEnable(GL_SCISSOR_TEST);
    {
        int sc_y = framebuffer_h - (pixel_y + pixel_h);
        if (sc_y < 0) sc_y = 0;
        glScissor(pixel_x, sc_y, pixel_w, pixel_h);
    }

    /* -----------------------------
       2D pass: background + grid
       ----------------------------- */
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);

    RG_Color white = { 255, 255, 255, 255 };
    rg_fill_rect(0, 0, logical_w, logical_h, white);

    RG_Color grid = { 200, 200, 200, 255 };
    {
        int cx = logical_w / 2;
        int cy = logical_h / 2;
        rg_line(0, cy, logical_w, cy, grid);
        rg_line(cx, 0, cx, logical_h, grid);
    }

    const CadFileData* data = &core->data;

    /* -----------------------------
       Wireframe mode: just draw 2D edges and bail
       ----------------------------- */
    if (view->wireframe) {
        for (int i = 0; i < data->polygonCount; i++) {
            CadPolygon* poly = CadCore_GetPolygon((CadCore*)core, (int16_t)i);
            if (!poly || poly->flags == 0) continue;

            int16_t point_idx = poly->firstPoint;
            if (point_idx < 0 || point_idx >= CAD_MAX_POINTS) continue;
            if (poly->npoints < 2) continue;

            #define MAX_STACK_POINTS 64
            int stack_x[MAX_STACK_POINTS];
            int stack_y[MAX_STACK_POINTS];
            int* x_coords = NULL;
            int* y_coords = NULL;

            int npoints = poly->npoints;
            if (npoints > 256) npoints = 256;

            if (npoints <= MAX_STACK_POINTS) {
                x_coords = stack_x;
                y_coords = stack_y;
            } else {
                x_coords = (int*)calloc(npoints, sizeof(int));
                y_coords = (int*)calloc(npoints, sizeof(int));
                if (!x_coords || !y_coords) {
                    if (x_coords) free(x_coords);
                    if (y_coords) free(y_coords);
                    continue;
                }
            }

            int16_t current = point_idx;
            int count = 0;
            int16_t visited[64]; /* Track visited points to detect cycles (smaller array) */
            int visited_count = 0;
            
            while (current >= 0 && current < CAD_MAX_POINTS && count < npoints) {
                /* Check for cycles (only check if we have room to track) */
                if (visited_count < 64) {
                    int already_visited = 0;
                    for (int v = 0; v < visited_count; v++) {
                        if (visited[v] == current) {
                            already_visited = 1;
                            break;
                        }
                    }
                    if (already_visited) break;
                    visited[visited_count++] = current;
                }
                
                CadPoint* pt = CadCore_GetPoint((CadCore*)core, current);
                if (!pt || pt->flags == 0) break; /* Invalid point */

                CadView_ProjectPoint(view, pt->pointx, pt->pointy, pt->pointz,
                                     &x_coords[count], &y_coords[count], logical_w, logical_h);

                current = pt->nextPoint;
                count++;
                if (count > 1000) break; /* Safety limit */
            }

            if (count >= 2) {
                RG_Color black = { 0, 0, 0, 255 };
                for (int j = 0; j < count; j++) {
                    int next = (j + 1) % count;
                    rg_line(x_coords[j], y_coords[j], x_coords[next], y_coords[next], black);
                }
            }

            if (npoints > MAX_STACK_POINTS) {
                free(x_coords);
                free(y_coords);
            }
        }

        /* Render all points: selected = red, orphaned = blue */
        for (int i = 0; i < core->data.pointCount; i++) {
            CadPoint* pt = CadCore_GetPoint((CadCore*)core, (int16_t)i);
            if (!pt || pt->flags == 0) continue;
            
            /* Check if point is selected */
            int is_selected = CadCore_IsPointSelected((CadCore*)core, (int16_t)i);
            
            /* Check if point is connected to any polygon */
            int is_connected = CadCore_IsPointConnected((CadCore*)core, (int16_t)i);
            
            /* Only render selected points (red) or orphaned points (blue) */
            if (is_selected || !is_connected) {
                int x, y;
                CadView_ProjectPoint(view, pt->pointx, pt->pointy, pt->pointz,
                                     &x, &y, logical_w, logical_h);
                
                RG_Color color;
                if (is_selected) {
                    color = (RG_Color){ 255, 0, 0, 255 }; /* Red for selected */
                } else {
                    color = (RG_Color){ 0, 0, 255, 255 }; /* Blue for orphaned */
                }
                
                int size = 4;
                rg_fill_rect(x - size, y - size, size * 2, size * 2, color);
            }
        }

        return;
    }

    /* -----------------------------
       3D pass setup (solid mode)
       IMPORTANT: establish clean GL state ONCE per viewport.
       ----------------------------- */

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    {
        const double depth_range = 10000.0;
        glOrtho(-logical_w / 2.0, logical_w / 2.0,
                -logical_h / 2.0, logical_h / 2.0,
                -depth_range, depth_range);
    }

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    /* Depth for this viewport only (scissor is enabled) */
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);

    /* Lighting baseline */
    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_CULL_FACE); /* keep off unless you guarantee winding */

    glEnable(GL_LIGHTING);
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
    glEnable(GL_LIGHT0);
    glShadeModel(GL_SMOOTH);

    /* Keep per-vertex glColor affecting diffuse/ambient */
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);

    /* Key fix for stability when any scaling sneaks in */
    glEnable(GL_NORMALIZE);

    /* Light parameters (directional) */
    {
        float light_pos[4]      = { 1.0f, 1.0f, 1.0f, 0.0f };
        float light_ambient[4]  = { 0.30f, 0.30f, 0.30f, 1.0f };
        float light_diffuse[4]  = { 0.80f, 0.80f, 0.80f, 1.0f };
        float light_specular[4] = { 0.00f, 0.00f, 0.00f, 1.0f }; /* spec off unless you set shininess */
        glLightfv(GL_LIGHT0, GL_POSITION, light_pos);
        glLightfv(GL_LIGHT0, GL_AMBIENT,  light_ambient);
        glLightfv(GL_LIGHT0, GL_DIFFUSE,  light_diffuse);
        glLightfv(GL_LIGHT0, GL_SPECULAR, light_specular);
    }

    /* Polygon color: #AAAAAA */
    RG_Color poly_gray = { 0xAA, 0xAA, 0xAA, 255 };
    RG_Color edge_color = { 0x66, 0x66, 0x66, 255 };

    /* -----------------------------
       Draw polygons (solid)
       ----------------------------- */
    for (int i = 0; i < data->polygonCount; i++) {
        CadPolygon* poly = CadCore_GetPolygon((CadCore*)core, (int16_t)i);
        if (!poly || poly->flags == 0) continue;

        int16_t point_idx = poly->firstPoint;
        if (point_idx < 0 || point_idx >= CAD_MAX_POINTS) continue;
        if (poly->npoints < 2) continue;

        #define MAX_STACK_POINTS 64
        int    stack_x[MAX_STACK_POINTS];
        int    stack_y[MAX_STACK_POINTS];
        double stack_z[MAX_STACK_POINTS];

        int*    x_coords = NULL;
        int*    y_coords = NULL;
        double* z_coords = NULL;

        int npoints = poly->npoints;
        if (npoints > 256) npoints = 256;

        if (npoints <= MAX_STACK_POINTS) {
            x_coords = stack_x;
            y_coords = stack_y;
            z_coords = stack_z;
        } else {
            x_coords = (int*)calloc(npoints, sizeof(int));
            y_coords = (int*)calloc(npoints, sizeof(int));
            z_coords = (double*)calloc(npoints, sizeof(double));
            if (!x_coords || !y_coords || !z_coords) {
                if (x_coords) free(x_coords);
                if (y_coords) free(y_coords);
                if (z_coords) free(z_coords);
                continue;
            }
        }

        int16_t current = point_idx;
        int count = 0;
        int16_t visited[64]; /* Track visited points to detect cycles (smaller array) */
        int visited_count = 0;

        while (current >= 0 && current < CAD_MAX_POINTS && count < npoints) {
            /* Check for cycles (only check if we have room to track) */
            if (visited_count < 64) {
                int already_visited = 0;
                for (int v = 0; v < visited_count; v++) {
                    if (visited[v] == current) {
                        already_visited = 1;
                        break;
                    }
                }
                if (already_visited) break;
                visited[visited_count++] = current;
            }
            
            CadPoint* pt = CadCore_GetPoint((CadCore*)core, current);
            if (!pt || pt->flags == 0) break; /* Invalid point */

            CadView_ProjectPoint(view, pt->pointx, pt->pointy, pt->pointz,
                                 &x_coords[count], &y_coords[count], logical_w, logical_h);

            /* View-space-ish depth (your existing approach) */
            double pz;
            if (view->type == CAD_VIEW_3D) {
                double rx = view->rot_x * M_PI / 180.0;
                double ry = view->rot_y * M_PI / 180.0;
                double z1 = pt->pointy * sin(rx) + pt->pointz * cos(rx);
                pz = -pt->pointx * sin(ry) + z1 * cos(ry);
            } else {
                switch (view->type) {
                case CAD_VIEW_TOP:   pz = pt->pointy;  break;
                case CAD_VIEW_FRONT: pz = pt->pointz;  break;
                case CAD_VIEW_RIGHT: pz = -pt->pointx; break;
                default:             pz = pt->pointz;  break;
                }
            }

            z_coords[count] = pz;

            current = pt->nextPoint;
            count++;
            if (count > 1000) break;
        }

        if (count == 2) {
            /* 2-point faces are wireframe edges - render as lines */
            glDisable(GL_LIGHTING);
            glColor4ub(edge_color.r, edge_color.g, edge_color.b, edge_color.a);
            
            glBegin(GL_LINES);
            for (int j = 0; j < count; j++) {
                int gl_y = logical_h - y_coords[j];
                glVertex3d((double)x_coords[j] - logical_w / 2.0,
                           (double)gl_y - logical_h / 2.0,
                           z_coords[j]);
            }
            glEnd();
        } else if (count >= 3) {
            /* Compute normal in the SAME space as the vertices we draw (fixes �weird shading�) */
            double nx = 0.0, ny = 0.0, nz = 1.0;
            {
                double x1 = (double)x_coords[0] - logical_w / 2.0;
                double y1 = (double)(logical_h - y_coords[0]) - logical_h / 2.0;
                double z1 = z_coords[0];

                double x2 = (double)x_coords[1] - logical_w / 2.0;
                double y2 = (double)(logical_h - y_coords[1]) - logical_h / 2.0;
                double z2 = z_coords[1];

                double x3 = (double)x_coords[2] - logical_w / 2.0;
                double y3 = (double)(logical_h - y_coords[2]) - logical_h / 2.0;
                double z3 = z_coords[2];

                double v1x = x2 - x1, v1y = y2 - y1, v1z = z2 - z1;
                double v2x = x3 - x1, v2y = y3 - y1, v2z = z3 - z1;

                nx = v1y * v2z - v1z * v2y;
                ny = v1z * v2x - v1x * v2z;
                nz = v1x * v2y - v1y * v2x;

                double len = sqrt(nx*nx + ny*ny + nz*nz);
                if (len > 1e-9) { nx /= len; ny /= len; nz /= len; }
                else { nx = 0.0; ny = 0.0; nz = 1.0; }
            }

            glNormal3d(nx, ny, nz);
            glColor4ub(poly_gray.r, poly_gray.g, poly_gray.b, poly_gray.a);

            glBegin(GL_POLYGON);
            for (int j = 0; j < count; j++) {
                int gl_y = logical_h - y_coords[j];
                glVertex3d((double)x_coords[j] - logical_w / 2.0,
                           (double)gl_y - logical_h / 2.0,
                           z_coords[j]);
            }
            glEnd();

            /* Outline (unlit) */
            glDisable(GL_LIGHTING);
            glColor4ub(edge_color.r, edge_color.g, edge_color.b, edge_color.a);

            glBegin(GL_LINES);
            for (int j = 0; j < count; j++) {
                int next = (j + 1) % count;
                int gl_y1 = logical_h - y_coords[j];
                int gl_y2 = logical_h - y_coords[next];

                glVertex3d((double)x_coords[j] - logical_w / 2.0,
                           (double)gl_y1 - logical_h / 2.0,
                           z_coords[j]);

                glVertex3d((double)x_coords[next] - logical_w / 2.0,
                           (double)gl_y2 - logical_h / 2.0,
                           z_coords[next]);
            }
            glEnd();

            glEnable(GL_LIGHTING);
        }

        if (npoints > MAX_STACK_POINTS) {
            free(x_coords);
            free(y_coords);
            free(z_coords);
        }
    }

    /* -----------------------------
       Selected points (2D overlay)
       ----------------------------- */
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);

    /* Set up 2D projection for overlay drawing */
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0.0, (double)logical_w, (double)logical_h, 0.0, -1.0, 1.0);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    /* Render all points: selected = red, orphaned = blue */
    for (int i = 0; i < core->data.pointCount; i++) {
        CadPoint* pt = CadCore_GetPoint((CadCore*)core, (int16_t)i);
        if (!pt || pt->flags == 0) continue;
        
        /* Check if point is selected */
        int is_selected = CadCore_IsPointSelected((CadCore*)core, (int16_t)i);
        
        /* Check if point is connected to any polygon */
        int is_connected = CadCore_IsPointConnected((CadCore*)core, (int16_t)i);
        
        /* Only render selected points (red) or orphaned points (blue) */
        if (is_selected || !is_connected) {
            int x, y;
            CadView_ProjectPoint(view, pt->pointx, pt->pointy, pt->pointz,
                                 &x, &y, logical_w, logical_h);
            
            RG_Color color;
            if (is_selected) {
                color = (RG_Color){ 255, 0, 0, 255 }; /* Red for selected */
            } else {
                color = (RG_Color){ 0, 0, 255, 255 }; /* Blue for orphaned */
            }
            
            int size = 4;
            /* Draw square at projected point location */
            rg_fill_rect(x - size, y - size, size * 2, size * 2, color);
        }
    }

    /* Restore matrices */
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();

    /* Keep scissor clipped to viewport */
    glEnable(GL_SCISSOR_TEST);
    {
        int sc_y = framebuffer_h - (pixel_y + pixel_h);
        if (sc_y < 0) sc_y = 0;
        glScissor(pixel_x, sc_y, pixel_w, pixel_h);
    }
}

/* ----------------------------------------------------------------------------
   Point selection - find nearest point to screen coordinates
   ---------------------------------------------------------------------------- */

int16_t CadView_FindNearestPoint(const CadView* view, const CadCore* core,
                                 int screen_x, int screen_y,
                                 int viewport_x, int viewport_y,
                                 int viewport_w, int viewport_h,
                                 int threshold_pixels) {
    if (!view || !core) return -1;
    
    /* Convert screen coordinates to viewport-relative coordinates */
    int vp_x = screen_x - viewport_x;
    int vp_y = screen_y - viewport_y;
    
    /* Check if click is within viewport */
    if (vp_x < 0 || vp_x >= viewport_w || vp_y < 0 || vp_y >= viewport_h) {
        return -1;
    }
    
    /* Find nearest point by projecting all points and finding closest in screen space */
    int16_t nearest_idx = -1;
    double nearest_dist_sq = (double)(threshold_pixels * threshold_pixels);
    
    for (int i = 0; i < core->data.pointCount && i < CAD_MAX_POINTS; i++) {
        const CadPoint* pt = &core->data.points[i];
        if (pt->flags == 0) continue; /* Skip invalid points */
        
        /* Project point to screen coordinates (CadView_ProjectPoint already applies zoom/pan) */
        int proj_x, proj_y;
        CadView_ProjectPoint(view, pt->pointx, pt->pointy, pt->pointz, 
                            &proj_x, &proj_y, viewport_w, viewport_h);
        
        /* Calculate distance squared in screen space (viewport-relative) */
        double dx = (double)vp_x - (double)proj_x;
        double dy = (double)vp_y - (double)proj_y;
        double dist_sq = dx * dx + dy * dy;
        
        if (dist_sq < nearest_dist_sq) {
            nearest_dist_sq = dist_sq;
            nearest_idx = (int16_t)i;
        }
    }
    
    return nearest_idx;
}

/* ----------------------------------------------------------------------------
   Find all points at the same location as the nearest point
   ---------------------------------------------------------------------------- */

int CadView_FindPointsAtLocation(const CadView* view, const CadCore* core,
                                 int screen_x, int screen_y,
                                 int viewport_x, int viewport_y,
                                 int viewport_w, int viewport_h,
                                 int threshold_pixels,
                                 double world_threshold,
                                 int16_t* out_indices, int max_count) {
    if (!view || !core || !out_indices || max_count <= 0) return 0;
    
    /* First, find the nearest point */
    int16_t nearest_idx = CadView_FindNearestPoint(view, core, screen_x, screen_y,
                                                   viewport_x, viewport_y,
                                                   viewport_w, viewport_h,
                                                   threshold_pixels);
    
    if (nearest_idx < 0) return 0;
    
    /* Get the world coordinates of the nearest point */
    const CadPoint* nearest_pt = &core->data.points[nearest_idx];
    if (nearest_pt->flags == 0) return 0;
    
    double ref_x = nearest_pt->pointx;
    double ref_y = nearest_pt->pointy;
    double ref_z = nearest_pt->pointz;
    
    /* Find all points within world_threshold distance of this point */
    int count = 0;
    for (int i = 0; i < core->data.pointCount && i < CAD_MAX_POINTS && count < max_count; i++) {
        const CadPoint* pt = &core->data.points[i];
        if (pt->flags == 0) continue; /* Skip invalid points */
        
        /* Calculate 3D distance */
        double dx = pt->pointx - ref_x;
        double dy = pt->pointy - ref_y;
        double dz = pt->pointz - ref_z;
        double dist_sq = dx * dx + dy * dy + dz * dz;
        
        if (dist_sq <= world_threshold * world_threshold) {
            out_indices[count++] = (int16_t)i;
        }
    }
    
    return count;
}

/* ----------------------------------------------------------------------------
   Unproject screen delta to 3D world delta
   ---------------------------------------------------------------------------- */

void CadView_UnprojectDelta(const CadView* view, int screen_dx, int screen_dy,
                            int viewport_w, int viewport_h,
                            double* out_dx, double* out_dy, double* out_dz) {
    if (!view || !out_dx || !out_dy || !out_dz) return;

    (void)viewport_w;
    (void)viewport_h;
    
    /* Convert screen delta to viewport space (accounting for zoom) */
    double vp_dx = (double)screen_dx / view->zoom;
    double vp_dy = -(double)screen_dy / view->zoom; /* Flip Y for screen to world */
    
    /* Convert viewport delta to world delta based on view type */
    if (view->type == CAD_VIEW_3D) {
        /* For 3D view, we need to apply inverse rotation */
        /* This is complex - for now, move in screen X/Y plane */
        /* In a full implementation, you'd project onto the view plane */
        double rx = view->rot_x * M_PI / 180.0;
        double ry = view->rot_y * M_PI / 180.0;
        
        /* Approximate: move in the plane perpendicular to view direction */
        /* Simplified: move in rotated X/Y plane */
        *out_dx = vp_dx * cos(-ry);
        *out_dy = vp_dx * sin(-ry) * sin(-rx) + vp_dy * cos(-rx);
        *out_dz = vp_dx * sin(-ry) * cos(-rx) - vp_dy * sin(-rx);
    } else {
        /* Orthographic projections - constrain to view plane */
        switch (view->type) {
        case CAD_VIEW_TOP:
            /* Top view: screen X/Y maps to world X/Z, Y stays constant */
            *out_dx = vp_dx;
            *out_dy = 0.0;  /* Y doesn't change in top view */
            *out_dz = -vp_dy;  /* Screen Y is world -Z */
            break;
        case CAD_VIEW_FRONT:
            /* Front view: screen X/Y maps to world X/Y, Z stays constant */
            *out_dx = vp_dx;
            *out_dy = vp_dy;
            *out_dz = 0.0;  /* Z doesn't change in front view */
            break;
        case CAD_VIEW_RIGHT:
            /* Right view: screen X/Y maps to world Z/Y, X stays constant */
            *out_dx = 0.0;  /* X doesn't change in right view */
            *out_dy = vp_dy;
            *out_dz = vp_dx;  /* Screen X is world Z */
            break;
        default:
            *out_dx = vp_dx;
            *out_dy = vp_dy;
            *out_dz = 0.0;
            break;
        }
    }
}

/* ----------------------------------------------------------------------------
   Unproject screen point to 3D world coordinates
   ---------------------------------------------------------------------------- */

void CadView_UnprojectPoint(const CadView* view, int screen_x, int screen_y,
                            int viewport_w, int viewport_h,
                            double* out_x, double* out_y, double* out_z) {
    if (!view || !out_x || !out_y || !out_z) return;
    
    /* Convert screen coordinates to viewport space (centered, accounting for zoom and pan) */
    double vp_x = ((double)screen_x - (double)viewport_w / 2.0) / view->zoom - view->pan_x;
    double vp_y = -((double)screen_y - (double)viewport_h / 2.0) / view->zoom - view->pan_y; /* Flip Y */
    
    /* Convert viewport coordinates to world coordinates based on view type */
    if (view->type == CAD_VIEW_3D) {
        /* For 3D view, we project onto the view plane (z=0 in view space) */
        /* This is a simplified projection - in a full implementation you'd use proper ray casting */
        double rx = view->rot_x * M_PI / 180.0;
        double ry = view->rot_y * M_PI / 180.0;
        
        /* Approximate: treat as if viewing from above with rotation */
        /* Reverse the rotation to get world coordinates */
        *out_x = vp_x * cos(ry) - vp_y * sin(rx) * sin(ry);
        *out_y = vp_y * cos(rx);
        *out_z = vp_x * sin(ry) + vp_y * sin(rx) * cos(ry);
    } else {
        /* Orthographic projections - reverse the projection */
        switch (view->type) {
        case CAD_VIEW_TOP:
            /* Top view: viewport X/Y maps to world X/Z, Y is constant (0) */
            *out_x = vp_x;
            *out_y = 0.0;  /* Y doesn't change in top view */
            *out_z = -vp_y;  /* Screen Y is world -Z */
            break;
        case CAD_VIEW_FRONT:
            /* Front view: viewport X/Y maps to world X/Y, Z is constant (0) */
            *out_x = vp_x;
            *out_y = -vp_y;  /* Screen Y is world -Y */
            *out_z = 0.0;  /* Z doesn't change in front view */
            break;
        case CAD_VIEW_RIGHT:
            /* Right view: viewport X/Y maps to world Z/Y, X is constant (0) */
            *out_x = 0.0;  /* X doesn't change in right view */
            *out_y = -vp_y;  /* Screen Y is world -Y */
            *out_z = vp_x;  /* Screen X is world Z */
            break;
        default:
            *out_x = vp_x;
            *out_y = vp_y;
            *out_z = 0.0;
            break;
        }
    }
}
