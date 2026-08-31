#ifndef CAD_GEOMETRY_H
#define CAD_GEOMETRY_H

/* Small, portable geometry predicates shared by editor previews and command
   validation.  These functions deliberately have no SDL or document-state
   dependencies so the exact interaction rule can be unit tested. */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum CadPointChainPlanarity {
    CAD_POINT_CHAIN_DEGENERATE = 0,
    CAD_POINT_CHAIN_COPLANAR,
    CAD_POINT_CHAIN_NON_COPLANAR,
    CAD_POINT_CHAIN_INVALID
} CadPointChainPlanarity;

/* Computes the unnormalized, winding-preserving area normal for an ordered
   polygon stored as packed XYZ triples.  The complete chain contributes to
   the result, so valid faces may start with collinear vertices.  Returns
   nonzero for a finite, non-degenerate polygon.  On failure, normal is set to
   {0, 0, 0}. */
int CadGeometry_ComputePolygonNormal(
    const double* coordinates, int count, double normal[3]);

/* Classifies an ordered point chain.  A two-point chain is a valid colored
   line when its endpoints differ.  A face with three or more points is
   degenerate only when no non-collinear triple exists; the defining plane is
   not assumed to come from the first three vertices. */
CadPointChainPlanarity CadGeometry_ClassifyPointChain(
    const double* coordinates, int count, double coplanarTolerance);

#ifdef __cplusplus
}
#endif

#endif
