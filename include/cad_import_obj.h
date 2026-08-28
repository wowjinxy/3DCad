#pragma once

#include "cad_core.h"

/* Import Wavefront OBJ geometry transactionally.
   Supports vertices, faces, line records (polylines become two-point
   segments), negative/relative indices, and material_N color names. Texture
   coordinates and normals are syntax-checked but are not retained by the CAD
   model. On failure, core is left unchanged and a diagnostic is written to
   stderr. */
int CadImport_OBJ(CadCore* core, const char* filename);
