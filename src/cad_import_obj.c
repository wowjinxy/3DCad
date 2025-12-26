#define _CRT_SECURE_NO_WARNINGS

#include "cad_import_obj.h"
#include "cad_core.h"
#include "cad_file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

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

/* Import CAD data from Wavefront OBJ format (.obj) */
int CadImport_OBJ(CadCore* core, const char* filename) {
    if (!core || !filename) return 0;
    
    FILE* fp = NULL;

#ifdef _WIN32
    /* Convert UTF-8 filename to wide string for Windows */
    int wlen = MultiByteToWideChar(CP_UTF8, 0, filename, -1, NULL, 0);
    if (wlen > 0) {
        wchar_t* wfilename = (wchar_t*)calloc(wlen, sizeof(wchar_t));
        if (wfilename) {
            MultiByteToWideChar(CP_UTF8, 0, filename, -1, wfilename, wlen);
            fp = _wfopen(wfilename, L"r");
            free(wfilename);
        }
    }
    if (!fp) {
        fp = fopen(filename, "r");
    }
#else
    fp = fopen(filename, "r");
#endif
    
    if (!fp) {
        fprintf(stderr, "Error: Could not open file '%s' for reading\n", filename);
        return 0;
    }

    /* Clear existing data */
    CadCore_Clear(core);

    /* Read vertices first, then faces */
    double* vertices = (double*)malloc(CAD_MAX_POINTS * 3 * sizeof(double));
    if (!vertices) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        fclose(fp);
        return 0;
    }
    
    int vertex_count = 0;
    int face_count = 0;
    char line[1024];
    
    /* First pass: read all vertices */
    while (fgets(line, sizeof(line), fp) && vertex_count < CAD_MAX_POINTS) {
        /* Skip whitespace */
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        
        /* Skip empty lines and comments */
        if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#') continue;
        
        /* Check for vertex */
        if (*p == 'v' && (p[1] == ' ' || p[1] == '\t')) {
            p++; /* Skip 'v' */
            while (*p == ' ' || *p == '\t') p++;
            
            double x, y, z;
            if (sscanf(p, "%lf %lf %lf", &x, &y, &z) == 3) {
                vertices[vertex_count * 3 + 0] = x;
                vertices[vertex_count * 3 + 1] = y;
                vertices[vertex_count * 3 + 2] = z;
                vertex_count++;
            }
        }
    }
    
    if (vertex_count == 0) {
        fprintf(stderr, "Error: No vertices found in OBJ file\n");
        free(vertices);
        fclose(fp);
        return 0;
    }
    
    fprintf(stdout, "Importing OBJ: %d vertices\n", vertex_count);
    
    /* Add vertices to CAD system */
    int16_t* point_indices = (int16_t*)malloc(vertex_count * sizeof(int16_t));
    if (!point_indices) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        free(vertices);
        fclose(fp);
        return 0;
    }
    
    for (int i = 0; i < vertex_count; i++) {
        int16_t pt_idx = CadCore_AddPoint(core, 
            vertices[i * 3 + 0],
            vertices[i * 3 + 1],
            vertices[i * 3 + 2]);
        if (pt_idx < 0) {
            fprintf(stderr, "Warning: Failed to add vertex %d (limit reached)\n", i);
            break;
        }
        point_indices[i] = pt_idx;
    }
    
    free(vertices);
    
    /* Second pass: read faces */
    rewind(fp);
    
    while (fgets(line, sizeof(line), fp) && face_count < CAD_MAX_POLYGONS) {
        /* Skip whitespace */
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        
        /* Skip empty lines and comments */
        if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#') continue;
        
        /* Check for face */
        if (*p == 'f' && (p[1] == ' ' || p[1] == '\t')) {
            p++; /* Skip 'f' */
            while (*p == ' ' || *p == '\t') p++;
            
            /* Parse face indices (OBJ uses 1-based, may include texture/normal: v/vt/vn) */
            int indices[12];
            int count = 0;
            
            while (*p && count < 12) {
                /* Skip to start of number */
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '\0' || *p == '\n' || *p == '\r') break;
                
                /* Parse vertex index (may be followed by /texture/normal) */
                int idx = 0;
                int negative = 0;
                if (*p == '-') {
                    negative = 1;
                    p++;
                }
                
                if (*p >= '0' && *p <= '9') {
                    while (*p >= '0' && *p <= '9') {
                        idx = idx * 10 + (*p - '0');
                        p++;
                    }
                    if (negative) idx = -idx;
                    
                    /* OBJ uses 1-based indexing, convert to 0-based */
                    idx--;
                    
                    /* Handle negative indices (relative to end) */
                    if (idx < 0) {
                        idx = vertex_count + idx;
                    }
                    
                    if (idx >= 0 && idx < vertex_count) {
                        indices[count++] = idx;
                    }
                    
                    /* Skip texture/normal indices if present */
                    if (*p == '/') {
                        p++;
                        /* Skip texture index */
                        while (*p >= '0' && *p <= '9') p++;
                        if (*p == '/') {
                            p++;
                            /* Skip normal index */
                            while (*p >= '0' && *p <= '9') p++;
                        }
                    }
                } else {
                    break;
                }
            }
            
            if (count < 2) {
                continue; /* Skip invalid faces */
            }
            
            if (count > 12) {
                fprintf(stderr, "Warning: Face with %d vertices exceeds limit (12), truncating\n", count);
                count = 12;
            }
            
            /* Create polygon - each polygon gets its own copy of points */
            int16_t first_point = INVALID_INDEX;
            int16_t prev_point = INVALID_INDEX;
            
            for (int i = 0; i < count; i++) {
                /* Get original point coordinates */
                CadPoint* orig = CadCore_GetPoint(core, point_indices[indices[i]]);
                if (!orig) continue;
                
                /* Create new point for this polygon */
                int16_t new_pt = CadCore_AddPoint(core, orig->pointx, orig->pointy, orig->pointz);
                if (new_pt == INVALID_INDEX) {
                    fprintf(stderr, "Warning: Failed to add point for face (limit reached)\n");
                    break;
                }
                
                if (first_point == INVALID_INDEX) {
                    first_point = new_pt;
                }
                
                if (prev_point != INVALID_INDEX) {
                    CadPoint* prev = CadCore_GetPoint(core, prev_point);
                    if (prev) prev->nextPoint = new_pt;
                }
                
                prev_point = new_pt;
            }
            
            /* Close the chain */
            if (prev_point != INVALID_INDEX) {
                CadPoint* last = CadCore_GetPoint(core, prev_point);
                if (last) last->nextPoint = INVALID_INDEX;
            }
            
            /* Add polygon with default color */
            if (first_point != INVALID_INDEX) {
                int16_t poly_idx = CadCore_AddPolygon(core, first_point, 0, (uint8_t)count);
                if (poly_idx >= 0) {
                    face_count++;
                } else {
                    fprintf(stderr, "Warning: Failed to add polygon (limit reached)\n");
                    break;
                }
            }
        }
    }
    
    free(point_indices);
    fclose(fp);
    
    fprintf(stdout, "Imported OBJ: %d vertices, %d faces\n", vertex_count, face_count);
    
    if (face_count == 0) {
        fprintf(stderr, "Warning: No faces found in OBJ file\n");
    }
    
    return (face_count > 0) ? 1 : 0;
}
