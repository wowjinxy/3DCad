#include "cad_view.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    require_close(CadView_NormalizedDepth(16.0), 0.0, 1e-12,
                  "near plane maps to the nearest depth-buffer value");
    require_close(CadView_NormalizedDepth(512.0), 0.96875, 1e-12,
                  "camera depth maps to normalized depth");
    require_true(CadView_NormalizedDepth(512.0) <
                     CadView_NormalizedDepth(1024.0),
                 "near surfaces win the depth test over far surfaces");
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

static int16_t append_positioned_triangle(CadCore* core,
                                          const CadPosition positions[3],
                                          uint8_t color)
{
    int16_t points[3];
    for (int i = 0; i < 3; ++i) {
        points[i] = CadCore_AddPoint(core, positions[i].x,
                                    positions[i].y, positions[i].z);
        if (points[i] < 0) return -1;
    }
    core->data.points[points[0]].nextPoint = points[1];
    core->data.points[points[1]].nextPoint = points[2];
    core->data.points[points[2]].nextPoint = -1;
    return CadCore_AddPolygon(core, points[0], color, 3);
}

static int16_t append_positioned_polygon(CadCore* core,
                                         const CadPosition* positions,
                                         int count, uint8_t color)
{
    int16_t points[CAD_MAX_FACE_POINTS];
    if (!core || !positions || count < 2 || count > CAD_MAX_FACE_POINTS)
        return -1;
    for (int point = 0; point < count; ++point) {
        points[point] = CadCore_AddPoint(core, positions[point].x,
                                        positions[point].y,
                                        positions[point].z);
        if (points[point] < 0) return -1;
    }
    for (int point = 0; point < count; ++point)
        core->data.points[points[point]].nextPoint =
            point + 1 < count ? points[point + 1] : -1;
    return CadCore_AddPolygon(core, points[0], color, (uint8_t)count);
}

static void test_polygon_pick_prefers_exact_interior_over_near_line(void)
{
    const CadPosition face_points[3] = {
        { -20.0, -20.0, 0.0 },
        { 20.0, -20.0, 0.0 },
        { 0.0, 20.0, 0.0 }
    };
    /* At depth 256, Y=3 projects six pixels above the center cursor. */
    const CadPosition line_points[2] = {
        { -20.0, 3.0, -256.0 },
        { 20.0, 3.0, -256.0 }
    };
    CadCore core;
    CadView view;
    int16_t face;
    int16_t line;

    CadCore_Init(&core);
    face = append_positioned_polygon(&core, face_points, 3, 1);
    line = append_positioned_polygon(&core, line_points, 2, 2);
    require_true(face >= 0 && line >= 0,
                 "interior-versus-line pick geometry created");
    CadView_Init(&view, CAD_VIEW_3D);
    view.rot_x = view.rot_y = view.rot_z = 0.0;

    require_true(CadView_FindNearestPolygon(
                     &view, &core, 320, 240, 0, 0, 640, 480, 8) == face,
                 "exact face interior beats a nearer line in the hit halo");
    CadCore_Destroy(&core);
}

static void test_polygon_pick_prefers_exact_line_over_offset_near_line(void)
{
    const CadPosition exact_points[2] = {
        { -30.0, 0.0, 0.0 }, { 30.0, 0.0, 0.0 }
    };
    /* At depth 256, Y=2 projects four pixels above the center cursor. */
    const CadPosition offset_points[2] = {
        { -20.0, 2.0, -256.0 }, { 20.0, 2.0, -256.0 }
    };
    CadCore core;
    CadView view;
    int16_t exact;
    int16_t offset;

    CadCore_Init(&core);
    exact = append_positioned_polygon(&core, exact_points, 2, 1);
    offset = append_positioned_polygon(&core, offset_points, 2, 2);
    require_true(exact >= 0 && offset >= 0,
                 "line-distance pick geometry created");
    CadView_Init(&view, CAD_VIEW_3D);
    view.rot_x = view.rot_y = view.rot_z = 0.0;

    require_true(CadView_FindNearestPolygon(
                     &view, &core, 320, 240, 0, 0, 640, 480, 8) == exact,
                 "exact line beats a nearer offset line in the hit halo");
    CadCore_Destroy(&core);
}

static void test_polygon_pick_keeps_exact_front_line_selectable(void)
{
    const CadPosition face_points[3] = {
        { -20.0, -20.0, 0.0 },
        { 20.0, -20.0, 0.0 },
        { 0.0, 20.0, 0.0 }
    };
    const CadPosition line_points[2] = {
        { -20.0, 0.0, -256.0 }, { 20.0, 0.0, -256.0 }
    };
    CadCore core;
    CadView view;
    int16_t face;
    int16_t line;

    CadCore_Init(&core);
    face = append_positioned_polygon(&core, face_points, 3, 1);
    line = append_positioned_polygon(&core, line_points, 2, 2);
    require_true(face >= 0 && line >= 0,
                 "face-and-exact-line pick geometry created");
    CadView_Init(&view, CAD_VIEW_3D);
    view.rot_x = view.rot_y = view.rot_z = 0.0;

    require_true(CadView_FindNearestPolygon(
                     &view, &core, 320, 240, 0, 0, 640, 480, 8) == line,
                 "an exact foreground line remains selectable over a face");
    CadCore_Destroy(&core);
}

static void test_nonplanar_polygon_pick_matches_rendered_ear_depth(void)
{
    /* Both quads project to the same 80x80 square.  Ear clipping renders the
       lower-left triangle of the warped quad from D/A/B.  At (290,210), that
       triangle is at depth 800, while extrapolating the unrelated A/B/C
       triangle incorrectly reports about depth 221. */
    const CadPosition warped_points[4] = {
        { -15.625, -15.625, -312.0 },
        { 62.5, -62.5, 288.0 },
        { 62.5, 62.5, 288.0 },
        { -62.5, 62.5, 288.0 }
    };
    const CadPosition flat_points[4] = {
        { -31.25, -31.25, -112.0 },
        { 31.25, -31.25, -112.0 },
        { 31.25, 31.25, -112.0 },
        { -31.25, 31.25, -112.0 }
    };
    CadCore core;
    CadView view;
    int16_t warped;
    int16_t flat;

    CadCore_Init(&core);
    warped = append_positioned_polygon(&core, warped_points, 4, 1);
    flat = append_positioned_polygon(&core, flat_points, 4, 2);
    require_true(warped >= 0 && flat >= 0,
                 "non-planar depth pick geometry created");
    CadView_Init(&view, CAD_VIEW_3D);
    view.rot_x = view.rot_y = view.rot_z = 0.0;

    require_true(CadView_FindNearestPolygon(
                     &view, &core, 290, 210, 0, 0, 640, 480, 2) == flat,
                 "picker depth agrees with the rendered ear triangle");
    CadCore_Destroy(&core);
}

static void test_concave_polygon_notch_hit_testing(void)
{
    /* A rectangle with a 20-pixel-wide notch descending from its top edge. */
    const CadPosition concave_points[8] = {
        { -40.0, -40.0, 0.0 }, { 40.0, -40.0, 0.0 },
        { 40.0, 40.0, 0.0 }, { 10.0, 40.0, 0.0 },
        { 10.0, -10.0, 0.0 }, { -10.0, -10.0, 0.0 },
        { -10.0, 40.0, 0.0 }, { -40.0, 40.0, 0.0 }
    };
    CadCore core;
    CadView view;
    int16_t polygon;

    CadCore_Init(&core);
    polygon = append_positioned_polygon(&core, concave_points, 8, 7);
    require_true(polygon >= 0, "concave pick polygon created");
    CadView_Init(&view, CAD_VIEW_FRONT);

    require_true(CadView_FindNearestPolygon(
                     &view, &core, 320, 220, 0, 0, 640, 480, 3) == -1,
                 "concave notch remains a miss");
    require_true(CadView_FindNearestPolygon(
                     &view, &core, 345, 220, 0, 0, 640, 480, 3) == polygon,
                 "concave arm remains an interior hit");
    CadCore_Destroy(&core);
}

static void test_reciprocal_pair_with_collinear_prefix(void)
{
    const CadPosition away_points[5] = {
        { -40.0, -20.0, 0.0 }, { 0.0, -20.0, 0.0 },
        { 40.0, -20.0, 0.0 }, { 40.0, 20.0, 0.0 },
        { -40.0, 20.0, 0.0 }
    };
    const CadPosition facing_points[5] = {
        { -40.0, -20.0, 0.0 }, { -40.0, 20.0, 0.0 },
        { 40.0, 20.0, 0.0 }, { 40.0, -20.0, 0.0 },
        { 0.0, -20.0, 0.0 }
    };
    CadCore core;
    CadView view;
    int16_t away;
    int16_t facing;

    CadCore_Init(&core);
    away = append_positioned_polygon(&core, away_points, 5, 31);
    facing = append_positioned_polygon(&core, facing_points, 5, 63);
    require_true(away >= 0 && facing >= 0,
                 "collinear-prefix reciprocal geometry created");
    core.data.polygons[away].both = facing;
    core.data.polygons[facing].both = away;
    CadView_Init(&view, CAD_VIEW_3D);
    view.rot_x = view.rot_y = view.rot_z = 0.0;
    view.wireframe = 0;

    require_true(CadView_FindNearestPolygon(
                     &view, &core, 320, 240, 0, 0, 640, 480, 3) == facing,
                 "complete face winding selects the camera-facing pair member");
    CadCore_Destroy(&core);
}

static void initialize_pose_from_core(const CadCore* core, CadPose* pose);

static void test_polygon_pick_samples_depth_at_cursor(void)
{
    /* Both triangles have exactly the same screen outline.  The slanted
       triangle crosses the flat one: it is nearer at the left cursor and
       farther at the right.  Its mean vertex depth is 600, so the old
       mean-depth picker incorrectly chose the depth-500 face everywhere. */
    const CadPosition slanted[3] = {
        { -39.0625, -39.0625, -312.0 },
        { 156.25, -156.25, 288.0 },
        { 0.0, 156.25, 288.0 }
    };
    const CadPosition flat[3] = {
        { -97.65625, -97.65625, -12.0 },
        { 97.65625, -97.65625, -12.0 },
        { 0.0, 97.65625, -12.0 }
    };
    CadCore core;
    CadPose pose;
    CadScene scene;
    CadView view;
    int16_t slanted_polygon;
    int16_t flat_polygon;

    CadCore_Init(&core);
    slanted_polygon = append_positioned_triangle(&core, slanted, 1);
    flat_polygon = append_positioned_triangle(&core, flat, 2);
    require_true(slanted_polygon >= 0 && flat_polygon >= 0,
                 "crossing perspective triangles created");
    CadView_Init(&view, CAD_VIEW_3D);
    view.rot_x = view.rot_y = view.rot_z = 0.0;

    require_true(CadView_FindNearestPolygon(
                     &view, &core, 245, 315, 0, 0, 640, 480, 2) ==
                     slanted_polygon,
                 "static picker uses near depth at the left cursor");
    require_true(CadView_FindNearestPolygon(
                     &view, &core, 395, 315, 0, 0, 640, 480, 2) ==
                     flat_polygon,
                 "static picker uses near depth at the right cursor");

    initialize_pose_from_core(&core, &pose);
    scene.topology = &core.data;
    scene.pose = &pose;
    scene.generation = 5;
    require_true(CadView_FindNearestScenePolygon(
                     &view, &scene, 245, 315, 0, 0, 640, 480, 2) ==
                     slanted_polygon,
                 "scene picker uses near depth at the left cursor");
    require_true(CadView_FindNearestScenePolygon(
                     &view, &scene, 395, 315, 0, 0, 640, 480, 2) ==
                     flat_polygon,
                 "scene picker uses near depth at the right cursor");
    CadCore_Destroy(&core);
}

static void test_frame_bounds_orientation_policy(void)
{
    CadView view;
    CadView_Init(&view, CAD_VIEW_3D);
    view.rot_x = 17.0;
    view.rot_y = 123.0;
    view.rot_z = 11.0;
    view.focal_length = 640.0;
    view.zoom = 3.0;
    view.pan_x = 87.0;
    view.pan_y = -42.0;

    require_true(CadView_FrameBoundsPreserveOrientation(
                     &view, -80.0, -45.0, -120.0,
                     150.0, 95.0, 60.0, 1024, 768),
                 "selection bounds fit succeeds at current orientation");
    require_close(view.rot_x, 17.0, 1e-12,
                  "selection fit preserves pitch");
    require_close(view.rot_y, 123.0, 1e-12,
                  "selection fit preserves yaw");
    require_close(view.rot_z, 11.0, 1e-12,
                  "selection fit preserves roll");
    require_close(view.focal_length, 640.0, 1e-12,
                  "selection fit preserves focal length");
    require_true(isfinite(view.zoom) && view.zoom > 0.0 &&
                     isfinite(view.pan_x) && isfinite(view.pan_y) &&
                     isfinite(view.camera_distance),
                 "selection fit recomputes finite camera placement");

    require_true(CadView_FrameBounds(&view, -80.0, -45.0, -120.0,
                                     150.0, 95.0, 60.0, 1024, 768),
                 "home bounds fit succeeds");
    require_close(view.rot_x, -20.0, 1e-12,
                  "home fit restores default pitch");
    require_close(view.rot_y, 30.0, 1e-12,
                  "home fit restores default yaw");
    require_close(view.rot_z, 0.0, 1e-12,
                  "home fit restores default roll");
    require_close(view.focal_length, 512.0, 1e-12,
                  "home fit restores default focal length");
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

static void initialize_pose_from_core(const CadCore* core, CadPose* pose)
{
    memset(pose, 0, sizeof(*pose));
    for (int point = 0; point < core->data.pointCount; ++point) {
        if (!core->data.points[point].flags) continue;
        pose->points[point].x = core->data.points[point].pointx;
        pose->points[point].y = core->data.points[point].pointy;
        pose->points[point].z = core->data.points[point].pointz;
        pose->pointValid[point] = 1;
    }
}

static void set_pose_point(CadPose* pose, int16_t point,
                           double x, double y, double z)
{
    pose->points[point].x = x;
    pose->points[point].y = y;
    pose->points[point].z = z;
    pose->pointValid[point] = 1;
}

static void test_scene_point_picking_in_every_view(void)
{
    static const CadViewType types[] = {
        CAD_VIEW_TOP, CAD_VIEW_FRONT, CAD_VIEW_RIGHT, CAD_VIEW_3D
    };
    CadCore core;
    CadPose pose;
    CadScene scene;
    int16_t first;
    int16_t second;
    int16_t coincident[4];

    CadCore_Init(&core);
    first = CadCore_AddPoint(&core, 140.0, 120.0, 100.0);
    second = CadCore_AddPoint(&core, -120.0, -100.0, -80.0);
    require_true(first >= 0 && second >= 0, "scene point allocation");
    initialize_pose_from_core(&core, &pose);
    set_pose_point(&pose, first, 0.0, 0.0, 0.0);
    set_pose_point(&pose, second, 0.0, 0.0, 0.0);
    scene.topology = &core.data;
    scene.pose = &pose;
    scene.generation = 1;

    for (size_t view_index = 0; view_index < sizeof(types) / sizeof(types[0]);
         ++view_index) {
        CadView view;
        int count;
        CadView_Init(&view, types[view_index]);
        if (types[view_index] == CAD_VIEW_3D)
            view.rot_x = view.rot_y = view.rot_z = 0.0;
        require_true(CadView_FindNearestScenePoint(
                         &view, &scene, 320, 240, 0, 0, 640, 480, 5) == first,
                     "posed point is picked by its stable ID in every view");
        require_true(CadView_FindNearestPoint(
                         &view, &core, 320, 240, 0, 0, 640, 480, 5) == -1,
                     "static coordinates are not substituted for scene picking");
        count = CadView_FindScenePointsAtLocation(
            &view, &scene, 320, 240, 0, 0, 640, 480, 5, 0.001,
            coincident, (int)(sizeof(coincident) / sizeof(coincident[0])));
        require_true(count == 2 && coincident[0] == first &&
                         coincident[1] == second,
                     "coincident posed points are returned in stable-ID order");
    }
    CadCore_Destroy(&core);
}

static int16_t append_pose_polygon(CadCore* core, CadPose* pose,
                                   const CadPosition* positions, int count,
                                   uint8_t color)
{
    int16_t points[CAD_MAX_FACE_POINTS];
    int16_t polygon;
    for (int i = 0; i < count; ++i) {
        points[i] = CadCore_AddPoint(core, 200.0 + i * 20.0,
                                    180.0 + i * 15.0,
                                    160.0 + i * 10.0);
        if (points[i] < 0) return -1;
        set_pose_point(pose, points[i], positions[i].x, positions[i].y,
                       positions[i].z);
    }
    for (int i = 0; i < count; ++i)
        core->data.points[points[i]].nextPoint =
            i + 1 < count ? points[i + 1] : -1;
    polygon = CadCore_AddPolygon(core, points[0], color, (uint8_t)count);
    return polygon;
}

static void test_scene_polygon_and_colored_line_picking(void)
{
    static const CadViewType types[] = {
        CAD_VIEW_TOP, CAD_VIEW_FRONT, CAD_VIEW_RIGHT, CAD_VIEW_3D
    };
    const CadPosition triangle_positions[3] = {
        { -28.0, -22.0, -14.0 },
        { 24.0, -16.0, 19.0 },
        { -7.0, 27.0, 6.0 }
    };
    const CadPosition line_positions[2] = {
        { -30.0, 0.0, 0.0 }, { 30.0, 0.0, 0.0 }
    };
    CadCore core;
    CadPose pose;
    CadScene scene;
    int16_t triangle;
    int16_t line;

    CadCore_Init(&core);
    memset(&pose, 0, sizeof(pose));
    triangle = append_pose_polygon(&core, &pose, triangle_positions, 3, 37);
    require_true(triangle >= 0, "posed triangle allocation");
    scene.topology = &core.data;
    scene.pose = &pose;
    scene.generation = 2;
    for (size_t view_index = 0; view_index < sizeof(types) / sizeof(types[0]);
         ++view_index) {
        CadView view;
        int px[3], py[3];
        int hit_x = 0, hit_y = 0;
        CadView_Init(&view, types[view_index]);
        if (types[view_index] == CAD_VIEW_3D)
            view.rot_x = view.rot_y = view.rot_z = 0.0;
        for (int point = 0; point < 3; ++point) {
            CadView_ProjectPoint(&view, triangle_positions[point].x,
                                 triangle_positions[point].y,
                                 triangle_positions[point].z,
                                 &px[point], &py[point], 640, 480);
            hit_x += px[point];
            hit_y += py[point];
        }
        hit_x /= 3;
        hit_y /= 3;
        require_true(CadView_FindNearestScenePolygon(
                         &view, &scene, hit_x, hit_y,
                         0, 0, 640, 480, 5) == triangle,
                     "posed polygon interior is picked in every view");
    }
    require_true(core.data.polygons[triangle].color == 37,
                 "posed polygon preserves its indexed color");
    CadCore_Destroy(&core);

    CadCore_Init(&core);
    memset(&pose, 0, sizeof(pose));
    line = append_pose_polygon(&core, &pose, line_positions, 2, 91);
    require_true(line >= 0, "posed two-point line allocation");
    scene.topology = &core.data;
    scene.pose = &pose;
    scene.generation = 3;
    for (size_t view_index = 0; view_index < sizeof(types) / sizeof(types[0]);
         ++view_index) {
        CadView view;
        CadView_Init(&view, types[view_index]);
        if (types[view_index] == CAD_VIEW_3D)
            view.rot_x = view.rot_y = view.rot_z = 0.0;
        require_true(CadView_FindNearestScenePolygon(
                         &view, &scene, 320, 240,
                         0, 0, 640, 480, 4) == line,
                     "posed two-point line is pickable in every view");
    }
    require_true(core.data.polygons[line].color == 91,
                 "two-point line retains its palette index");
    CadCore_Destroy(&core);
}

static void test_posed_reciprocal_pair_visibility_switches(void)
{
    const CadPosition positive[3] = {
        { -25.0, -20.0, 0.0 }, { 25.0, -20.0, 0.0 }, { 0.0, 25.0, 0.0 }
    };
    const CadPosition negative[3] = {
        { -25.0, -20.0, 0.0 }, { 0.0, 25.0, 0.0 }, { 25.0, -20.0, 0.0 }
    };
    CadCore core;
    CadPose pose;
    CadScene scene;
    CadView view;
    int16_t front;
    int16_t back;

    CadCore_Init(&core);
    memset(&pose, 0, sizeof(pose));
    front = append_pose_polygon(&core, &pose, positive, 3, 12);
    back = append_pose_polygon(&core, &pose, negative, 3, 78);
    require_true(front >= 0 && back >= 0, "reciprocal pair allocation");
    core.data.polygons[front].both = back;
    core.data.polygons[back].both = front;
    scene.topology = &core.data;
    scene.pose = &pose;
    scene.generation = 4;
    pose.faceNormalValid[front] = pose.faceNormalValid[back] = 1;
    pose.faceNormals[front].z = 1.0;
    pose.faceNormals[back].z = -1.0;
    CadView_Init(&view, CAD_VIEW_3D);
    view.rot_x = view.rot_y = view.rot_z = 0.0;
    view.wireframe = 0;
    require_true(CadView_FindNearestScenePolygon(
                     &view, &scene, 320, 240, 0, 0, 640, 480, 3) == back,
                 "camera-facing reciprocal member supplies the visible color");
    require_true(core.data.polygons[back].color == 78,
                 "first visible reciprocal member has its own indexed color");

    /* Morphing can turn the surface through the camera without changing the
       fixed polygon chains.  Keep the pair geometrically reciprocal while
       swapping their displayed winding. */
    {
        int16_t front_point = core.data.polygons[front].firstPoint;
        int16_t back_point = core.data.polygons[back].firstPoint;
        for (int ordinal = 0; ordinal < 3; ++ordinal) {
            require_true(front_point >= 0 && back_point >= 0,
                         "expected stable pair point IDs");
            set_pose_point(&pose, front_point, negative[ordinal].x,
                           negative[ordinal].y, negative[ordinal].z);
            set_pose_point(&pose, back_point, positive[ordinal].x,
                           positive[ordinal].y, positive[ordinal].z);
            front_point = core.data.points[front_point].nextPoint;
            back_point = core.data.points[back_point].nextPoint;
        }
    }
    pose.faceNormals[front].z = -1.0;
    pose.faceNormals[back].z = 1.0;
    ++scene.generation;
    require_true(CadView_FindNearestScenePolygon(
                     &view, &scene, 320, 240, 0, 0, 640, 480, 3) == front,
                 "reciprocal visibility is recomputed from the morphed pose");
    require_true(core.data.polygons[front].color == 12,
                 "switched reciprocal member supplies its indexed color");
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
    test_polygon_pick_samples_depth_at_cursor();
    test_polygon_pick_prefers_exact_interior_over_near_line();
    test_polygon_pick_prefers_exact_line_over_offset_near_line();
    test_polygon_pick_keeps_exact_front_line_selectable();
    test_nonplanar_polygon_pick_matches_rendered_ear_depth();
    test_concave_polygon_notch_hit_testing();
    test_reciprocal_pair_with_collinear_prefix();
    test_frame_bounds_orientation_policy();
    test_scene_point_picking_in_every_view();
    test_scene_polygon_and_colored_line_picking();
    test_posed_reciprocal_pair_visibility_switches();
    puts("view tests passed");
    return 0;
}
