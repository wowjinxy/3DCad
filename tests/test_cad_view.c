#include "cad_view.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void require_true(int condition, const char* message)
{
    if (!condition) {
        fprintf(stderr, "view test failed: %s\n", message);
        exit(1);
    }
}

static void require_close(double actual, double expected, double tolerance, const char* message)
{
    if (fabs(actual - expected) > tolerance) {
        fprintf(stderr, "view test failed: %s (actual %.6f, expected %.6f)\n",
                message, actual, expected);
        exit(1);
    }
}

static void test_orthographic_round_trip(void)
{
    CadView view;
    double x, y, z;
    int screen_x, screen_y;
    CadView_Init(&view, CAD_VIEW_FRONT);
    CadView_SetZoom(&view, 2.0);
    CadView_Pan(&view, 17.0, -9.0);
    CadView_ProjectPoint(&view, 12.5, -4.0, 99.0, &screen_x, &screen_y, 640, 480);
    CadView_UnprojectPoint(&view, screen_x, screen_y, 640, 480, &x, &y, &z);
    require_close(x, 12.5, 0.26, "front-view X round trip");
    require_close(y, -4.0, 0.26, "front-view Y round trip");
    require_close(z, 0.0, 1e-9, "front-view hidden axis");

    CadView_Init(&view, CAD_VIEW_TOP);
    CadView_SetZoom(&view, 0.75);
    CadView_Pan(&view, -21.0, 13.0);
    CadView_ProjectPoint(&view, -18.0, 77.0, 14.0,
                         &screen_x, &screen_y, 513, 377);
    CadView_UnprojectPoint(&view, screen_x, screen_y, 513, 377, &x, &y, &z);
    require_close(x, -18.0, 0.7, "top-view X round trip");
    require_close(y, 0.0, 1e-9, "top-view hidden axis");
    require_close(z, 14.0, 0.7, "top-view Z round trip");

    CadView_Init(&view, CAD_VIEW_RIGHT);
    CadView_SetZoom(&view, 3.25);
    CadView_Pan(&view, 8.0, 27.0);
    CadView_ProjectPoint(&view, 88.0, -11.0, 6.0,
                         &screen_x, &screen_y, 800, 600);
    CadView_UnprojectPoint(&view, screen_x, screen_y, 800, 600, &x, &y, &z);
    require_close(x, 0.0, 1e-9, "right-view hidden axis");
    require_close(y, -11.0, 0.16, "right-view Y round trip");
    require_close(z, 6.0, 0.16, "right-view Z round trip");
}

static void test_perspective_projection(void)
{
    CadView view;
    double x, y, depth;
    CadView_Init(&view, CAD_VIEW_3D);
    view.rot_x = 0.0;
    view.rot_y = 0.0;
    view.rot_z = 0.0;
    require_true(CadView_ProjectPointDepth(&view, 100.0, 0.0, 0.0,
                                            &x, &y, &depth, 640, 480),
                 "visible perspective point");
    require_close(x, 420.0, 1e-9, "512/z perspective scale");
    require_close(y, 240.0, 1e-9, "perspective center Y");
    require_close(depth, 512.0, 1e-9, "perspective depth");
    require_true(!CadView_ProjectPointDepth(&view, 0.0, 0.0, -500.0,
                                             &x, &y, &depth, 640, 480),
                 "near-plane clipping");

    CadView_RotateRoll(&view, 90.0);
    require_true(CadView_ProjectPointDepth(&view, 100.0, 0.0, 0.0,
                                            &x, &y, &depth, 640, 480),
                 "rolled perspective point");
    require_close(x, 320.0, 0.001, "camera roll X");
    require_close(y, 140.0, 0.001, "camera roll Y");

    view.rot_x = -31.0;
    view.rot_y = 47.0;
    view.rot_z = 19.0;
    view.zoom = 1.6;
    view.pan_x = 23.0;
    view.pan_y = -14.0;
    CadView_UnprojectPoint(&view, 411, 173, 640, 480, &x, &y, &depth);
    require_true(CadView_ProjectPointDepth(&view, x, y, depth,
                                            &x, &y, &depth, 640, 480),
                 "rotated view-plane unprojection is visible");
    require_close(x, 411.0, 1e-6, "rotated/rolled unproject-project X");
    require_close(y, 173.0, 1e-6, "rotated/rolled unproject-project Y");

    require_true(!CadView_ProjectPointDepth(&view, NAN, 0.0, 0.0,
                                             &x, &y, &depth, 640, 480),
                 "NaN model coordinate rejected");
    CadView_SetZoom(&view, NAN);
    require_close(view.zoom, 1.6, 1e-12, "NaN zoom ignored");
}

static void test_extreme_finite_projection_guards(void)
{
    CadCore core;
    CadView view;
    double x, y, depth;
    int screen_x = 0, screen_y = 0;
    int16_t behind, ahead, line;

    CadView_Init(&view, CAD_VIEW_3D);
    view.rot_x = view.rot_y = view.rot_z = 0.0;
    CadView_Rotate(&view, DBL_MAX, 0.0);
    CadView_RotateRoll(&view, DBL_MAX);
    require_true(isfinite(view.rot_y) && view.rot_y >= 0.0 && view.rot_y < 360.0 &&
                 isfinite(view.rot_z) && view.rot_z >= 0.0 && view.rot_z < 360.0,
                 "extreme finite camera rotations normalize without looping");
    view.rot_x = view.rot_y = view.rot_z = 0.0;
    require_true(CadView_ProjectPointDepth(&view, DBL_MAX, 0.0, DBL_MAX,
                                            &x, &y, &depth, 640, 480),
                 "stable divide-first projection accepts a finite extreme ratio");
    require_close(x, 832.0, 1e-9, "finite extreme ratio projects predictably");
    require_close(y, 240.0, 1e-9, "finite extreme ratio keeps screen Y");
    require_true(isfinite(x) && isfinite(y) && isfinite(depth),
                 "extreme perspective output remains finite");

    require_true(!CadView_ProjectPointDepth(&view, DBL_MAX, 0.0, 0.0,
                                             &x, &y, &depth, 640, 480),
                 "unrepresentable perspective screen coordinate rejected");
    CadView_ProjectPoint(&view, DBL_MAX, 0.0, 0.0,
                         &screen_x, &screen_y, 640, 480);
    require_true(screen_x == INT_MIN / 4 && screen_y == INT_MIN / 4,
                 "integer projection returns its invalid sentinel safely");

    CadView_Init(&view, CAD_VIEW_FRONT);
    require_true(!CadView_ProjectPointDepth(&view, DBL_MAX, 0.0, 0.0,
                                             &x, &y, &depth, 640, 480),
                 "unrepresentable orthographic screen coordinate rejected");
    view.pan_x = DBL_MAX;
    require_true(!CadView_ProjectPointDepth(&view, 0.0, 0.0, 0.0,
                                             &x, &y, &depth, 640, 480),
                 "extreme finite pan cannot escape screen bounds");

    /* This line crosses the near plane between -DBL_MAX and DBL_MAX.  A
       direct (end-start) calculation overflows; the normalized clip ratio
       must still place the intersection at the viewport center. */
    CadCore_Init(&core);
    behind = CadCore_AddPoint(&core, -100.0, 0.0, -DBL_MAX);
    ahead = CadCore_AddPoint(&core, 100.0, 0.0, DBL_MAX);
    require_true(behind >= 0 && ahead >= 0, "extreme clipped line points created");
    core.data.points[behind].nextPoint = ahead;
    core.data.points[ahead].nextPoint = -1;
    line = CadCore_AddPolygon(&core, behind, 1, 2);
    require_true(line >= 0, "extreme clipped line created");
    CadView_Init(&view, CAD_VIEW_3D);
    view.rot_x = view.rot_y = view.rot_z = 0.0;
    require_true(CadView_FindNearestPolygon(&view, &core, 320, 240,
                                             0, 0, 640, 480, 2) == line,
                 "normalized extreme clip remains hittable at its true location");
    require_true(CadView_FindNearestPolygon(&view, &core, 160, 240,
                                             0, 0, 640, 480, 2) == -1,
                 "overflowed clip arithmetic cannot create a phantom segment");
    CadCore_Destroy(&core);
}

static void test_world_yaw_preserves_up_axis(void)
{
    CadView yaw_zero;
    CadView yaw_quarter;
    double x0 = 0.0, y0 = 0.0, d0 = 0.0;
    double x1 = 0.0, y1 = 0.0, d1 = 0.0;

    CadView_Init(&yaw_zero, CAD_VIEW_3D);
    yaw_zero.rot_x = -35.0;
    yaw_zero.rot_y = 0.0;
    yaw_zero.rot_z = 0.0;
    yaw_quarter = yaw_zero;
    yaw_quarter.rot_y = 90.0;

    require_true(CadView_ProjectPointDepth(&yaw_zero, 0.0, 100.0, 0.0,
                                            &x0, &y0, &d0, 640, 480) &&
                 CadView_ProjectPointDepth(&yaw_quarter, 0.0, 100.0, 0.0,
                                            &x1, &y1, &d1, 640, 480),
                 "world-up axis projects at both yaw angles");
    require_close(x1, x0, 1e-9, "world-Y yaw does not roll the up axis in X");
    require_close(y1, y0, 1e-9, "world-Y yaw does not twist the pitched horizon");
    require_close(d1, d0, 1e-9, "world-Y yaw preserves up-axis camera depth");
}

static int16_t append_triangle(CadCore* core, double scale, double z,
                               uint8_t color)
{
    int16_t a = CadCore_AddPoint(core, -20.0 * scale, -20.0 * scale, z);
    int16_t b = CadCore_AddPoint(core, 20.0 * scale, -20.0 * scale, z);
    int16_t c = CadCore_AddPoint(core, 0.0, 20.0 * scale, z);
    if (a < 0 || b < 0 || c < 0) return -1;
    core->data.points[a].nextPoint = b;
    core->data.points[b].nextPoint = c;
    core->data.points[c].nextPoint = -1;
    return CadCore_AddPolygon(core, a, color, 3);
}

static void test_perspective_depth_and_clipping_hits(void)
{
    CadCore core;
    CadView view;
    int16_t near_polygon;
    int16_t far_polygon;
    int16_t a, b, c, clipped_polygon;
    CadCore_Init(&core);
    near_polygon = append_triangle(&core, 1.0, 0.0, 1);
    far_polygon = append_triangle(&core, 2.0, 512.0, 2);
    require_true(near_polygon >= 0 && far_polygon >= 0,
                 "overlapping perspective triangles created");
    CadView_Init(&view, CAD_VIEW_3D);
    view.rot_x = view.rot_y = view.rot_z = 0.0;
    require_true(CadView_FindNearestPolygon(&view, &core, 320, 240,
                                             0, 0, 640, 480, 4) == near_polygon,
                 "nearest visible-depth polygon wins an overlapping hit");
    CadCore_Destroy(&core);

    CadCore_Init(&core);
    a = CadCore_AddPoint(&core, 0.0, 10.0, -510.0);
    b = CadCore_AddPoint(&core, -20.0, -20.0, 0.0);
    c = CadCore_AddPoint(&core, 20.0, -20.0, 0.0);
    require_true(a >= 0 && b >= 0 && c >= 0, "near-crossing points created");
    core.data.points[a].nextPoint = b;
    core.data.points[b].nextPoint = c;
    core.data.points[c].nextPoint = -1;
    clipped_polygon = CadCore_AddPolygon(&core, a, 3, 3);
    require_true(clipped_polygon >= 0, "near-crossing polygon created");
    CadView_Init(&view, CAD_VIEW_3D);
    view.rot_x = view.rot_y = view.rot_z = 0.0;
    require_true(CadView_FindNearestPolygon(&view, &core, 320, 240,
                                             0, 0, 640, 480, 4) == clipped_polygon,
                 "polygon crossing the near plane remains hittable");
    CadCore_Destroy(&core);
}

static void test_polygon_hit_testing(void)
{
    CadCore core;
    CadView view;
    int16_t a, b, c, polygon;
    CadCore_Init(&core);
    a = CadCore_AddPoint(&core, -20.0, -20.0, 0.0);
    b = CadCore_AddPoint(&core, 20.0, -20.0, 0.0);
    c = CadCore_AddPoint(&core, 0.0, 20.0, 0.0);
    require_true(a >= 0 && b >= 0 && c >= 0, "triangle point allocation");
    core.data.points[a].nextPoint = b;
    core.data.points[b].nextPoint = c;
    core.data.points[c].nextPoint = -1;
    polygon = CadCore_AddPolygon(&core, a, 7, 3);
    require_true(polygon >= 0, "triangle polygon allocation");
    CadView_Init(&view, CAD_VIEW_FRONT);
    require_true(CadView_FindNearestPolygon(&view, &core, 320, 240,
                                             0, 0, 640, 480, 6) == polygon,
                 "polygon interior hit");
    require_true(CadView_FindNearestPolygon(&view, &core, 600, 400,
                                             0, 0, 640, 480, 6) == -1,
                 "polygon miss");
    CadCore_Destroy(&core);
}

int main(void)
{
    test_orthographic_round_trip();
    test_perspective_projection();
    test_extreme_finite_projection_guards();
    test_world_yaw_preserves_up_axis();
    test_polygon_hit_testing();
    test_perspective_depth_and_clipping_hits();
    puts("view tests passed");
    return 0;
}
