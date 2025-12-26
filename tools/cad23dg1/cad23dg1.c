/* Simple CLI converter: .cad -> .txt
 * Usage: cad23dg1 <input.cad> [output.txt]
 */
// A little CLI frontend so I can use the existing components to convert Iwamoto 3D-CAD files to Fundoshi-Kun format - Sunlit

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cad_file.h"
#include "cad_export_3dg1.h"
#include "cad_core.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input.cad> [output.txt]\n", argc > 0 ? argv[0] : "cad23dg1");
        return 1;
    }

    const char* inpath = argv[1];
    char outpath[1024];

    if (argc >= 3) {
        strncpy(outpath, argv[2], sizeof(outpath) - 1);
        outpath[sizeof(outpath) - 1] = '\0';
    } else {
        /* generate output filename by replacing extension with .txt */
        const char* dot = strrchr(inpath, '.');
        if (dot) {
            size_t len = (size_t)(dot - inpath);
            if (len >= sizeof(outpath)) len = sizeof(outpath) - 1;
            memcpy(outpath, inpath, len);
            outpath[len] = '\0';
            strcat(outpath, ".txt");
        } else {
            strncpy(outpath, inpath, sizeof(outpath) - 1);
            outpath[sizeof(outpath) - 1] = '\0';
            strcat(outpath, ".txt");
        }
    }

    CadCore core;
    memset(&core, 0, sizeof(core));

    if (!CadFile_Load(inpath, &core.data)) {
        fprintf(stderr, "Failed to load CAD file '%s'\n", inpath);
        return 2;
    }

    if (!CadExport_3DG1(&core, outpath)) {
        fprintf(stderr, "Failed to export Fundoshi-Kun file '%s'\n", outpath);
        return 3;
    }

    return 0;
}
