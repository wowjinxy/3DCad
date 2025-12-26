#pragma once

#include "cad_core.h"

/* Import CAD data from Wavefront OBJ format (.obj)
   Note: Limited support due to SuperFX engine constraints
   - Supports vertices (v) and faces (f)
   - Ignores materials, normals, texture coordinates
   - Maximum 8192 vertices, 4096 polygons
*/
int CadImport_OBJ(CadCore* core, const char* filename);
