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

/* Convert color index (0-255) to RGB values */
static void color_index_to_rgb(uint8_t color_idx, float* r, float* g, float* b) {
    /* Simple palette: map color index to RGB
       For now, use a grayscale mapping, but can be extended to use actual palette */
    float intensity = (float)color_idx / 255.0f;
    *r = intensity;
    *g = intensity;
    *b = intensity;
    
    /* Alternative: use a simple color palette based on index */
    /* You can customize this to match your actual color palette */
    if (color_idx < 16) {
        /* First 16 colors: grayscale */
        float gray = (float)color_idx / 15.0f;
        *r = gray; *g = gray; *b = gray;
    } else {
        /* Map remaining colors to a simple palette */
        int hue = (color_idx - 16) % 6;
        float sat = 0.7f;
        float val = 0.8f;
        
        switch (hue) {
        case 0: *r = val; *g = val * (1 - sat); *b = val * (1 - sat); break; /* Red */
        case 1: *r = val * (1 - sat); *g = val; *b = val * (1 - sat); break; /* Green */
        case 2: *r = val * (1 - sat); *g = val * (1 - sat); *b = val; break; /* Blue */
        case 3: *r = val; *g = val; *b = val * (1 - sat); break; /* Yellow */
        case 4: *r = val; *g = val * (1 - sat); *b = val; break; /* Magenta */
        case 5: *r = val * (1 - sat); *g = val; *b = val; break; /* Cyan */
        default: *r = val; *g = val; *b = val; break; /* White */
        }
    }
}

/* Export CAD data to OBJ format with MTL materials */
int CadExport_OBJ(const CadCore* core, const char* filename) {
    if (!core || !filename) return 0;
    
    FILE* fp_obj = NULL;
    FILE* fp_mtl = NULL;
    
    /* Generate MTL filename from OBJ filename */
    char mtl_filename[MAX_PATH];
    strncpy(mtl_filename, filename, MAX_PATH - 1);
    mtl_filename[MAX_PATH - 1] = '\0';
    char* ext = strrchr(mtl_filename, '.');
    if (ext) {
        strcpy(ext, ".mtl");
    } else {
        strcat(mtl_filename, ".mtl");
    }
    
    /* Extract just the MTL filename (without path) for mtllib reference */
    char mtl_basename[256];
    const char* mtl_slash = strrchr(mtl_filename, '/');
    if (!mtl_slash) mtl_slash = strrchr(mtl_filename, '\\');
    if (mtl_slash) {
        strncpy(mtl_basename, mtl_slash + 1, sizeof(mtl_basename) - 1);
    } else {
        strncpy(mtl_basename, mtl_filename, sizeof(mtl_basename) - 1);
    }
    mtl_basename[sizeof(mtl_basename) - 1] = '\0';
    
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
    
    /* Open MTL file */
    wlen = MultiByteToWideChar(CP_UTF8, 0, mtl_filename, -1, NULL, 0);
    if (wlen > 0) {
        wchar_t* wmtl_filename = (wchar_t*)calloc(wlen, sizeof(wchar_t));
        if (wmtl_filename) {
            MultiByteToWideChar(CP_UTF8, 0, mtl_filename, -1, wmtl_filename, wlen);
            fp_mtl = _wfopen(wmtl_filename, L"w");
            free(wmtl_filename);
        }
    }
    if (!fp_mtl) {
        fp_mtl = fopen(mtl_filename, "w");
    }
#else
    fp_obj = fopen(filename, "w");
    fp_mtl = fopen(mtl_filename, "w");
#endif
    
    if (!fp_obj) {
        fprintf(stderr, "Error: Could not open file '%s' for writing\n", filename);
        return 0;
    }
    if (!fp_mtl) {
        fprintf(stderr, "Error: Could not open MTL file '%s' for writing\n", mtl_filename);
        fclose(fp_obj);
        return 0;
    }
    
    /* Write OBJ header */
    fprintf(fp_obj, "# OBJ file exported from 3DCadGui\n");
    fprintf(fp_obj, "# Points: %d, Polygons: %d\n", core->data.pointCount, core->data.polygonCount);
    fprintf(fp_obj, "mtllib %s\n", mtl_basename);
    fprintf(fp_obj, "\n");
    
    /* Write MTL header */
    fprintf(fp_mtl, "# MTL file exported from 3DCadGui\n");
    fprintf(fp_mtl, "# Material library for %s\n", mtl_basename);
    fprintf(fp_mtl, "\n");
    
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
    
    /* Step 2: Write all vertices */
    for (int i = 0; i < core->data.pointCount && i < CAD_MAX_POINTS; i++) {
        const CadPoint* pt = &core->data.points[i];
        if (pt->flags != 0) {
            fprintf(fp_obj, "v %.6f %.6f %.6f\n", pt->pointx, pt->pointy, pt->pointz);
        }
    }
    
    fprintf(fp_obj, "\n");
    
    /* Step 3: Collect unique colors and create materials */
    uint8_t used_colors[256];
    int color_count = 0;
    int color_map[256]; /* Maps color index to material index */
    
    /* Initialize color map */
    for (int i = 0; i < 256; i++) {
        color_map[i] = -1;
    }
    
    /* Find all unique colors used in polygons */
    for (int i = 0; i < core->data.polygonCount && i < CAD_MAX_POLYGONS; i++) {
        const CadPolygon* poly = &core->data.polygons[i];
        if (poly->flags == 0 || poly->npoints < CAD_MIN_FACE_POINTS) continue;
        
        uint8_t color_idx = poly->color;
        if (color_map[color_idx] == -1) {
            color_map[color_idx] = color_count;
            used_colors[color_count++] = color_idx;
        }
    }
    
    /* Write materials to MTL file */
    for (int i = 0; i < color_count; i++) {
        uint8_t color_idx = used_colors[i];
        float r, g, b;
        color_index_to_rgb(color_idx, &r, &g, &b);
        
        fprintf(fp_mtl, "newmtl material_%d\n", color_idx);
        fprintf(fp_mtl, "Ka %.3f %.3f %.3f\n", r * 0.2f, g * 0.2f, b * 0.2f); /* Ambient */
        fprintf(fp_mtl, "Kd %.3f %.3f %.3f\n", r, g, b); /* Diffuse */
        fprintf(fp_mtl, "Ks %.3f %.3f %.3f\n", 0.5f, 0.5f, 0.5f); /* Specular */
        fprintf(fp_mtl, "Ns 32.0\n"); /* Shininess */
        fprintf(fp_mtl, "d 1.0\n"); /* Dissolve (opacity) */
        fprintf(fp_mtl, "\n");
    }
    
    fclose(fp_mtl);
    
    /* Step 4: Write all faces (polygons) with material assignments */
    uint8_t current_material = 255; /* Invalid, will force first material to be set */
    
    for (int i = 0; i < core->data.polygonCount && i < CAD_MAX_POLYGONS; i++) {
        const CadPolygon* poly = &core->data.polygons[i];
        if (poly->flags == 0 || poly->npoints < CAD_MIN_FACE_POINTS) continue;
        
        /* Set material if it changed */
        if (poly->color != current_material) {
            current_material = poly->color;
            fprintf(fp_obj, "usemtl material_%d\n", current_material);
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
        
        /* Write face if we have at least 2 vertices */
        if (point_count >= CAD_MIN_FACE_POINTS) {
            fprintf(fp_obj, "f");
            for (int j = 0; j < point_count; j++) {
                fprintf(fp_obj, " %d", point_indices[j]);
            }
            fprintf(fp_obj, "\n");
        }
    }
    
    fclose(fp_obj);
    fprintf(stdout, "Exported OBJ file: %s (%d vertices, %d faces, %d materials)\n", 
            filename, vertex_count, core->data.polygonCount, color_count);
    fprintf(stdout, "Exported MTL file: %s\n", mtl_filename);
    return 1;
}

