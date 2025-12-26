#define _CRT_SECURE_NO_WARNINGS

#include "cad_core.h"
#include "cad_file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#ifndef CP_UTF8
#define CP_UTF8 65001
#endif
#endif

/* Export CAD data to Fundoshi-Kun format */
int CadExport_3DG1(const CadCore* core, const char* filename) {
    if (!core || !filename) return 0;
    
    FILE* fp_obj = NULL;

#ifdef _WIN32
    /* Convert UTF-8 filename to wide string for Windows */
    int wlen = MultiByteToWideChar(CP_UTF8, 0, filename, -1, NULL, 0);
    if (wlen > 0) {
        wchar_t* wfilename = (wchar_t*)calloc(wlen, sizeof(wchar_t));
        if (wfilename) {
            MultiByteToWideChar(CP_UTF8, 0, filename, -1, wfilename, wlen);
            fp_obj = _wfopen(wfilename, L"w");
            free(wfilename);
        }
    }
    if (!fp_obj) {
        fp_obj = fopen(filename, "w");
    }
#else
    fp_obj = fopen(filename, "w");
#endif
    
    if (!fp_obj) {
        fprintf(stderr, "Error: Could not open file '%s' for writing\n", filename);
        return 0;
    }

    /* Step 1: Collect all valid points and create index mapping */
    int point_to_vertex[CAD_MAX_POINTS];
    int vertex_count = 0;
    
    for (int i = 0; i < core->data.pointCount && i < CAD_MAX_POINTS; i++) {
        const CadPoint* pt = &core->data.points[i];
        if (pt->flags != 0) {
            point_to_vertex[i] = vertex_count + 1; /* OBJ uses 1-based indexing */
            vertex_count++;
        } else {
            point_to_vertex[i] = -1; /* Invalid point */
        }
    }

    /* Write Fundoshi-Kun header */
    fprintf(fp_obj, "3DG1\n"); // 3DG1 magic
    fprintf(fp_obj, "%d\n", vertex_count); // total points (counting from 1)
    
    /* Step 2: Write all vertices */
    for (int i = 0; i < core->data.pointCount && i < CAD_MAX_POINTS; i++) {
        const CadPoint* pt = &core->data.points[i];
        if (pt->flags != 0) {
            //fprintf(fp_obj, "%.6f %.6f %.6f\n", pt->pointx, pt->pointy, pt->pointz);
            fprintf(fp_obj, "%.f %.f %.f\n", pt->pointx, pt->pointy, pt->pointz); // we don't need all this precision
        }
    }
    
    fprintf(fp_obj, "\n");
    
    /* Step 2: Write all faces (polygons) with material assignments */
    uint8_t current_material = 255; /* Invalid, will force first material to be set */
    
    for (int i = 0; i < core->data.polygonCount && i < CAD_MAX_POLYGONS; i++) {
        const CadPolygon* poly = &core->data.polygons[i];
        if (poly->flags == 0 || poly->npoints < 3) continue;
        
        /* Set material if it changed */
        if (poly->color != current_material) {
            current_material = poly->color;
        }
        
        /* Collect polygon vertices */
        int16_t point_indices[256];
        int point_count = 0;
        int16_t current = poly->firstPoint;
        
        while (current >= 0 && current < CAD_MAX_POINTS && point_count < 256) {
            if (current >= core->data.pointCount) break;
            const CadPoint* pt = &core->data.points[current];
            if (pt->flags == 0) break;
            
            int vertex_idx = point_to_vertex[current];
            if (vertex_idx > 0) {
                point_indices[point_count++] = (int16_t)vertex_idx;
            }
            
            current = pt->nextPoint;
            if (point_count > 1000) break; /* Safety limit */
        }
        
        /* Write face if we have at least 3 vertices */
        if (point_count >= 3) {
            fprintf(fp_obj, "%d", point_count); // Fundoshi-Kun needs number of points at start of face entry
            for (int j = 0; j < point_count; j++) {
                int point_idx = (point_indices[j] - 1);  // Fundoshi-Kun vert references are 0 indexed
                fprintf(fp_obj, " %d", point_idx);
            }
            fprintf(fp_obj, " %d", current_material); // Fundoshi-Kun needs color/texture index at end of face entry
            fprintf(fp_obj, "\n");
        }
    }
    
    fclose(fp_obj);
    return 1;
}

