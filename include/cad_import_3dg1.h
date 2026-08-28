#pragma once

#include "cad_core.h"

/* Import Fundoshi-Kun 3DG1 text transactionally. DOS 0x1A EOF markers are
   accepted. On failure, core is left unchanged and a diagnostic is written
   to stderr. */
int CadImport_3DG1(CadCore* core, const char* filename);
