#include "cad_geometry.h"

#include <math.h>
#include <stdio.h>

static int failures;

#define CHECK(expression)                                                     \
    do {                                                                      \
        if (!(expression)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,          \
                    __LINE__, #expression);                                   \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

static void test_first_three_collinear_is_still_planar(void) {
    const double points[][3] = {
        {0.0, 0.0, 4.0}, {1.0, 0.0, 4.0}, {2.0, 0.0, 4.0},
        {2.0, 2.0, 4.0}, {0.0, 2.0, 4.0}
    };
    CHECK(CadGeometry_ClassifyPointChain(points, 5, 0.01) ==
          CAD_POINT_CHAIN_COPLANAR);
}

static void test_polygon_normal_uses_complete_winding(void) {
    const double clockwise[][3] = {
        {0.0, 0.0, 4.0}, {1.0, 0.0, 4.0}, {2.0, 0.0, 4.0},
        {2.0, -2.0, 4.0}, {0.0, -2.0, 4.0}
    };
    const double degenerate[][3] = {
        {0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}, {2.0, 2.0, 2.0}
    };
    double normal[3] = {1.0, 1.0, 1.0};

    CHECK(CadGeometry_ComputePolygonNormal(clockwise, 5, normal));
    CHECK(fabs(normal[0]) < 1e-12);
    CHECK(fabs(normal[1]) < 1e-12);
    CHECK(normal[2] < 0.0);
    CHECK(!CadGeometry_ComputePolygonNormal(degenerate, 3, normal));
    CHECK(normal[0] == 0.0 && normal[1] == 0.0 && normal[2] == 0.0);
    CHECK(!CadGeometry_ComputePolygonNormal(NULL, 3, normal));
    CHECK(!CadGeometry_ComputePolygonNormal(clockwise, 5, NULL));
}

static void test_non_coplanar_and_degenerate_chains(void) {
    const double nonplanar[][3] = {
        {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {2.0, 0.0, 0.0},
        {2.0, 2.0, 0.0}, {0.0, 2.0, 0.02}
    };
    const double collinear[][3] = {
        {-2.0, -2.0, -2.0}, {0.0, 0.0, 0.0},
        {1.0, 1.0, 1.0}, {5.0, 5.0, 5.0}
    };
    const double point_line[][3] = {{3.0, 4.0, 5.0}, {3.0, 4.0, 5.0}};
    const double line[][3] = {{3.0, 4.0, 5.0}, {3.0, 4.0, 6.0}};
    CHECK(CadGeometry_ClassifyPointChain(nonplanar, 5, 0.01) ==
          CAD_POINT_CHAIN_NON_COPLANAR);
    CHECK(CadGeometry_ClassifyPointChain(collinear, 4, 0.01) ==
          CAD_POINT_CHAIN_DEGENERATE);
    CHECK(CadGeometry_ClassifyPointChain(point_line, 2, 0.01) ==
          CAD_POINT_CHAIN_DEGENERATE);
    CHECK(CadGeometry_ClassifyPointChain(line, 2, 0.01) ==
          CAD_POINT_CHAIN_COPLANAR);
}

static void test_invalid_input_and_tolerance(void) {
    const double invalid[][3] = {{0.0, 0.0, 0.0}, {1.0, NAN, 0.0}};
    const double near_planar[][3] = {
        {0.0, 0.0, 0.0}, {2.0, 0.0, 0.0},
        {2.0, 2.0, 0.0}, {0.0, 2.0, 0.005}
    };
    CHECK(CadGeometry_ClassifyPointChain(NULL, 2, 0.01) ==
          CAD_POINT_CHAIN_INVALID);
    CHECK(CadGeometry_ClassifyPointChain(invalid, 2, 0.01) ==
          CAD_POINT_CHAIN_INVALID);
    CHECK(CadGeometry_ClassifyPointChain(near_planar, 4, 0.01) ==
          CAD_POINT_CHAIN_COPLANAR);
}

int main(void) {
    test_first_three_collinear_is_still_planar();
    test_polygon_normal_uses_complete_winding();
    test_non_coplanar_and_degenerate_chains();
    test_invalid_input_and_tolerance();
    if (failures) {
        fprintf(stderr, "%d CAD geometry test(s) failed\n", failures);
        return 1;
    }
    puts("CAD geometry tests passed");
    return 0;
}
