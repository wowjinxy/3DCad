#define _CRT_SECURE_NO_WARNINGS

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "cad_export_3dg1.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#ifndef CP_UTF8
#define CP_UTF8 65001
#endif
#else
#include <unistd.h>
#endif

typedef struct {
    int x;
    int y;
    int z;
} ThreeDg1ExportVertex;

typedef struct {
    int vertex[CAD_MAX_FACE_POINTS];
    int count;
    uint8_t color;
} ThreeDg1ExportFace;

static FILE* open_utf8_file(const char* filename, const char* mode) {
#ifdef _WIN32
    FILE* fp = NULL;
    int filename_len;
    int mode_len;
    wchar_t* wide_filename;
    wchar_t* wide_mode;

    filename_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                       filename, -1, NULL, 0);
    mode_len = MultiByteToWideChar(CP_UTF8, 0, mode, -1, NULL, 0);
    if (filename_len <= 0 || mode_len <= 0) {
        return NULL;
    }
    wide_filename = (wchar_t*)malloc((size_t)filename_len * sizeof(wchar_t));
    wide_mode = (wchar_t*)malloc((size_t)mode_len * sizeof(wchar_t));
    if (!wide_filename || !wide_mode) {
        free(wide_filename);
        free(wide_mode);
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, filename, -1,
                            wide_filename, filename_len) > 0 &&
        MultiByteToWideChar(CP_UTF8, 0, mode, -1, wide_mode, mode_len) > 0) {
        fp = _wfopen(wide_filename, wide_mode);
    }
    free(wide_filename);
    free(wide_mode);
    return fp;
#else
    return fopen(filename, mode);
#endif
}

static char* make_staging_filename(const char* filename) {
    static unsigned serial;
    char suffix[64];
    size_t filename_length;
    size_t suffix_length;
    char* staging;
#ifdef _WIN32
    unsigned process = (unsigned)GetCurrentProcessId();
    unsigned sequence = (unsigned)InterlockedIncrement((volatile LONG*)&serial);
#else
    unsigned process = (unsigned)getpid();
    unsigned sequence = ++serial;
#endif

    if (!filename) return NULL;
    snprintf(suffix, sizeof(suffix), ".tmp-%u-%u", process, sequence);
    filename_length = strlen(filename);
    suffix_length = strlen(suffix);
    if (filename_length > SIZE_MAX - suffix_length - 1) return NULL;
    staging = (char*)malloc(filename_length + suffix_length + 1);
    if (!staging) return NULL;
    memcpy(staging, filename, filename_length);
    memcpy(staging + filename_length, suffix, suffix_length + 1);
    return staging;
}

static void remove_utf8_file(const char* filename) {
    if (!filename) return;
#ifdef _WIN32
    {
        int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         filename, -1, NULL, 0);
        wchar_t* wide = length > 0
            ? (wchar_t*)malloc((size_t)length * sizeof(*wide)) : NULL;
        if (wide && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        filename, -1, wide, length) > 0) {
            DeleteFileW(wide);
        }
        free(wide);
    }
#else
    remove(filename);
#endif
}

static int replace_utf8_file(const char* staging, const char* destination) {
#ifdef _WIN32
    int staging_length;
    int destination_length;
    wchar_t* wide_staging;
    wchar_t* wide_destination;
    int ok = 0;
    staging_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         staging, -1, NULL, 0);
    destination_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                             destination, -1, NULL, 0);
    if (staging_length <= 0 || destination_length <= 0) return 0;
    wide_staging = (wchar_t*)malloc((size_t)staging_length * sizeof(*wide_staging));
    wide_destination = (wchar_t*)malloc((size_t)destination_length * sizeof(*wide_destination));
    if (wide_staging && wide_destination &&
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, staging, -1,
                            wide_staging, staging_length) > 0 &&
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, destination, -1,
                            wide_destination, destination_length) > 0) {
        ok = MoveFileExW(wide_staging, wide_destination,
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    }
    free(wide_staging);
    free(wide_destination);
    return ok;
#else
    return rename(staging, destination) == 0;
#endif
}

static int finish_staged_file(FILE* file) {
    int ok = 1;
    if (!file) return 0;
    if (fflush(file) != 0) ok = 0;
#ifdef _WIN32
    if (ok && _commit(_fileno(file)) != 0) ok = 0;
#else
    if (ok && fsync(fileno(file)) != 0) ok = 0;
#endif
    if (fclose(file) != 0) ok = 0;
    return ok;
}

static int round_coordinate(double value, int* rounded_out) {
    double rounded;
    if (!isfinite(value)) {
        return 0;
    }
    rounded = value >= 0.0 ? floor(value + 0.5) : ceil(value - 0.5);
    if (rounded < (double)INT_MIN || rounded > (double)INT_MAX) {
        return 0;
    }
    *rounded_out = (int)rounded;
    return 1;
}

static int find_or_add_vertex(ThreeDg1ExportVertex* vertices, int* vertex_count,
                              const CadPoint* point) {
    ThreeDg1ExportVertex rounded;
    int i;

    if (!round_coordinate(point->pointx, &rounded.x) ||
        !round_coordinate(point->pointy, &rounded.y) ||
        !round_coordinate(point->pointz, &rounded.z)) {
        return -2;
    }
    for (i = 0; i < *vertex_count; i++) {
        if (vertices[i].x == rounded.x &&
            vertices[i].y == rounded.y &&
            vertices[i].z == rounded.z) {
            return i;
        }
    }
    if (*vertex_count == CAD_MAX_POINTS) {
        return -1;
    }
    vertices[*vertex_count] = rounded;
    (*vertex_count)++;
    return *vertex_count - 1;
}

static int collect_3dg1_geometry(const CadCore* core,
                                 ThreeDg1ExportVertex* vertices,
                                 int* vertex_count,
                                 ThreeDg1ExportFace* faces,
                                 int* face_count) {
    int index;

    if (core->data.pointCount < 0 || core->data.pointCount > CAD_MAX_POINTS ||
        core->data.polygonCount < 0 || core->data.polygonCount > CAD_MAX_POLYGONS) {
        fprintf(stderr, "3DG1 export error: CAD high-water counts are out of range\n");
        return 0;
    }

    *vertex_count = 0;
    *face_count = 0;

    /* Coordinate deduplication is intentional for the recovered Fundoshi
       workflow. It collapses native linked-chain copies after grid rounding. */
    for (index = 0; index < core->data.pointCount; index++) {
        const CadPoint* point = &core->data.points[index];
        int result;
        if (point->flags == 0) {
            continue;
        }
        result = find_or_add_vertex(vertices, vertex_count, point);
        if (result == -2) {
            fprintf(stderr,
                    "3DG1 export error: point %d cannot be represented as a 32-bit grid coordinate\n",
                    index);
            return 0;
        }
        if (result < 0) {
            fprintf(stderr,
                    "3DG1 export error: more than %d unique grid points\n",
                    CAD_MAX_POINTS);
            return 0;
        }
    }

    for (index = 0; index < core->data.polygonCount; index++) {
        const CadPolygon* polygon = &core->data.polygons[index];
        ThreeDg1ExportFace* face;
        int16_t point_index;
        int16_t visited[CAD_MAX_FACE_POINTS];
        int i;

        if (polygon->flags == 0) {
            continue;
        }
        if (polygon->npoints < CAD_MIN_FACE_POINTS ||
            polygon->npoints > CAD_MAX_FACE_POINTS) {
            fprintf(stderr,
                    "3DG1 export error: polygon %d has %u points; supported range is %d..%d\n",
                    index, (unsigned)polygon->npoints,
                    CAD_MIN_FACE_POINTS, CAD_MAX_FACE_POINTS);
            return 0;
        }
        if (*face_count == CAD_MAX_POLYGONS) {
            fprintf(stderr, "3DG1 export error: polygon capacity exceeded\n");
            return 0;
        }

        face = &faces[*face_count];
        face->count = polygon->npoints;
        face->color = polygon->color;
        point_index = polygon->firstPoint;
        for (i = 0; i < face->count; i++) {
            const CadPoint* point;
            int vertex_index;
            int j;

            if (point_index < 0 || point_index >= core->data.pointCount ||
                point_index >= CAD_MAX_POINTS) {
                fprintf(stderr,
                        "3DG1 export error: polygon %d has a broken point chain at element %d\n",
                        index, i);
                return 0;
            }
            point = &core->data.points[point_index];
            if (point->flags == 0) {
                fprintf(stderr,
                        "3DG1 export error: polygon %d references deleted point %d\n",
                        index, point_index);
                return 0;
            }
            for (j = 0; j < i; j++) {
                if (visited[j] == point_index) {
                    fprintf(stderr,
                            "3DG1 export error: polygon %d contains a cyclic point chain\n",
                            index);
                    return 0;
                }
            }
            visited[i] = point_index;
            vertex_index = find_or_add_vertex(vertices, vertex_count, point);
            if (vertex_index < 0) {
                fprintf(stderr,
                        "3DG1 export error: point %d has an invalid coordinate\n",
                        point_index);
                return 0;
            }
            face->vertex[i] = vertex_index;
            point_index = point->nextPoint;
        }
        if (point_index != INVALID_INDEX) {
            fprintf(stderr,
                    "3DG1 export error: polygon %d point chain is longer than its declared size\n",
                    index);
            return 0;
        }
        (*face_count)++;
    }
    return 1;
}

int CadExport_3DG1(const CadCore* core, const char* filename) {
    ThreeDg1ExportVertex vertices[CAD_MAX_POINTS];
    ThreeDg1ExportFace faces[CAD_MAX_POLYGONS];
    int vertex_count;
    int face_count;
    unsigned char used_colors[256] = {0};
    int color_count = 0;
    char* staging = NULL;
    FILE* file = NULL;
    int i;
    int ok = 1;
    char diagnostic[256];

    if (!core || !filename || filename[0] == '\0') {
        fprintf(stderr, "3DG1 export error: core and filename are required\n");
        return 0;
    }
    if (!CadCore_ValidateDocument(core, diagnostic, sizeof(diagnostic))) {
        fprintf(stderr, "3DG1 export error: invalid CAD topology: %s\n",
                diagnostic[0] ? diagnostic : "validation failed");
        return 0;
    }
    if (!collect_3dg1_geometry(core, vertices, &vertex_count,
                               faces, &face_count)) {
        return 0;
    }

    for (i = 0; i < face_count; i++) {
        if (!used_colors[faces[i].color]) {
            used_colors[faces[i].color] = 1;
            color_count++;
        }
    }

    staging = make_staging_filename(filename);
    if (!staging) {
        fprintf(stderr, "3DG1 export error: could not construct a staging filename\n");
        return 0;
    }
    file = open_utf8_file(staging, "wb");
    if (!file) {
        fprintf(stderr, "3DG1 export error: could not create a staging file for '%s'\n",
                filename);
        free(staging);
        return 0;
    }

    if (fprintf(file, "3DG1\n%d\n", vertex_count) < 0) {
        ok = 0;
    }
    for (i = 0; ok && i < vertex_count; i++) {
        if (fprintf(file, "%d %d %d\n",
                    vertices[i].x, vertices[i].y, vertices[i].z) < 0) {
            ok = 0;
        }
    }
    for (i = 0; ok && i < face_count; i++) {
        int j;
        if (fprintf(file, "%d", faces[i].count) < 0) {
            ok = 0;
            break;
        }
        for (j = 0; j < faces[i].count; j++) {
            if (fprintf(file, " %d", faces[i].vertex[j]) < 0) {
                ok = 0;
                break;
            }
        }
        if (ok && fprintf(file, " %u\n", (unsigned)faces[i].color) < 0) {
            ok = 0;
        }
    }
    if (ok && fputc(0x1a, file) == EOF) {
        ok = 0;
    }
    if (ferror(file)) ok = 0;
    if (!finish_staged_file(file)) ok = 0;
    file = NULL;

    if (!ok) {
        fprintf(stderr, "3DG1 export error: failed while writing '%s'\n", filename);
        remove_utf8_file(staging);
        free(staging);
        return 0;
    }
    if (!replace_utf8_file(staging, filename)) {
        fprintf(stderr,
                "3DG1 export error: could not atomically replace '%s'; the prior file remains\n",
                filename);
        remove_utf8_file(staging);
        free(staging);
        return 0;
    }
    fprintf(stdout,
            "Exported 3DG1: %s (%d grid vertices, %d faces, %d colors)\n",
            filename, vertex_count, face_count, color_count);
    free(staging);
    return 1;
}
