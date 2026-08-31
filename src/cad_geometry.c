#include "cad_geometry.h"

#include <float.h>
#include <math.h>

static double squared_distance(const double a[3], const double b[3]) {
    const double x = b[0] - a[0];
    const double y = b[1] - a[1];
    const double z = b[2] - a[2];
    return x * x + y * y + z * z;
}

static const double* coordinate_at(const double* coordinates, int index) {
    return coordinates + index * 3;
}

int CadGeometry_ComputePolygonNormal(
    const double* coordinates, int count, double normal[3]) {
    double accumulated[3] = {0.0, 0.0, 0.0};
    double length;

    if (!normal) return 0;
    normal[0] = normal[1] = normal[2] = 0.0;
    if (!coordinates || count < 3) return 0;
    for (int i = 0; i < count; ++i) {
        const double* current = coordinate_at(coordinates, i);
        if (!isfinite(current[0]) || !isfinite(current[1]) ||
            !isfinite(current[2])) {
            return 0;
        }
    }

    /* Sum a triangle fan around the first vertex.  Using coordinate
       differences avoids sensitivity to a large world-space translation,
       while summing every triangle preserves the polygon's winding even when
       an early triangle is collinear. */
    for (int i = 1; i + 1 < count; ++i) {
        const double* origin = coordinate_at(coordinates, 0);
        const double* current = coordinate_at(coordinates, i);
        const double* next = coordinate_at(coordinates, i + 1);
        const double ux = current[0] - origin[0];
        const double uy = current[1] - origin[1];
        const double uz = current[2] - origin[2];
        const double vx = next[0] - origin[0];
        const double vy = next[1] - origin[1];
        const double vz = next[2] - origin[2];
        accumulated[0] += uy * vz - uz * vy;
        accumulated[1] += uz * vx - ux * vz;
        accumulated[2] += ux * vy - uy * vx;
    }
    length = sqrt(accumulated[0] * accumulated[0] +
                  accumulated[1] * accumulated[1] +
                  accumulated[2] * accumulated[2]);
    if (!isfinite(length) || length <= DBL_EPSILON) return 0;
    normal[0] = accumulated[0];
    normal[1] = accumulated[1];
    normal[2] = accumulated[2];
    return 1;
}

CadPointChainPlanarity CadGeometry_ClassifyPointChain(
    const double* coordinates, int count, double coplanarTolerance) {
    int first = -1;
    int second = -1;
    double normal[3] = {0.0, 0.0, 0.0};
    double normalLength = 0.0;
    const double distinctThresholdSquared = DBL_EPSILON * DBL_EPSILON;

    if (!coordinates || count < 2 || !isfinite(coplanarTolerance) ||
        coplanarTolerance < 0.0) {
        return CAD_POINT_CHAIN_INVALID;
    }
    for (int i = 0; i < count; ++i) {
        const double* current = coordinate_at(coordinates, i);
        if (!isfinite(current[0]) || !isfinite(current[1]) ||
            !isfinite(current[2])) {
            return CAD_POINT_CHAIN_INVALID;
        }
    }

    for (int i = 0; i < count && first < 0; ++i) {
        for (int j = i + 1; j < count; ++j) {
            if (squared_distance(coordinate_at(coordinates, i),
                                 coordinate_at(coordinates, j)) >
                distinctThresholdSquared) {
                first = i;
                second = j;
                break;
            }
        }
    }
    if (first < 0) return CAD_POINT_CHAIN_DEGENERATE;
    if (count == 2) return CAD_POINT_CHAIN_COPLANAR;

    /* Search the complete chain for a defining plane.  This accepts common
       ordered faces whose first three points happen to be collinear. */
    for (int i = 0; i < count; ++i) {
        if (i == first || i == second) continue;
        const double* first_point = coordinate_at(coordinates, first);
        const double* second_point = coordinate_at(coordinates, second);
        const double* current = coordinate_at(coordinates, i);
        const double ux = second_point[0] - first_point[0];
        const double uy = second_point[1] - first_point[1];
        const double uz = second_point[2] - first_point[2];
        const double vx = current[0] - first_point[0];
        const double vy = current[1] - first_point[1];
        const double vz = current[2] - first_point[2];
        const double nx = uy * vz - uz * vy;
        const double ny = uz * vx - ux * vz;
        const double nz = ux * vy - uy * vx;
        const double length = sqrt(nx * nx + ny * ny + nz * nz);
        if (isfinite(length) && length > DBL_EPSILON) {
            normal[0] = nx;
            normal[1] = ny;
            normal[2] = nz;
            normalLength = length;
            break;
        }
    }
    if (normalLength == 0.0) return CAD_POINT_CHAIN_DEGENERATE;

    for (int i = 0; i < count; ++i) {
        const double* first_point = coordinate_at(coordinates, first);
        const double* current = coordinate_at(coordinates, i);
        const double distance = fabs(
            normal[0] * (current[0] - first_point[0]) +
            normal[1] * (current[1] - first_point[1]) +
            normal[2] * (current[2] - first_point[2])) /
            normalLength;
        if (!isfinite(distance)) return CAD_POINT_CHAIN_INVALID;
        if (distance > coplanarTolerance)
            return CAD_POINT_CHAIN_NON_COPLANAR;
    }
    return CAD_POINT_CHAIN_COPLANAR;
}
