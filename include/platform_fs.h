#pragma once

#include "cad_codec.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Platform filesystem services are deliberately separate from every codec.
   Paths are UTF-8, reads are bounded, and replacement writes are atomic. */
#define CAD_PLATFORM_PATH_BYTE_LIMIT 32768u
#define CAD_PLATFORM_DEFAULT_FILE_LIMIT (64u * 1024u * 1024u)

CadResult CadPlatform_ReadFile(const char* utf8Path, size_t sizeLimit,
                               uint8_t** outputBytes, size_t* outputSize);
CadResult CadPlatform_WriteFileAtomic(const char* utf8Path,
                                      const uint8_t* bytes, size_t size);
/* Returns non-zero when two bounded UTF-8 paths resolve to the same file.
   Existing files are compared by identity; lexical absolute paths provide a
   fallback when one side does not currently exist. */
int CadPlatform_PathsEqual(const char* firstUtf8Path,
                           const char* secondUtf8Path);
void CadPlatform_Free(void* allocation);

#ifdef __cplusplus
}
#endif
