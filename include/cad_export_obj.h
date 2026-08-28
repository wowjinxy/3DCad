#pragma once

#include "cad_core.h"

/* Export CAD data to OBJ/MTL. Two-point polygons are emitted as OBJ lines;
   larger polygons are emitted as faces. Returns zero on validation or I/O
   failure and writes an actionable diagnostic to stderr. */
int CadExport_OBJ(const CadCore* core, const char* filename);

