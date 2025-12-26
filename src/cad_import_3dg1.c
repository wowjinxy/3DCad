#define _CRT_SECURE_NO_WARNINGS

#include "cad_import_3dg1.h"
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

/* Import CAD data from Fundoshi-Kun format (.3dg1) */
int CadImport_3DG1(CadCore* core, const char* filename) {
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

    /* Read magic header */
    char magic[16];
    if (!fgets(magic, sizeof(magic), fp)) {
        fprintf(stderr, "Error: Could not read header from '%s'\n", filename);
        fclose(fp);
        return 0;
    }
    
    /* Remove newline */
    char* nl = strchr(magic, '\n');
    if (nl) *nl = '\0';
    nl = strchr(magic, '\r');
    if (nl) *nl = '\0';
    
    if (strcmp(magic, "3DG1") != 0) {
        fprintf(stderr, "Error: Invalid file format - expected '3DG1', got '%s'\n", magic);
        fclose(fp);
        return 0;
    }

    /* Read vertex count */
    int vertex_count = 0;
    if (fscanf(fp, "%d", &vertex_count) != 1) {
        fprintf(stderr, "Error: Could not read vertex count\n");
        fclose(fp);
        return 0;
    }
    
    if (vertex_count <= 0 || vertex_count > CAD_MAX_POINTS) {
        fprintf(stderr, "Error: Invalid vertex count: %d\n", vertex_count);
        fclose(fp);
        return 0;
    }
    
    fprintf(stdout, "Importing 3DG1: %d vertices\n", vertex_count);

    /* Read vertices */
    int16_t* point_indices = (int16_t*)malloc(vertex_count * sizeof(int16_t));
    if (!point_indices) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        fclose(fp);
        return 0;
    }
    
    for (int i = 0; i < vertex_count; i++) {
        double x, y, z;
        if (fscanf(fp, "%lf %lf %lf", &x, &y, &z) != 3) {
            fprintf(stderr, "Error: Could not read vertex %d\n", i);
            free(point_indices);
            fclose(fp);
            return 0;
        }
        
        int16_t pt_idx = CadCore_AddPoint(core, x, y, z);
        if (pt_idx < 0) {
            fprintf(stderr, "Error: Failed to add point %d\n", i);
            free(point_indices);
            fclose(fp);
            return 0;
        }
        point_indices[i] = pt_idx;
    }

    /* Read faces */
    int face_count = 0;
    char line[1024];
    
    /* Skip to faces section (skip blank lines) */
    while (fgets(line, sizeof(line), fp)) {
        /* Skip empty lines and whitespace-only lines */
        char* p = line;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == '\0') continue;
        
        /* Check for EOF marker */
        if (*p == '\x1a') break;
        
        /* Parse face: count v0 v1 v2 ... color */
        int count;
        if (sscanf(p, "%d", &count) != 1) {
            continue; /* Skip invalid lines */
        }
        
        if (count < 2 || count > 12) {
            fprintf(stderr, "Warning: Skipping face with invalid vertex count: %d\n", count);
            continue;
        }
        
        /* Parse vertex indices and color */
        int indices[12];
        int color = 0;
        
        /* Skip the count in the string */
        while (*p && (*p == '-' || (*p >= '0' && *p <= '9'))) p++;
        while (*p == ' ' || *p == '\t') p++;
        
        int parsed = 0;
        for (int i = 0; i < count && *p; i++) {
            if (sscanf(p, "%d", &indices[i]) != 1) break;
            parsed++;
            /* Skip to next number */
            while (*p && (*p == '-' || (*p >= '0' && *p <= '9'))) p++;
            while (*p == ' ' || *p == '\t') p++;
        }
        
        /* Read color (last number) */
        if (*p) {
            sscanf(p, "%d", &color);
        }
        
        if (parsed != count) {
            fprintf(stderr, "Warning: Skipping face with mismatched vertex count\n");
            continue;
        }
        
        /* Validate indices */
        int valid = 1;
        for (int i = 0; i < count; i++) {
            if (indices[i] < 0 || indices[i] >= vertex_count) {
                fprintf(stderr, "Warning: Skipping face with invalid vertex index: %d\n", indices[i]);
                valid = 0;
                break;
            }
        }
        if (!valid) continue;
        
        /* Create polygon - each polygon gets its own copy of points */
        int16_t first_point = INVALID_INDEX;
        int16_t prev_point = INVALID_INDEX;
        
        for (int i = 0; i < count; i++) {
            /* Get original point coordinates */
            CadPoint* orig = CadCore_GetPoint(core, point_indices[indices[i]]);
            if (!orig) continue;
            
            /* Create new point for this polygon */
            int16_t new_pt = CadCore_AddPoint(core, orig->pointx, orig->pointy, orig->pointz);
            if (new_pt == INVALID_INDEX) continue;
            
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
        
        /* Add polygon */
        if (first_point != INVALID_INDEX) {
            int16_t poly_idx = CadCore_AddPolygon(core, first_point, (uint8_t)color, (uint8_t)count);
            if (poly_idx >= 0) {
                face_count++;
            }
        }
    }
    
    free(point_indices);
    fclose(fp);
    
    fprintf(stdout, "Imported 3DG1: %d vertices, %d faces\n", vertex_count, face_count);
    return 1;
}
