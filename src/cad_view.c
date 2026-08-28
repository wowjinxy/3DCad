#define _CRT_SECURE_NO_WARNINGS

#include "cad_view.h"
#include "render_gl.h"

#include <SDL3/SDL_opengl.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define CAD_NEAR_PLANE 16.0
#define CAD_GRID_EXTENT 512.0
#define CAD_MAX_PROJECTED_FACE_POINTS (CAD_MAX_FACE_POINTS + 2)
#define CAD_SCREEN_COORD_MARGIN 32

typedef struct ProjectedPolygon {
    int16_t index;
    int count;
    double x[CAD_MAX_PROJECTED_FACE_POINTS];
    double y[CAD_MAX_PROJECTED_FACE_POINTS];
    double vx[CAD_MAX_PROJECTED_FACE_POINTS];
    double vy[CAD_MAX_PROJECTED_FACE_POINTS];
    double vz[CAD_MAX_PROJECTED_FACE_POINTS];
    double camera_depth[CAD_MAX_PROJECTED_FACE_POINTS];
    double depth;
    int selected;
} ProjectedPolygon;

typedef struct CadViewGeometry {
    const CadFileData* data;
    const CadPose* pose;
} CadViewGeometry;

/* OpenGL rendering is single-threaded in the SDL frontend.  Reusing one BSS
   scratch array avoids allocating roughly half a megabyte four times every
   frame without putting that storage on the small Windows thread stack. */
static ProjectedPolygon cad_polygon_scratch[CAD_MAX_POLYGONS];

static int cad_geometry_from_core(const CadCore* core,
                                  CadViewGeometry* geometry)
{
    if (!core || !geometry) return 0;
    geometry->data = &core->data;
    geometry->pose = NULL;
    return 1;
}

static int cad_geometry_from_scene(const CadScene* scene,
                                   CadViewGeometry* geometry)
{
    if (!scene || !scene->topology || !scene->pose || !geometry) return 0;
    geometry->data = scene->topology;
    geometry->pose = scene->pose;
    return 1;
}

static int cad_geometry_point(const CadViewGeometry* geometry,
                              int16_t point_index,
                              CadPosition* position)
{
    const CadPoint* point;
    if (!geometry || !geometry->data || !position || point_index < 0 ||
        point_index >= CAD_MAX_POINTS) return 0;
    point = &geometry->data->points[point_index];
    if (!point->flags) return 0;
    if (geometry->pose) {
        if (!geometry->pose->pointValid[point_index]) return 0;
        *position = geometry->pose->points[point_index];
    } else {
        position->x = point->pointx;
        position->y = point->pointy;
        position->z = point->pointz;
    }
    return isfinite(position->x) && isfinite(position->y) &&
           isfinite(position->z);
}

static int cad_geometry_point_selected(const CadViewGeometry* geometry,
                                       int16_t point_index)
{
    return geometry && geometry->data && point_index >= 0 &&
           point_index < CAD_MAX_POINTS &&
           geometry->data->points[point_index].flags &&
           geometry->data->points[point_index].selectFlag != 0;
}

static int cad_geometry_polygon_selected(const CadViewGeometry* geometry,
                                         int16_t polygon_index)
{
    return geometry && geometry->data && polygon_index >= 0 &&
           polygon_index < CAD_MAX_POLYGONS &&
           geometry->data->polygons[polygon_index].flags &&
           geometry->data->polygons[polygon_index].selectFlag != 0;
}

static int cad_view_parameters_valid(const CadView* view)
{
    if (!view || view->type < CAD_VIEW_TOP || view->type > CAD_VIEW_3D ||
        !isfinite(view->zoom) || view->zoom <= 0.0 ||
        !isfinite(view->pan_x) || !isfinite(view->pan_y)) return 0;
    if (view->type == CAD_VIEW_3D &&
        (!isfinite(view->camera_distance) ||
         !isfinite(view->focal_length) || view->focal_length <= 0.0 ||
         !isfinite(view->rot_x) || !isfinite(view->rot_y) ||
         !isfinite(view->rot_z))) return 0;
    return 1;
}

/* Every projected coordinate eventually reaches integer-based GUI drawing.
   Keep a small margin so handle/outlining arithmetic cannot overflow either. */
static int cad_screen_coordinate_valid(double value)
{
    return isfinite(value) &&
           value >= (double)INT_MIN + CAD_SCREEN_COORD_MARGIN &&
           value <= (double)INT_MAX - CAD_SCREEN_COORD_MARGIN;
}

static int cad_round_screen_coordinate(double value, int* output)
{
    if (!output || !cad_screen_coordinate_valid(value)) return 0;
    *output = (int)lround(value);
    return 1;
}

static int cad_finalize_screen_point(const CadView* view, double px, double py,
                                     int width, int height,
                                     double* out_x, double* out_y)
{
    double screen_x, screen_y;
    if (!view || !out_x || !out_y || !isfinite(px) || !isfinite(py)) return 0;
    screen_x = width * 0.5 + px;
    screen_y = height * 0.5 - py;
    if (!isfinite(screen_x) || !isfinite(screen_y)) return 0;
    screen_x += view->pan_x;
    screen_y -= view->pan_y;
    if (!cad_screen_coordinate_valid(screen_x) ||
        !cad_screen_coordinate_valid(screen_y)) return 0;
    *out_x = screen_x;
    *out_y = screen_y;
    return 1;
}

static int cad_project_camera_point(const CadView* view,
                                    double vx, double vy, double vz,
                                    int width, int height,
                                    double* out_x, double* out_y,
                                    double* out_depth)
{
    double depth, px, py;
    if (!cad_view_parameters_valid(view) || view->type != CAD_VIEW_3D ||
        !out_depth || !isfinite(vx) || !isfinite(vy) || !isfinite(vz)) return 0;
    depth = view->camera_distance + vz;
    if (!isfinite(depth) || depth < CAD_NEAR_PLANE) return 0;

    /* Divide before multiplying by focal length.  This preserves useful
       projections such as DBL_MAX / DBL_MAX while avoiding an unnecessary
       DBL_MAX * focal_length overflow. */
    px = vx / depth;
    py = vy / depth;
    if (!isfinite(px) || !isfinite(py)) return 0;
    px *= view->focal_length;
    py *= view->focal_length;
    if (!isfinite(px) || !isfinite(py)) return 0;
    px *= view->zoom;
    py *= view->zoom;
    if (!isfinite(px) || !isfinite(py) ||
        !cad_finalize_screen_point(view, px, py, width, height,
                                   out_x, out_y)) return 0;
    *out_depth = depth;
    return 1;
}

static int cad_project_orthographic_point(const CadView* view,
                                          double vx, double vy, double depth,
                                          int width, int height,
                                          double* out_x, double* out_y,
                                          double* out_depth)
{
    if (!cad_view_parameters_valid(view) || view->type == CAD_VIEW_3D ||
        !out_depth || !isfinite(vx) || !isfinite(vy) || !isfinite(depth)) return 0;
    vx *= view->zoom;
    vy *= view->zoom;
    if (!isfinite(vx) || !isfinite(vy) ||
        !cad_finalize_screen_point(view, vx, vy, width, height,
                                   out_x, out_y)) return 0;
    *out_depth = depth;
    return 1;
}

/* Stable half-space intersection for finite values near DBL_MAX. */
static int cad_clip_intersection_t(double start, double end, double plane,
                                   double* out_t)
{
    double scale, scaled_start, scaled_end, scaled_plane, denominator, t;
    if (!out_t || !isfinite(start) || !isfinite(end) || !isfinite(plane)) return 0;
    scale = fmax(fabs(start), fmax(fabs(end), fabs(plane)));
    if (!isfinite(scale) || scale == 0.0) return 0;
    scaled_start = start / scale;
    scaled_end = end / scale;
    scaled_plane = plane / scale;
    denominator = scaled_end - scaled_start;
    if (!isfinite(denominator) || denominator == 0.0) return 0;
    t = (scaled_plane - scaled_start) / denominator;
    if (!isfinite(t) || t < -1e-12 || t > 1.0 + 1e-12) return 0;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    *out_t = t;
    return 1;
}

static int cad_lerp_finite(double start, double end, double t, double* output)
{
    double value;
    if (!output || !isfinite(start) || !isfinite(end) ||
        !isfinite(t) || t < 0.0 || t > 1.0) return 0;
    value = start * (1.0 - t) + end * t;
    if (!isfinite(value)) return 0;
    *output = value;
    return 1;
}

static double cad_degrees(double value)
{
    return fmod(value, 360.0) * M_PI / 180.0;
}

static void cad_default_camera(CadView* view)
{
    view->zoom = 1.0;
    view->pan_x = 0.0;
    view->pan_y = 0.0;
    view->rot_x = view->type == CAD_VIEW_3D ? -20.0 : 0.0;
    view->rot_y = view->type == CAD_VIEW_3D ? 30.0 : 0.0;
    view->rot_z = 0.0;
    view->camera_distance = 512.0;
    view->focal_length = 512.0;
}

static void cad_rotate_to_view(const CadView* view, double x, double y, double z,
                               double* out_x, double* out_y, double* out_z)
{
    const double rx = cad_degrees(view->rot_x);
    const double ry = cad_degrees(view->rot_y);
    const double rz = cad_degrees(view->rot_z);
    const double sx = sin(rx), cx = cos(rx);
    const double sy = sin(ry), cy = cos(ry);
    const double sz = sin(rz), cz = cos(rz);
    /* World-Y yaw first keeps the ground plane/world-up axis stable while
       orbiting.  Camera-X pitch follows, then optional camera roll. */
    const double x1 = x * cy + z * sy;
    const double z1 = -x * sy + z * cy;
    const double y2 = y * cx - z1 * sx;
    const double z2 = y * sx + z1 * cx;

    *out_x = x1 * cz - y2 * sz;
    *out_y = x1 * sz + y2 * cz;
    *out_z = z2;
}

static void cad_rotate_from_view(const CadView* view, double x, double y, double z,
                                 double* out_x, double* out_y, double* out_z)
{
    const double rx = -cad_degrees(view->rot_x);
    const double ry = -cad_degrees(view->rot_y);
    const double rz = -cad_degrees(view->rot_z);
    const double sz = sin(rz), cz = cos(rz);
    const double sy = sin(ry), cy = cos(ry);
    const double sx = sin(rx), cx = cos(rx);
    const double x1 = x * cz - y * sz;
    const double y1 = x * sz + y * cz;
    const double y2 = y1 * cx - z * sx;
    const double z2 = y1 * sx + z * cx;

    /* Exact inverse of Rz * Rx * Ry: undo roll, pitch, then yaw. */
    *out_x = x1 * cy + z2 * sy;
    *out_y = y2;
    *out_z = -x1 * sy + z2 * cy;
}

static void cad_orthographic_components(CadViewType type, double x, double y, double z,
                                        double* out_x, double* out_y, double* out_depth)
{
    switch (type) {
    case CAD_VIEW_TOP:
        *out_x = x;
        *out_y = -z;
        *out_depth = y;
        break;
    case CAD_VIEW_FRONT:
        *out_x = x;
        *out_y = y;
        *out_depth = z;
        break;
    case CAD_VIEW_RIGHT:
        *out_x = z;
        *out_y = y;
        *out_depth = -x;
        break;
    default:
        *out_x = x;
        *out_y = y;
        *out_depth = z;
        break;
    }
}

void CadView_Init(CadView* view, CadViewType type)
{
    if (!view) return;
    view->type = type;
    view->wireframe = 1;
    view->show_grid = 1;
    memset(view->palette_rgba, 0, sizeof(view->palette_rgba));
    view->palette_valid = 0;
    cad_default_camera(view);
}

void CadView_Reset(CadView* view)
{
    if (!view) return;
    cad_default_camera(view);
}

void CadView_SetZoom(CadView* view, double zoom)
{
    if (!view || !isfinite(zoom)) return;
    if (zoom < 0.2) zoom = 0.2;
    if (zoom > 8.0) zoom = 8.0;
    view->zoom = zoom;
}

void CadView_Pan(CadView* view, double dx, double dy)
{
    double pan_x, pan_y;
    if (!view || !isfinite(dx) || !isfinite(dy)) return;
    pan_x = view->pan_x + dx;
    pan_y = view->pan_y + dy;
    if (!isfinite(pan_x) || !isfinite(pan_y)) return;
    view->pan_x = pan_x;
    view->pan_y = pan_y;
}

void CadView_Rotate(CadView* view, double dx, double dy)
{
    double rot_x, rot_y;
    if (!view || view->type != CAD_VIEW_3D ||
        !isfinite(dx) || !isfinite(dy)) return;
    rot_x = view->rot_x + dy;
    if (!isfinite(rot_x)) return;
    if (rot_x > 89.0) rot_x = 89.0;
    if (rot_x < -89.0) rot_x = -89.0;
    rot_y = fmod(view->rot_y + fmod(dx, 360.0), 360.0);
    if (!isfinite(rot_y)) return;
    if (rot_y < 0.0) rot_y += 360.0;
    view->rot_x = rot_x;
    view->rot_y = rot_y;
}

void CadView_RotateRoll(CadView* view, double degrees)
{
    double rotation;
    if (!view || view->type != CAD_VIEW_3D || !isfinite(degrees)) return;
    rotation = fmod(view->rot_z + fmod(degrees, 360.0), 360.0);
    if (!isfinite(rotation)) return;
    if (rotation < 0.0) rotation += 360.0;
    view->rot_z = rotation;
}

void CadView_Pan3DVertical(CadView* view, double dy)
{
    double pan_y;
    if (!view || view->type != CAD_VIEW_3D || !isfinite(dy)) return;
    pan_y = view->pan_y + dy;
    if (!isfinite(pan_y)) return;
    view->pan_y = pan_y;
}

static int cad_frame_bounds(CadView* view,
                            double min_x, double min_y, double min_z,
                            double max_x, double max_y, double max_z,
                            int viewport_w, int viewport_h,
                            int preserve_orientation)
{
    const double padding = 0.82;
    double center_x, center_y, center_z;
    double span_x, span_y;
    double zoom;
    if (!view || viewport_w <= 0 || viewport_h <= 0 ||
        !isfinite(min_x) || !isfinite(min_y) || !isfinite(min_z) ||
        !isfinite(max_x) || !isfinite(max_y) || !isfinite(max_z) ||
        min_x > max_x || min_y > max_y || min_z > max_z) return 0;

    center_x = min_x + (max_x - min_x) * 0.5;
    center_y = min_y + (max_y - min_y) * 0.5;
    center_z = min_z + (max_z - min_z) * 0.5;
    if (!isfinite(center_x) || !isfinite(center_y) || !isfinite(center_z)) return 0;

    if (view->type != CAD_VIEW_3D) {
        double component_x, component_y, ignored;
        double component_min_x, component_min_y;
        double component_max_x, component_max_y;
        cad_orthographic_components(view->type, min_x, min_y, min_z,
                                    &component_min_x, &component_min_y, &ignored);
        cad_orthographic_components(view->type, max_x, max_y, max_z,
                                    &component_max_x, &component_max_y, &ignored);
        if (component_min_x > component_max_x) {
            double swap = component_min_x; component_min_x = component_max_x; component_max_x = swap;
        }
        if (component_min_y > component_max_y) {
            double swap = component_min_y; component_min_y = component_max_y; component_max_y = swap;
        }
        span_x = component_max_x - component_min_x;
        span_y = component_max_y - component_min_y;
        if (span_x < 1e-9) span_x = 1.0;
        if (span_y < 1e-9) span_y = 1.0;
        zoom = fmin((double)viewport_w * padding / span_x,
                    (double)viewport_h * padding / span_y);
        CadView_SetZoom(view, zoom);
        cad_orthographic_components(view->type, center_x, center_y, center_z,
                                    &component_x, &component_y, &ignored);
        view->pan_x = -component_x * view->zoom;
        view->pan_y = -component_y * view->zoom;
        return isfinite(view->pan_x) && isfinite(view->pan_y);
    }

    /* Size the perspective camera from all eight bounds corners.  Framing
       the rotated box instead of only its radius avoids clipping long, thin
       models while keeping the familiar home orbit. */
    if (preserve_orientation) {
        /* A selection fit is a dolly/zoom operation, not an orbit.  Start
           the projection measurement from neutral scale and pan while
           retaining the user's current 3D camera axes and focal length. */
        if (!cad_view_parameters_valid(view)) return 0;
        view->zoom = 1.0;
        view->pan_x = 0.0;
        view->pan_y = 0.0;
    } else {
        cad_default_camera(view);
    }
    {
        double min_vz = DBL_MAX;
        double radius = hypot(hypot(max_x - min_x, max_y - min_y), max_z - min_z) * 0.5;
        int corner;
        for (corner = 0; corner < 8; ++corner) {
            double x = (corner & 1) ? max_x : min_x;
            double y = (corner & 2) ? max_y : min_y;
            double z = (corner & 4) ? max_z : min_z;
            double vx, vy, vz;
            cad_rotate_to_view(view, x, y, z, &vx, &vy, &vz);
            if (vz < min_vz) min_vz = vz;
        }
        if (radius < 1.0) radius = 1.0;
        view->camera_distance = -min_vz + fmax(64.0, radius * 2.5);
        if (!isfinite(view->camera_distance)) return 0;
    }
    {
        double screen_min_x = DBL_MAX, screen_min_y = DBL_MAX;
        double screen_max_x = -DBL_MAX, screen_max_y = -DBL_MAX;
        int projected = 0;
        int corner;
        for (corner = 0; corner < 8; ++corner) {
            double x = (corner & 1) ? max_x : min_x;
            double y = (corner & 2) ? max_y : min_y;
            double z = (corner & 4) ? max_z : min_z;
            double sx, sy, depth;
            if (!CadView_ProjectPointDepth(view, x, y, z, &sx, &sy, &depth,
                                           viewport_w, viewport_h)) continue;
            if (sx < screen_min_x) screen_min_x = sx;
            if (sx > screen_max_x) screen_max_x = sx;
            if (sy < screen_min_y) screen_min_y = sy;
            if (sy > screen_max_y) screen_max_y = sy;
            ++projected;
        }
        if (!projected) return 0;
        span_x = screen_max_x - screen_min_x;
        span_y = screen_max_y - screen_min_y;
        if (span_x < 1e-9) span_x = 1.0;
        if (span_y < 1e-9) span_y = 1.0;
        CadView_SetZoom(view, fmin((double)viewport_w * padding / span_x,
                                  (double)viewport_h * padding / span_y));

        /* Projection is affine in zoom once camera distance is fixed. */
        view->pan_x = -(((screen_min_x + screen_max_x) * 0.5 - viewport_w * 0.5) * view->zoom);
        view->pan_y = (((screen_min_y + screen_max_y) * 0.5 - viewport_h * 0.5) * view->zoom);
        return isfinite(view->pan_x) && isfinite(view->pan_y);
    }
}

int CadView_FrameBounds(CadView* view,
                        double min_x, double min_y, double min_z,
                        double max_x, double max_y, double max_z,
                        int viewport_w, int viewport_h)
{
    return cad_frame_bounds(view, min_x, min_y, min_z,
                            max_x, max_y, max_z,
                            viewport_w, viewport_h, 0);
}

int CadView_FrameBoundsPreserveOrientation(
    CadView* view,
    double min_x, double min_y, double min_z,
    double max_x, double max_y, double max_z,
    int viewport_w, int viewport_h)
{
    return cad_frame_bounds(view, min_x, min_y, min_z,
                            max_x, max_y, max_z,
                            viewport_w, viewport_h, 1);
}

void CadView_SetPalette(CadView* view, const uint8_t* rgba256x4)
{
    if (!view || !rgba256x4) return;
    memcpy(view->palette_rgba, rgba256x4, sizeof(view->palette_rgba));
    view->palette_valid = 1;
}

void CadView_ClearPalette(CadView* view)
{
    if (!view) return;
    memset(view->palette_rgba, 0, sizeof(view->palette_rgba));
    view->palette_valid = 0;
}

int CadView_ProjectPointDepth(const CadView* view, double x, double y, double z,
                              double* out_x, double* out_y, double* out_depth,
                              int viewport_w, int viewport_h)
{
    double vx, vy, vz;
    if (!view || !out_x || !out_y || !out_depth ||
        viewport_w <= 0 || viewport_h <= 0 ||
        !isfinite(x) || !isfinite(y) || !isfinite(z) ||
        !cad_view_parameters_valid(view)) {
        return 0;
    }

    if (view->type == CAD_VIEW_3D) {
        cad_rotate_to_view(view, x, y, z, &vx, &vy, &vz);
        if (!isfinite(vx) || !isfinite(vy) || !isfinite(vz)) return 0;
        return cad_project_camera_point(view, vx, vy, vz,
                                        viewport_w, viewport_h,
                                        out_x, out_y, out_depth);
    }

    cad_orthographic_components(view->type, x, y, z, &vx, &vy, &vz);
    return cad_project_orthographic_point(view, vx, vy, vz,
                                          viewport_w, viewport_h,
                                          out_x, out_y, out_depth);
}

void CadView_ProjectPoint(const CadView* view, double x, double y, double z,
                          int* out_x, int* out_y, int viewport_w, int viewport_h)
{
    double px, py, depth;
    if (!out_x || !out_y) return;
    if (!CadView_ProjectPointDepth(view, x, y, z, &px, &py, &depth,
                                   viewport_w, viewport_h)) {
        *out_x = INT_MIN / 4;
        *out_y = INT_MIN / 4;
        return;
    }
    if (!cad_round_screen_coordinate(px, out_x) ||
        !cad_round_screen_coordinate(py, out_y)) {
        *out_x = INT_MIN / 4;
        *out_y = INT_MIN / 4;
    }
}

static int cad_project_world_segment(const CadView* view,
                                     double ax, double ay, double az,
                                     double bx, double by, double bz,
                                     int width, int height,
                                     double* out_ax, double* out_ay,
                                     double* out_bx, double* out_by)
{
    double avx, avy, avz, bvx, bvy, bvz;
    double depth;
    double clip_z;
    int a_inside, b_inside;
    if (!cad_view_parameters_valid(view) || view->type != CAD_VIEW_3D ||
        !out_ax || !out_ay || !out_bx || !out_by ||
        !isfinite(ax) || !isfinite(ay) || !isfinite(az) ||
        !isfinite(bx) || !isfinite(by) || !isfinite(bz)) return 0;
    cad_rotate_to_view(view, ax, ay, az, &avx, &avy, &avz);
    cad_rotate_to_view(view, bx, by, bz, &bvx, &bvy, &bvz);
    if (!isfinite(avx) || !isfinite(avy) || !isfinite(avz) ||
        !isfinite(bvx) || !isfinite(bvy) || !isfinite(bvz)) return 0;
    clip_z = CAD_NEAR_PLANE - view->camera_distance;
    if (!isfinite(clip_z)) return 0;
    a_inside = avz >= clip_z;
    b_inside = bvz >= clip_z;
    if (!a_inside && !b_inside) return 0;
    if (a_inside != b_inside) {
        double t, clipped_x, clipped_y;
        if (!cad_clip_intersection_t(avz, bvz, clip_z, &t) ||
            !cad_lerp_finite(avx, bvx, t, &clipped_x) ||
            !cad_lerp_finite(avy, bvy, t, &clipped_y)) return 0;
        if (!a_inside) {
            avx = clipped_x; avy = clipped_y; avz = clip_z;
        } else {
            bvx = clipped_x; bvy = clipped_y; bvz = clip_z;
        }
    }
    if (!cad_project_camera_point(view, avx, avy, avz, width, height,
                                  out_ax, out_ay, &depth)) return 0;
    return cad_project_camera_point(view, bvx, bvy, bvz, width, height,
                                    out_bx, out_by, &depth);
}

static void cad_draw_ortho_grid(const CadView* view, int width, int height)
{
    const RG_Color minor = { 232, 232, 232, 255 };
    const RG_Color major = { 205, 205, 205, 255 };
    const RG_Color axis = { 120, 130, 140, 255 };
    double step = 10.0;
    double pixels = step * view->zoom;
    const double center_x = width * 0.5 + view->pan_x;
    const double center_y = height * 0.5 - view->pan_y;
    double world_min, world_max, value;
    long long iterations;

    if (!cad_screen_coordinate_valid(center_x) ||
        !cad_screen_coordinate_valid(center_y) || !isfinite(pixels)) return;

    while (pixels < 7.0) {
        if (step > DBL_MAX / 10.0) return;
        step *= 10.0;
        pixels = step * view->zoom;
        if (!isfinite(pixels)) return;
    }

    world_min = (-center_x) / view->zoom;
    world_max = (width - center_x) / view->zoom;
    if (!isfinite(world_min) || !isfinite(world_max)) return;
    value = floor(world_min / step) * step;
    for (iterations = 0; isfinite(value) && value <= world_max &&
                         iterations <= (long long)width + 4; ++iterations) {
        int x;
        double screen = center_x + value * view->zoom;
        double line_value = value / step;
        long long line_number;
        double next;
        if (!cad_round_screen_coordinate(screen, &x) || !isfinite(line_value) ||
            line_value <= (double)LLONG_MIN ||
            line_value >= (double)LLONG_MAX) break;
        line_number = llround(line_value);
        RG_Color color = (line_number % 10 == 0) ? major : minor;
        rg_line(x, 0, x, height, color);
        next = value + step;
        if (!isfinite(next) || next <= value) break;
        value = next;
    }

    world_min = (center_y - height) / view->zoom;
    world_max = center_y / view->zoom;
    if (!isfinite(world_min) || !isfinite(world_max)) return;
    value = floor(world_min / step) * step;
    for (iterations = 0; isfinite(value) && value <= world_max &&
                         iterations <= (long long)height + 4; ++iterations) {
        int y;
        double screen = center_y - value * view->zoom;
        double line_value = value / step;
        long long line_number;
        double next;
        if (!cad_round_screen_coordinate(screen, &y) || !isfinite(line_value) ||
            line_value <= (double)LLONG_MIN ||
            line_value >= (double)LLONG_MAX) break;
        line_number = llround(line_value);
        RG_Color color = (line_number % 10 == 0) ? major : minor;
        rg_line(0, y, width, y, color);
        next = value + step;
        if (!isfinite(next) || next <= value) break;
        value = next;
    }

    if (center_x >= 0.0 && center_x <= width) {
        int x;
        if (cad_round_screen_coordinate(center_x, &x)) {
            rg_line(x, 0, x, height, axis);
            if (x + 1 < width) rg_line(x + 1, 0, x + 1, height, axis);
        }
    }
    if (center_y >= 0.0 && center_y <= height) {
        int y;
        if (cad_round_screen_coordinate(center_y, &y)) {
            rg_line(0, y, width, y, axis);
            if (y + 1 < height) rg_line(0, y + 1, width, y + 1, axis);
        }
    }
}

static void cad_draw_3d_ground(const CadView* view, int width, int height)
{
    const RG_Color minor = { 220, 225, 225, 255 };
    const RG_Color major = { 175, 185, 185, 255 };
    int coordinate;

    for (coordinate = -(int)CAD_GRID_EXTENT; coordinate <= (int)CAD_GRID_EXTENT; coordinate += 32) {
        double x1, y1, x2, y2;
        int sx1, sy1, sx2, sy2;
        RG_Color color = (coordinate % 128 == 0) ? major : minor;
        if (cad_project_world_segment(view,
                                      (double)coordinate, 0.0, -CAD_GRID_EXTENT,
                                      (double)coordinate, 0.0, CAD_GRID_EXTENT,
                                      width, height, &x1, &y1, &x2, &y2) &&
            cad_round_screen_coordinate(x1, &sx1) &&
            cad_round_screen_coordinate(y1, &sy1) &&
            cad_round_screen_coordinate(x2, &sx2) &&
            cad_round_screen_coordinate(y2, &sy2)) {
            rg_line(sx1, sy1, sx2, sy2, color);
        }
        if (cad_project_world_segment(view,
                                      -CAD_GRID_EXTENT, 0.0, (double)coordinate,
                                      CAD_GRID_EXTENT, 0.0, (double)coordinate,
                                      width, height, &x1, &y1, &x2, &y2) &&
            cad_round_screen_coordinate(x1, &sx1) &&
            cad_round_screen_coordinate(y1, &sy1) &&
            cad_round_screen_coordinate(x2, &sx2) &&
            cad_round_screen_coordinate(y2, &sy2)) {
            rg_line(sx1, sy1, sx2, sy2, color);
        }
    }
}

static int cad_collect_polygon(const CadView* view,
                               const CadViewGeometry* geometry, int16_t index,
                               ProjectedPolygon* projected, int width, int height)
{
    const CadPolygon* polygon;
    int16_t point_index;
    int count = 0;
    double depth_mean = 0.0;
    double input_x[CAD_MAX_FACE_POINTS];
    double input_y[CAD_MAX_FACE_POINTS];
    double input_z[CAD_MAX_FACE_POINTS];

    if (!cad_view_parameters_valid(view) || !geometry || !geometry->data ||
        !projected ||
        width <= 0 || height <= 0 || index < 0 || index >= CAD_MAX_POLYGONS) return 0;
    polygon = &geometry->data->polygons[index];
    if (!polygon->flags || polygon->npoints < 2 || polygon->npoints > CAD_MAX_FACE_POINTS) return 0;
    point_index = polygon->firstPoint;

    while (point_index >= 0 && point_index < CAD_MAX_POINTS && count < polygon->npoints) {
        const CadPoint* point = &geometry->data->points[point_index];
        CadPosition position;
        if (!cad_geometry_point(geometry, point_index, &position)) return 0;
        if (view->type == CAD_VIEW_3D) {
            cad_rotate_to_view(view, position.x, position.y, position.z,
                               &input_x[count], &input_y[count], &input_z[count]);
        } else {
            cad_orthographic_components(view->type,
                                        position.x, position.y, position.z,
                                        &input_x[count], &input_y[count], &input_z[count]);
        }
        if (!isfinite(input_x[count]) || !isfinite(input_y[count]) ||
            !isfinite(input_z[count])) return 0;
        point_index = point->nextPoint;
        ++count;
    }

    if (count != polygon->npoints || point_index != INVALID_INDEX) return 0;

    if (view->type == CAD_VIEW_3D) {
        const double clip_z = CAD_NEAR_PLANE - view->camera_distance;
        int output_count = 0;
        if (count == 2) {
            double ax = input_x[0], ay = input_y[0], az = input_z[0];
            double bx = input_x[1], by = input_y[1], bz = input_z[1];
            int a_inside = az >= clip_z;
            int b_inside = bz >= clip_z;
            if (!a_inside && !b_inside) return 0;
            if (a_inside != b_inside) {
                double t, ix, iy;
                if (!cad_clip_intersection_t(az, bz, clip_z, &t) ||
                    !cad_lerp_finite(ax, bx, t, &ix) ||
                    !cad_lerp_finite(ay, by, t, &iy)) return 0;
                if (!a_inside) { ax = ix; ay = iy; az = clip_z; }
                else { bx = ix; by = iy; bz = clip_z; }
            }
            projected->vx[0] = ax; projected->vy[0] = ay; projected->vz[0] = az;
            projected->vx[1] = bx; projected->vy[1] = by; projected->vz[1] = bz;
            output_count = 2;
        } else {
            int edge;
            for (edge = 0; edge < count; ++edge) {
                int previous = edge ? edge - 1 : count - 1;
                double sx = input_x[previous], sy = input_y[previous], sz = input_z[previous];
                double ex = input_x[edge], ey = input_y[edge], ez = input_z[edge];
                int start_inside = sz >= clip_z;
                int end_inside = ez >= clip_z;
                if (start_inside != end_inside) {
                    double t, clipped_x, clipped_y;
                    if (output_count >= CAD_MAX_PROJECTED_FACE_POINTS) return 0;
                    if (!cad_clip_intersection_t(sz, ez, clip_z, &t) ||
                        !cad_lerp_finite(sx, ex, t, &clipped_x) ||
                        !cad_lerp_finite(sy, ey, t, &clipped_y)) return 0;
                    projected->vx[output_count] = clipped_x;
                    projected->vy[output_count] = clipped_y;
                    projected->vz[output_count] = clip_z;
                    output_count++;
                }
                if (end_inside) {
                    if (output_count >= CAD_MAX_PROJECTED_FACE_POINTS) return 0;
                    projected->vx[output_count] = ex;
                    projected->vy[output_count] = ey;
                    projected->vz[output_count] = ez;
                    output_count++;
                }
            }
            if (output_count < 3) return 0;
        }
        projected->count = output_count;
        for (count = 0; count < output_count; ++count) {
            double depth;
            if (!cad_project_camera_point(view,
                                          projected->vx[count],
                                          projected->vy[count],
                                          projected->vz[count],
                                          width, height,
                                          &projected->x[count],
                                          &projected->y[count], &depth)) return 0;
            projected->camera_depth[count] = depth;
            depth_mean += depth / output_count;
            if (!isfinite(depth_mean)) return 0;
        }
    } else {
        projected->count = count;
        for (int point = 0; point < count; ++point) {
            projected->vx[point] = input_x[point];
            projected->vy[point] = input_y[point];
            projected->vz[point] = input_z[point];
            if (!cad_project_orthographic_point(view,
                                                input_x[point], input_y[point],
                                                input_z[point], width, height,
                                                &projected->x[point],
                                                &projected->y[point],
                                                &projected->camera_depth[point])) return 0;
            projected->camera_depth[point] = input_z[point];
            depth_mean += input_z[point] / count;
            if (!isfinite(depth_mean)) return 0;
        }
    }
    projected->index = index;
    projected->depth = depth_mean;
    projected->selected = cad_geometry_polygon_selected(geometry, index);
    return 1;
}

static int cad_compare_polygon_depth(const void* left, const void* right)
{
    const ProjectedPolygon* a = (const ProjectedPolygon*)left;
    const ProjectedPolygon* b = (const ProjectedPolygon*)right;
    if (a->depth < b->depth) return 1;
    if (a->depth > b->depth) return -1;
    return 0;
}

static RG_Color cad_index_color(const CadView* view, uint8_t index, double brightness)
{
    static const RG_Color fallback[16] = {
        { 116, 151, 148, 255 }, { 132, 167, 162, 255 },
        { 101, 139, 143, 255 }, { 151, 176, 166, 255 },
        { 118, 142, 169, 255 }, { 150, 139, 170, 255 },
        { 104, 160, 142, 255 }, { 170, 164, 142, 255 },
        { 156, 118, 118, 255 }, { 135, 157, 184, 255 },
        { 173, 145, 124, 255 }, { 124, 171, 154, 255 },
        { 164, 130, 164, 255 }, { 144, 172, 128, 255 },
        { 116, 160, 181, 255 }, { 174, 174, 174, 255 }
    };
    RG_Color color;
    if (view && view->palette_valid) {
        color.r = view->palette_rgba[index][0];
        color.g = view->palette_rgba[index][1];
        color.b = view->palette_rgba[index][2];
        color.a = view->palette_rgba[index][3];
    } else {
        color = fallback[index & 15u];
    }
    if (!isfinite(brightness)) brightness = 0.8;
    if (brightness < 0.25) brightness = 0.25;
    if (brightness > 1.0) brightness = 1.0;
    color.r = (uint8_t)lround(color.r * brightness);
    color.g = (uint8_t)lround(color.g * brightness);
    color.b = (uint8_t)lround(color.b * brightness);
    return color;
}

static double cad_polygon_brightness(const ProjectedPolygon* polygon)
{
    double ax, ay, az, bx, by, bz, nx, ny, nz, length, dot;
    if (!polygon || polygon->count < 3) return 0.8;
    ax = polygon->vx[1] - polygon->vx[0];
    ay = polygon->vy[1] - polygon->vy[0];
    az = polygon->vz[1] - polygon->vz[0];
    bx = polygon->vx[2] - polygon->vx[0];
    by = polygon->vy[2] - polygon->vy[0];
    bz = polygon->vz[2] - polygon->vz[0];
    if (!isfinite(ax) || !isfinite(ay) || !isfinite(az) ||
        !isfinite(bx) || !isfinite(by) || !isfinite(bz)) return 0.8;
    nx = ay * bz - az * by;
    ny = az * bx - ax * bz;
    nz = ax * by - ay * bx;
    if (!isfinite(nx) || !isfinite(ny) || !isfinite(nz)) return 0.8;
    length = hypot(hypot(nx, ny), nz);
    if (!isfinite(length)) return 0.8;
    if (length < 1e-9) return 0.5;
    nx /= length;
    ny /= length;
    nz /= length;
    dot = fabs(nx * 0.35 + ny * 0.55 + nz * 0.76);
    if (!isfinite(dot)) return 0.8;
    return 0.38 + dot * 0.62;
}

static int cad_points_coincident(const CadPosition* a, const CadPosition* b)
{
    const double epsilon = 1e-8;
    double scale;
    if (!a || !b) return 0;
    scale = fmax(1.0, fmax(fabs(a->x), fabs(b->x)));
    if (fabs(a->x - b->x) > epsilon * scale) return 0;
    scale = fmax(1.0, fmax(fabs(a->y), fabs(b->y)));
    if (fabs(a->y - b->y) > epsilon * scale) return 0;
    scale = fmax(1.0, fmax(fabs(a->z), fabs(b->z)));
    return fabs(a->z - b->z) <= epsilon * scale;
}

/* A reciprocal link by itself is not enough to suppress a face: malformed
   files can contain stale links.  Only geometrically reversed, mutual pairs
   participate in solid-mode side selection. */
static int cad_polygons_form_reciprocal_pair(const CadViewGeometry* geometry,
                                             int16_t first, int16_t second)
{
    int16_t first_points[CAD_MAX_FACE_POINTS];
    int16_t second_points[CAD_MAX_FACE_POINTS];
    const CadPolygon* a;
    const CadPolygon* b;
    int16_t point;
    int i, offset;
    if (!geometry || !geometry->data || first < 0 || second < 0 ||
        first >= CAD_MAX_POLYGONS ||
        second >= CAD_MAX_POLYGONS || first == second) return 0;
    a = &geometry->data->polygons[first];
    b = &geometry->data->polygons[second];
    if (!a->flags || !b->flags || a->both != second || b->both != first ||
        a->npoints < 3 || a->npoints != b->npoints ||
        a->npoints > CAD_MAX_FACE_POINTS) return 0;
    point = a->firstPoint;
    for (i = 0; i < a->npoints; ++i) {
        if (point < 0 || point >= CAD_MAX_POINTS ||
            !geometry->data->points[point].flags) return 0;
        first_points[i] = point;
        point = geometry->data->points[point].nextPoint;
    }
    if (point != INVALID_INDEX) return 0;
    point = b->firstPoint;
    for (i = 0; i < b->npoints; ++i) {
        if (point < 0 || point >= CAD_MAX_POINTS ||
            !geometry->data->points[point].flags) return 0;
        second_points[i] = point;
        point = geometry->data->points[point].nextPoint;
    }
    if (point != INVALID_INDEX) return 0;

    for (offset = 0; offset < a->npoints; ++offset) {
        CadPosition first_position;
        CadPosition second_position;
        if (!cad_geometry_point(geometry, first_points[0], &first_position) ||
            !cad_geometry_point(geometry, second_points[offset],
                                &second_position) ||
            !cad_points_coincident(&first_position, &second_position)) continue;
        for (i = 1; i < a->npoints; ++i) {
            int reverse = (offset - i + a->npoints) % a->npoints;
            if (!cad_geometry_point(geometry, first_points[i],
                                    &first_position) ||
                !cad_geometry_point(geometry, second_points[reverse],
                                    &second_position) ||
                !cad_points_coincident(&first_position, &second_position)) break;
        }
        if (i == a->npoints) return 1;
    }
    return 0;
}

static double cad_projected_normal_z(const ProjectedPolygon* polygon)
{
    double ax, ay, bx, by;
    if (!polygon || polygon->count < 3) return 0.0;
    ax = polygon->vx[1] - polygon->vx[0];
    ay = polygon->vy[1] - polygon->vy[0];
    bx = polygon->vx[2] - polygon->vx[0];
    by = polygon->vy[2] - polygon->vy[0];
    return ax * by - ay * bx;
}

static double cad_geometry_camera_normal_z(const CadView* view,
                                           const CadViewGeometry* geometry,
                                           int16_t index,
                                           const ProjectedPolygon* projected)
{
    if (view && geometry && geometry->pose && index >= 0 &&
        index < CAD_MAX_POLYGONS &&
        geometry->pose->faceNormalValid[index]) {
        const CadPosition normal = geometry->pose->faceNormals[index];
        double view_x, view_y, view_z;
        cad_rotate_to_view(view, normal.x, normal.y, normal.z,
                           &view_x, &view_y, &view_z);
        if (isfinite(view_z)) return view_z;
    }
    return cad_projected_normal_z(projected);
}

static int cad_paired_member_is_hidden(const CadView* view,
                                       const CadViewGeometry* geometry,
                                       int16_t index,
                                       const ProjectedPolygon* projected,
                                       int width, int height)
{
    const int16_t pair = geometry && geometry->data && index >= 0 &&
                         index < CAD_MAX_POLYGONS
                         ? geometry->data->polygons[index].both : INVALID_INDEX;
    ProjectedPolygon opposite;
    double normal, opposite_normal;
    if (!view || view->type != CAD_VIEW_3D || view->wireframe || !projected ||
        !cad_polygons_form_reciprocal_pair(geometry, index, pair) ||
        !cad_collect_polygon(view, geometry, pair, &opposite, width, height)) return 0;
    normal = cad_geometry_camera_normal_z(view, geometry, index, projected);
    opposite_normal = cad_geometry_camera_normal_z(view, geometry, pair,
                                                    &opposite);
    if (fabs(normal) <= 1e-10 || fabs(opposite_normal) <= 1e-10 ||
        (normal < 0.0) == (opposite_normal < 0.0)) {
        return index > pair;
    }
    /* Camera-space depth increases away from the camera, so a normal facing
       toward it has a negative Z component. */
    return normal >= 0.0;
}

static double cad_gl_depth(double camera_depth)
{
    double depth;
    if (camera_depth < CAD_NEAR_PLANE) camera_depth = CAD_NEAR_PLANE;
    depth = 1.0 - (2.0 * CAD_NEAR_PLANE / camera_depth);
    if (depth < -1.0) depth = -1.0;
    if (depth > 0.999999) depth = 0.999999;
    return depth;
}

static double cad_cross_2d(double ax, double ay, double bx, double by,
                           double cx, double cy)
{
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

static int cad_point_in_triangle(double px, double py,
                                 double ax, double ay,
                                 double bx, double by,
                                 double cx, double cy,
                                 double orientation)
{
    const double epsilon = 1e-9;
    return orientation * cad_cross_2d(ax, ay, bx, by, px, py) >= -epsilon &&
           orientation * cad_cross_2d(bx, by, cx, cy, px, py) >= -epsilon &&
           orientation * cad_cross_2d(cx, cy, ax, ay, px, py) >= -epsilon;
}

static void cad_emit_projected_vertex(const ProjectedPolygon* polygon, int index)
{
    glVertex3d(polygon->x[index], polygon->y[index],
               cad_gl_depth(polygon->camera_depth[index]));
}

static void cad_fill_projected(const ProjectedPolygon* polygon, RG_Color color)
{
    int vertices[CAD_MAX_PROJECTED_FACE_POINTS];
    int triangles[(CAD_MAX_PROJECTED_FACE_POINTS - 2) * 3];
    int vertex_count;
    int triangle_count = 0;
    double area = 0.0;
    double orientation;
    int i;
    if (!polygon || polygon->count < 3) return;
    vertex_count = polygon->count;
    for (i = 0; i < vertex_count; ++i) {
        int next = (i + 1) % vertex_count;
        vertices[i] = i;
        area += polygon->x[i] * polygon->y[next] -
                polygon->x[next] * polygon->y[i];
    }
    orientation = area >= 0.0 ? 1.0 : -1.0;

    while (vertex_count > 3) {
        int ear_found = 0;
        for (i = 0; i < vertex_count; ++i) {
            int previous_slot = (i + vertex_count - 1) % vertex_count;
            int next_slot = (i + 1) % vertex_count;
            int a = vertices[previous_slot];
            int b = vertices[i];
            int c = vertices[next_slot];
            int other;
            int contains_point = 0;
            if (orientation * cad_cross_2d(
                    polygon->x[a], polygon->y[a],
                    polygon->x[b], polygon->y[b],
                    polygon->x[c], polygon->y[c]) <= 1e-9) continue;
            for (other = 0; other < vertex_count; ++other) {
                int candidate;
                if (other == previous_slot || other == i || other == next_slot) continue;
                candidate = vertices[other];
                if (cad_point_in_triangle(
                        polygon->x[candidate], polygon->y[candidate],
                        polygon->x[a], polygon->y[a],
                        polygon->x[b], polygon->y[b],
                        polygon->x[c], polygon->y[c], orientation)) {
                    contains_point = 1;
                    break;
                }
            }
            if (contains_point) continue;
            triangles[triangle_count * 3 + 0] = a;
            triangles[triangle_count * 3 + 1] = b;
            triangles[triangle_count * 3 + 2] = c;
            triangle_count++;
            memmove(&vertices[i], &vertices[i + 1],
                    (size_t)(vertex_count - i - 1) * sizeof(vertices[0]));
            vertex_count--;
            ear_found = 1;
            break;
        }
        if (!ear_found) {
            /* Degenerate/self-intersecting projections are still displayed
               deterministically as a fan; topology validation handles the
               record-level invariants separately. */
            triangle_count = 0;
            for (i = 1; i + 1 < polygon->count; ++i) {
                triangles[triangle_count * 3 + 0] = 0;
                triangles[triangle_count * 3 + 1] = i;
                triangles[triangle_count * 3 + 2] = i + 1;
                triangle_count++;
            }
            vertex_count = 0;
            break;
        }
    }
    if (vertex_count == 3) {
        triangles[triangle_count * 3 + 0] = vertices[0];
        triangles[triangle_count * 3 + 1] = vertices[1];
        triangles[triangle_count * 3 + 2] = vertices[2];
        triangle_count++;
    }

    glColor4ub(color.r, color.g, color.b, color.a);
    glBegin(GL_TRIANGLES);
    for (i = 0; i < triangle_count * 3; ++i)
        cad_emit_projected_vertex(polygon, triangles[i]);
    glEnd();
}

static void cad_outline_projected(const ProjectedPolygon* polygon,
                                  RG_Color color, int use_depth)
{
    int i;
    GLenum primitive;
    if (!polygon || polygon->count < 2) return;
    primitive = polygon->count == 2 ? GL_LINES : GL_LINE_LOOP;
    glColor4ub(color.r, color.g, color.b, color.a);
    glBegin(primitive);
    for (i = 0; i < polygon->count; ++i) {
        if (use_depth) cad_emit_projected_vertex(polygon, i);
        else glVertex2d(polygon->x[i], polygon->y[i]);
    }
    glEnd();
}

static void cad_render_geometry(const CadView* view,
                                const CadViewGeometry* geometry,
                                int pixel_x, int pixel_y,
                                int pixel_w, int pixel_h,
                                int framebuffer_h,
                                int logical_w, int logical_h)
{
    ProjectedPolygon* polygons;
    uint8_t connected_points[CAD_MAX_POINTS];
    uint8_t hidden_pair_member[CAD_MAX_POLYGONS];
    int polygon_count = 0;
    int i;
    const RG_Color background = { 250, 250, 248, 255 };
    const RG_Color edge = { 32, 61, 190, 255 };
    const RG_Color selected_edge = { 28, 150, 90, 255 };

    if (!cad_view_parameters_valid(view) || !geometry || !geometry->data ||
        pixel_w <= 0 || pixel_h <= 0 || logical_w <= 0 || logical_h <= 0) return;
    rg_set_viewport_tl(pixel_x, pixel_y, pixel_w, pixel_h, framebuffer_h, logical_w, logical_h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    glDisable(GL_TEXTURE_2D);
    rg_fill_rect(0, 0, logical_w, logical_h, background);

    if (view->show_grid) {
        if (view->type == CAD_VIEW_3D) cad_draw_3d_ground(view, logical_w, logical_h);
        else cad_draw_ortho_grid(view, logical_w, logical_h);
    }

    polygons = cad_polygon_scratch;
    memset(connected_points, 0, sizeof(connected_points));
    memset(hidden_pair_member, 0, sizeof(hidden_pair_member));
    for (i = 0; i < geometry->data->polygonCount && i < CAD_MAX_POLYGONS; ++i) {
        const CadPolygon* source = &geometry->data->polygons[i];
        int16_t point;
        int step;
        if (source->flags) {
            point = source->firstPoint;
            for (step = 0; step < source->npoints &&
                           point >= 0 && point < CAD_MAX_POINTS; ++step) {
                if (!geometry->data->points[point].flags) break;
                connected_points[point] = 1;
                point = geometry->data->points[point].nextPoint;
            }
        }
        if (cad_collect_polygon(view, geometry, (int16_t)i,
                                &polygons[polygon_count],
                                logical_w, logical_h)) {
            ++polygon_count;
        }
    }
    if (view->type == CAD_VIEW_3D) {
        qsort(polygons, (size_t)polygon_count, sizeof(ProjectedPolygon), cad_compare_polygon_depth);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_TRUE);

        if (!view->wireframe)
            for (i = 0; i < polygon_count; ++i)
                hidden_pair_member[polygons[i].index] = (uint8_t)cad_paired_member_is_hidden(
                    view, geometry, polygons[i].index, &polygons[i],
                    logical_w, logical_h);
    }

    for (i = 0; i < polygon_count; ++i) {
        const CadPolygon* source = &geometry->data->polygons[polygons[i].index];
        int selected = polygons[i].selected;
        RG_Color line_color;
        if (hidden_pair_member[polygons[i].index]) continue;
        if (view->type == CAD_VIEW_3D && !view->wireframe && source->both >= 0 &&
            source->both < CAD_MAX_POLYGONS &&
            cad_polygons_form_reciprocal_pair(geometry, polygons[i].index,
                                              source->both)) {
            selected |= cad_geometry_polygon_selected(geometry, source->both);
        }
        line_color = selected ? selected_edge
                              : polygons[i].count == 2
                                ? cad_index_color(view, source->color, 1.0)
                                : edge;
        if (view->type == CAD_VIEW_3D && !view->wireframe && polygons[i].count >= 3) {
            RG_Color fill = selected
                ? (RG_Color){ 92, 190, 138, 255 }
                : cad_index_color(view, source->color, cad_polygon_brightness(&polygons[i]));
            cad_fill_projected(&polygons[i], fill);
        }
        cad_outline_projected(&polygons[i], line_color,
                              view->type == CAD_VIEW_3D);
    }

    /* Selection/orphan handles are an editor overlay and remain visible even
       when the selected point lies behind a shaded face. */
    glDisable(GL_DEPTH_TEST);

    for (i = 0; i < geometry->data->pointCount && i < CAD_MAX_POINTS; ++i) {
        const CadPoint* point = &geometry->data->points[i];
        CadPosition position;
        int selected, connected;
        int screen_x, screen_y;
        double x, y, depth;
        RG_Color color;
        if (!point->flags) continue;
        selected = cad_geometry_point_selected(geometry, (int16_t)i);
        connected = connected_points[i] != 0;
        if (!selected && connected) continue;
        if (!cad_geometry_point(geometry, (int16_t)i, &position)) continue;
        if (!CadView_ProjectPointDepth(view,
                                       position.x, position.y, position.z,
                                       &x, &y, &depth, logical_w, logical_h)) continue;
        if (!cad_round_screen_coordinate(x, &screen_x) ||
            !cad_round_screen_coordinate(y, &screen_y)) continue;
        color = selected ? (RG_Color){ 220, 45, 45, 255 } : (RG_Color){ 20, 80, 220, 255 };
        rg_fill_rect(screen_x - 3, screen_y - 3, 7, 7, color);
    }
}

void CadView_Render(const CadView* view, const CadCore* core,
                    int pixel_x, int pixel_y, int pixel_w, int pixel_h,
                    int framebuffer_h, int logical_w, int logical_h)
{
    CadViewGeometry geometry;
    if (!cad_geometry_from_core(core, &geometry)) return;
    cad_render_geometry(view, &geometry, pixel_x, pixel_y, pixel_w, pixel_h,
                        framebuffer_h, logical_w, logical_h);
}

void CadView_RenderScene(const CadView* view, const CadScene* scene,
                         int pixel_x, int pixel_y, int pixel_w, int pixel_h,
                         int framebuffer_h, int logical_w, int logical_h)
{
    CadViewGeometry geometry;
    if (!cad_geometry_from_scene(scene, &geometry)) return;
    cad_render_geometry(view, &geometry, pixel_x, pixel_y, pixel_w, pixel_h,
                        framebuffer_h, logical_w, logical_h);
}

static int16_t cad_find_nearest_point(const CadView* view,
                                      const CadViewGeometry* geometry,
                                      int screen_x, int screen_y,
                                      int viewport_x, int viewport_y,
                                      int viewport_w, int viewport_h,
                                      int threshold_pixels)
{
    int16_t nearest = -1;
    double best = (double)threshold_pixels * threshold_pixels;
    double best_depth = HUGE_VAL;
    int i;
    int local_x = screen_x - viewport_x;
    int local_y = screen_y - viewport_y;
    if (!view || !geometry || !geometry->data || local_x < 0 || local_y < 0 ||
        local_x >= viewport_w || local_y >= viewport_h) return -1;

    for (i = 0; i < geometry->data->pointCount && i < CAD_MAX_POINTS; ++i) {
        CadPosition position;
        double x, y, depth, dx, dy, distance;
        if (!cad_geometry_point(geometry, (int16_t)i, &position)) continue;
        if (!CadView_ProjectPointDepth(view,
                                       position.x, position.y, position.z,
                                       &x, &y, &depth, viewport_w, viewport_h)) continue;
        dx = local_x - x;
        dy = local_y - y;
        distance = dx * dx + dy * dy;
        if (distance < best - 1e-9 ||
            (fabs(distance - best) <= 1e-9 && depth < best_depth)) {
            best = distance;
            best_depth = depth;
            nearest = (int16_t)i;
        }
    }
    return nearest;
}

int16_t CadView_FindNearestPoint(const CadView* view, const CadCore* core,
                                 int screen_x, int screen_y,
                                 int viewport_x, int viewport_y,
                                 int viewport_w, int viewport_h,
                                 int threshold_pixels)
{
    CadViewGeometry geometry;
    if (!cad_geometry_from_core(core, &geometry)) return -1;
    return cad_find_nearest_point(view, &geometry, screen_x, screen_y,
                                  viewport_x, viewport_y,
                                  viewport_w, viewport_h, threshold_pixels);
}

int16_t CadView_FindNearestScenePoint(const CadView* view,
                                      const CadScene* scene,
                                      int screen_x, int screen_y,
                                      int viewport_x, int viewport_y,
                                      int viewport_w, int viewport_h,
                                      int threshold_pixels)
{
    CadViewGeometry geometry;
    if (!cad_geometry_from_scene(scene, &geometry)) return -1;
    return cad_find_nearest_point(view, &geometry, screen_x, screen_y,
                                  viewport_x, viewport_y,
                                  viewport_w, viewport_h, threshold_pixels);
}

static int cad_find_points_at_location(const CadView* view,
                                       const CadViewGeometry* geometry,
                                       int screen_x, int screen_y,
                                       int viewport_x, int viewport_y,
                                       int viewport_w, int viewport_h,
                                       int threshold_pixels,
                                       double world_threshold,
                                       int16_t* out_indices, int max_count)
{
    int16_t nearest;
    CadPosition reference;
    int count = 0;
    int i;
    if (!view || !geometry || !geometry->data || !out_indices || max_count <= 0 ||
        !isfinite(world_threshold) || world_threshold < 0.0) return 0;
    nearest = cad_find_nearest_point(view, geometry, screen_x, screen_y,
                                     viewport_x, viewport_y,
                                     viewport_w, viewport_h,
                                     threshold_pixels);
    if (nearest < 0 || !cad_geometry_point(geometry, nearest, &reference)) return 0;
    for (i = 0; i < geometry->data->pointCount && i < CAD_MAX_POINTS &&
                    count < max_count; ++i) {
        CadPosition position;
        double dx, dy, dz;
        if (!cad_geometry_point(geometry, (int16_t)i, &position)) continue;
        dx = position.x - reference.x;
        dy = position.y - reference.y;
        dz = position.z - reference.z;
        if (dx * dx + dy * dy + dz * dz <= world_threshold * world_threshold) {
            out_indices[count++] = (int16_t)i;
        }
    }
    return count;
}

int CadView_FindPointsAtLocation(const CadView* view, const CadCore* core,
                                 int screen_x, int screen_y,
                                 int viewport_x, int viewport_y,
                                 int viewport_w, int viewport_h,
                                 int threshold_pixels,
                                 double world_threshold,
                                 int16_t* out_indices, int max_count)
{
    CadViewGeometry geometry;
    if (!cad_geometry_from_core(core, &geometry)) return 0;
    return cad_find_points_at_location(view, &geometry, screen_x, screen_y,
                                       viewport_x, viewport_y,
                                       viewport_w, viewport_h,
                                       threshold_pixels, world_threshold,
                                       out_indices, max_count);
}

int CadView_FindScenePointsAtLocation(const CadView* view,
                                      const CadScene* scene,
                                      int screen_x, int screen_y,
                                      int viewport_x, int viewport_y,
                                      int viewport_w, int viewport_h,
                                      int threshold_pixels,
                                      double world_threshold,
                                      int16_t* out_indices, int max_count)
{
    CadViewGeometry geometry;
    if (!cad_geometry_from_scene(scene, &geometry)) return 0;
    return cad_find_points_at_location(view, &geometry, screen_x, screen_y,
                                       viewport_x, viewport_y,
                                       viewport_w, viewport_h,
                                       threshold_pixels, world_threshold,
                                       out_indices, max_count);
}

static double cad_segment_distance_sq(double px, double py, double ax, double ay,
                                      double bx, double by)
{
    double dx = bx - ax, dy = by - ay;
    double length_sq = dx * dx + dy * dy;
    double t, x, y;
    if (length_sq <= 1e-12) {
        dx = px - ax;
        dy = py - ay;
        return dx * dx + dy * dy;
    }
    t = ((px - ax) * dx + (py - ay) * dy) / length_sq;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    x = ax + t * dx;
    y = ay + t * dy;
    dx = px - x;
    dy = py - y;
    return dx * dx + dy * dy;
}

/* Evaluate the surface depth at the cursor instead of using the face's mean
   vertex depth.  Orthographic depth is affine in screen space; perspective
   reciprocal depth is affine.  Any non-degenerate projected triangle on a
   validated coplanar face therefore describes the entire face plane, even
   when the cursor lies in a different part of a concave polygon. */
static int cad_projected_depth_at_point(const CadView* view,
                                        const ProjectedPolygon* polygon,
                                        double x, double y,
                                        double* out_depth)
{
    if (!view || !polygon || !out_depth || polygon->count < 2 ||
        !isfinite(x) || !isfinite(y)) return 0;

    if (polygon->count == 2) {
        const double dx = polygon->x[1] - polygon->x[0];
        const double dy = polygon->y[1] - polygon->y[0];
        const double length_sq = dx * dx + dy * dy;
        double t = 0.0;
        double depth;
        if (isfinite(length_sq) && length_sq > 1e-12) {
            t = ((x - polygon->x[0]) * dx +
                 (y - polygon->y[0]) * dy) / length_sq;
            if (!isfinite(t)) return 0;
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
        }
        if (view->type == CAD_VIEW_3D) {
            const double reciprocal =
                (1.0 - t) / polygon->camera_depth[0] +
                t / polygon->camera_depth[1];
            if (!isfinite(reciprocal) || reciprocal <= 0.0) return 0;
            depth = 1.0 / reciprocal;
        } else if (!cad_lerp_finite(polygon->camera_depth[0],
                                    polygon->camera_depth[1], t, &depth)) {
            return 0;
        }
        if (!isfinite(depth)) return 0;
        *out_depth = depth;
        return 1;
    }

    {
        int second = -1;
        int third = -1;
        double denominator = 0.0;
        double largest_area = 0.0;
        double first_weight, second_weight, third_weight;
        double depth;

        /* Locate the most stable triangle that includes vertex zero. */
        for (int b = 1; b + 1 < polygon->count; ++b) {
            for (int c = b + 1; c < polygon->count; ++c) {
                const double area =
                    (polygon->y[b] - polygon->y[c]) *
                        (polygon->x[0] - polygon->x[c]) +
                    (polygon->x[c] - polygon->x[b]) *
                        (polygon->y[0] - polygon->y[c]);
                if (isfinite(area) && fabs(area) > largest_area) {
                    largest_area = fabs(area);
                    denominator = area;
                    second = b;
                    third = c;
                }
            }
        }
        if (second < 0 || third < 0 || largest_area <= 1e-12) return 0;

        first_weight =
            ((polygon->y[second] - polygon->y[third]) *
                 (x - polygon->x[third]) +
             (polygon->x[third] - polygon->x[second]) *
                 (y - polygon->y[third])) / denominator;
        second_weight =
            ((polygon->y[third] - polygon->y[0]) *
                 (x - polygon->x[third]) +
             (polygon->x[0] - polygon->x[third]) *
                 (y - polygon->y[third])) / denominator;
        third_weight = 1.0 - first_weight - second_weight;
        if (!isfinite(first_weight) || !isfinite(second_weight) ||
            !isfinite(third_weight)) return 0;

        if (view->type == CAD_VIEW_3D) {
            const double reciprocal =
                first_weight / polygon->camera_depth[0] +
                second_weight / polygon->camera_depth[second] +
                third_weight / polygon->camera_depth[third];
            if (!isfinite(reciprocal) || reciprocal <= 0.0) return 0;
            depth = 1.0 / reciprocal;
        } else {
            const double scale = fmax(
                fabs(polygon->camera_depth[0]),
                fmax(fabs(polygon->camera_depth[second]),
                     fabs(polygon->camera_depth[third])));
            if (!isfinite(scale)) return 0;
            if (scale == 0.0) {
                depth = 0.0;
            } else {
                depth = (first_weight * (polygon->camera_depth[0] / scale) +
                         second_weight *
                             (polygon->camera_depth[second] / scale) +
                         third_weight *
                             (polygon->camera_depth[third] / scale)) * scale;
            }
        }
        if (!isfinite(depth)) return 0;
        *out_depth = depth;
        return 1;
    }
}

static int cad_point_in_polygon(double x, double y, const ProjectedPolygon* polygon)
{
    int inside = 0;
    int i, j;
    for (i = 0, j = polygon->count - 1; i < polygon->count; j = i++) {
        const double yi = polygon->y[i], yj = polygon->y[j];
        const double xi = polygon->x[i], xj = polygon->x[j];
        if (((yi > y) != (yj > y)) &&
            (x < (xj - xi) * (y - yi) / ((yj - yi) == 0.0 ? 1e-12 : (yj - yi)) + xi)) {
            inside = !inside;
        }
    }
    return inside;
}

static int16_t cad_find_nearest_polygon(const CadView* view,
                                        const CadViewGeometry* geometry,
                                        int screen_x, int screen_y,
                                        int viewport_x, int viewport_y,
                                        int viewport_w, int viewport_h,
                                        int threshold_pixels)
{
    const double threshold_sq = (double)threshold_pixels * threshold_pixels;
    double best = threshold_sq;
    double best_depth = HUGE_VAL;
    int16_t nearest = -1;
    int local_x = screen_x - viewport_x;
    int local_y = screen_y - viewport_y;
    int i;
    if (!view || !geometry || !geometry->data || local_x < 0 || local_y < 0 ||
        local_x >= viewport_w || local_y >= viewport_h) return -1;
    for (i = 0; i < geometry->data->polygonCount && i < CAD_MAX_POLYGONS; ++i) {
        ProjectedPolygon polygon;
        double distance = HUGE_VAL;
        double hit_depth;
        int edge_index;
        if (!cad_collect_polygon(view, geometry, (int16_t)i, &polygon,
                                 viewport_w, viewport_h)) continue;
        if (cad_paired_member_is_hidden(view, geometry, (int16_t)i, &polygon,
                                        viewport_w, viewport_h)) continue;
        if (polygon.count >= 3 && cad_point_in_polygon(local_x, local_y, &polygon)) {
            distance = 0.0;
        } else {
            for (edge_index = 0; edge_index < polygon.count; ++edge_index) {
                int next = polygon.count == 2 ? 1 - edge_index : (edge_index + 1) % polygon.count;
                double candidate = cad_segment_distance_sq(local_x, local_y,
                                                           polygon.x[edge_index], polygon.y[edge_index],
                                                           polygon.x[next], polygon.y[next]);
                if (candidate < distance) distance = candidate;
                if (polygon.count == 2) break;
            }
        }
        hit_depth = polygon.depth;
        if (distance <= threshold_sq)
            (void)cad_projected_depth_at_point(view, &polygon,
                                               local_x, local_y, &hit_depth);
        if (distance <= threshold_sq &&
            (nearest == -1 || hit_depth < best_depth - 1e-9 ||
             (fabs(hit_depth - best_depth) <= 1e-9 &&
              distance < best - 1e-9))) {
            best = distance;
            best_depth = hit_depth;
            nearest = (int16_t)i;
        }
    }
    return nearest;
}

int16_t CadView_FindNearestPolygon(const CadView* view, const CadCore* core,
                                   int screen_x, int screen_y,
                                   int viewport_x, int viewport_y,
                                   int viewport_w, int viewport_h,
                                   int threshold_pixels)
{
    CadViewGeometry geometry;
    if (!cad_geometry_from_core(core, &geometry)) return -1;
    return cad_find_nearest_polygon(view, &geometry, screen_x, screen_y,
                                    viewport_x, viewport_y,
                                    viewport_w, viewport_h,
                                    threshold_pixels);
}

int16_t CadView_FindNearestScenePolygon(const CadView* view,
                                        const CadScene* scene,
                                        int screen_x, int screen_y,
                                        int viewport_x, int viewport_y,
                                        int viewport_w, int viewport_h,
                                        int threshold_pixels)
{
    CadViewGeometry geometry;
    if (!cad_geometry_from_scene(scene, &geometry)) return -1;
    return cad_find_nearest_polygon(view, &geometry, screen_x, screen_y,
                                    viewport_x, viewport_y,
                                    viewport_w, viewport_h,
                                    threshold_pixels);
}

void CadView_UnprojectPoint(const CadView* view, int screen_x, int screen_y,
                            int viewport_w, int viewport_h,
                            double* out_x, double* out_y, double* out_z)
{
    double px, py;
    if (!out_x || !out_y || !out_z) return;
    *out_x = *out_y = *out_z = 0.0;
    if (!cad_view_parameters_valid(view) ||
        viewport_w <= 0 || viewport_h <= 0) return;
    px = ((double)screen_x - viewport_w * 0.5 - view->pan_x) / view->zoom;
    py = (viewport_h * 0.5 - (double)screen_y - view->pan_y) / view->zoom;
    if (!isfinite(px) || !isfinite(py)) return;
    if (view->type == CAD_VIEW_3D) {
        double scale;
        scale = view->camera_distance / view->focal_length;
        if (!isfinite(scale) || !isfinite(px * scale) || !isfinite(py * scale)) return;
        cad_rotate_from_view(view, px * scale, py * scale, 0.0, out_x, out_y, out_z);
        if (!isfinite(*out_x) || !isfinite(*out_y) || !isfinite(*out_z))
            *out_x = *out_y = *out_z = 0.0;
        return;
    }
    switch (view->type) {
    case CAD_VIEW_TOP:
        *out_x = px; *out_y = 0.0; *out_z = -py;
        break;
    case CAD_VIEW_FRONT:
        *out_x = px; *out_y = py; *out_z = 0.0;
        break;
    case CAD_VIEW_RIGHT:
        *out_x = 0.0; *out_y = py; *out_z = px;
        break;
    default:
        *out_x = px; *out_y = py; *out_z = 0.0;
        break;
    }
}

void CadView_UnprojectDelta(const CadView* view, int screen_dx, int screen_dy,
                            int viewport_w, int viewport_h,
                            double* out_dx, double* out_dy, double* out_dz)
{
    double x0, y0, z0, x1, y1, z1;
    if (!view || !out_dx || !out_dy || !out_dz) return;
    CadView_UnprojectPoint(view, viewport_w / 2, viewport_h / 2,
                           viewport_w, viewport_h, &x0, &y0, &z0);
    CadView_UnprojectPoint(view, viewport_w / 2 + screen_dx, viewport_h / 2 + screen_dy,
                           viewport_w, viewport_h, &x1, &y1, &z1);
    *out_dx = x1 - x0;
    *out_dy = y1 - y0;
    *out_dz = z1 - z0;
    if (!isfinite(*out_dx) || !isfinite(*out_dy) || !isfinite(*out_dz))
        *out_dx = *out_dy = *out_dz = 0.0;
}
