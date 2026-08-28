#pragma once

#include "cad_core.h"

/* Export Fundoshi-Kun 3DG1 text with grid-coordinate deduplication, colors,
   two-point lines, and a DOS 0x1A EOF marker. */
int CadExport_3DG1(const CadCore* core, const char* filename);

