#pragma once

/* Standalone Fundoshi animation interchange (3DAN/3DGI).

   These APIs operate exclusively on caller-provided buffers.  Import expands
   global ANM tracks into the per-face point chains used by native X11 CAD
   records; export performs the inverse operation and only merges tracks that
   are identical in every stored frame after recovered coordinate rounding. */

#include "cad_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Decode is transactional: output is changed only after the entire text
   stream has parsed and the generated native topology has validated.  AUTO
   accepts either 3DAN or the recovered legacy 3DGI header. */
CadResult CadAnmCodec_Decode(const uint8_t* bytes, size_t size,
                             CadFormat requestedFormat,
                             CadFileData* output);

/* Encode deterministic CRLF text followed by a DOS 0x1A EOF marker.  3DAN is
   the normal output format; callers may explicitly request 3DGI.  Static-only
   documents export as one frame.  In mixed documents, static faces are held
   across the animated document's complete frame range. */
CadResult CadAnmCodec_Encode(const CadFileData* data, CadFormat format,
                             uint8_t** outputBytes, size_t* outputSize);

/* Validate that native data can be represented by standalone ANM without
   exceeding recovered capacities.  Quantization and game-coordinate range
   issues are returned as warnings rather than errors. */
CadResult CadAnmCodec_Validate(const CadFileData* data);

#ifdef __cplusplus
}
#endif
