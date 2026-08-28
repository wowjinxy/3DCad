#define _CRT_SECURE_NO_WARNINGS

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "cad_export_obj.h"

#include <errno.h>
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
#include <sys/stat.h>
#include <unistd.h>
#endif

typedef struct {
    double x;
    double y;
    double z;
} ObjExportVertex;

typedef struct {
    int vertex[CAD_MAX_FACE_POINTS];
    int count;
    uint8_t color;
} ObjExportFace;

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

static char* make_auxiliary_filename(const char* filename, const char* purpose) {
#ifdef _WIN32
    static volatile LONG serial;
#else
    static unsigned serial;
#endif
    char suffix[64];
    size_t filename_length;
    size_t suffix_length;
    char* staging;
#ifdef _WIN32
    unsigned process = (unsigned)GetCurrentProcessId();
    unsigned sequence = (unsigned)InterlockedIncrement(&serial);
#else
    unsigned process = (unsigned)getpid();
    unsigned sequence = ++serial;
#endif

    if (!filename || !purpose) return NULL;
    snprintf(suffix, sizeof(suffix), ".3dcad-%s-%u-%u",
             purpose, process, sequence);
    filename_length = strlen(filename);
    suffix_length = strlen(suffix);
    if (filename_length > SIZE_MAX - suffix_length - 1) return NULL;
    staging = (char*)malloc(filename_length + suffix_length + 1);
    if (!staging) return NULL;
    memcpy(staging, filename, filename_length);
    memcpy(staging + filename_length, suffix, suffix_length + 1);
    return staging;
}

static int query_utf8_path(const char* filename, int* exists,
                           int* is_directory, int* is_read_only) {
    if (!filename || !exists || !is_directory || !is_read_only) return 0;
    *exists = 0;
    *is_directory = 0;
    *is_read_only = 0;
#ifdef _WIN32
    {
        int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         filename, -1, NULL, 0);
        wchar_t* wide = length > 0
            ? (wchar_t*)malloc((size_t)length * sizeof(*wide)) : NULL;
        DWORD attributes;
        DWORD error;
        if (!wide || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         filename, -1, wide, length) <= 0) {
            free(wide);
            return 0;
        }
        attributes = GetFileAttributesW(wide);
        error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
        free(wide);
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            *exists = 1;
            *is_directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            *is_read_only = (attributes & FILE_ATTRIBUTE_READONLY) != 0;
            return 1;
        }
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
#else
    {
        struct stat status;
        if (lstat(filename, &status) == 0) {
            *exists = 1;
            *is_directory = S_ISDIR(status.st_mode);
            return 1;
        }
        return errno == ENOENT || errno == ENOTDIR;
    }
#endif
}

static int remove_utf8_file(const char* filename) {
    if (!filename) return 1;
#ifdef _WIN32
    {
        int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         filename, -1, NULL, 0);
        wchar_t* wide = length > 0
            ? (wchar_t*)malloc((size_t)length * sizeof(*wide)) : NULL;
        int removed = 0;
        if (wide && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        filename, -1, wide, length) > 0) {
            if (DeleteFileW(wide)) {
                removed = 1;
            } else {
                DWORD error = GetLastError();
                removed = error == ERROR_FILE_NOT_FOUND ||
                          error == ERROR_PATH_NOT_FOUND;
            }
        }
        free(wide);
        return removed;
    }
#else
    return unlink(filename) == 0 || errno == ENOENT;
#endif
}

static int move_utf8_file(const char* source, const char* destination,
                          int replace_existing) {
#ifdef _WIN32
    int source_length;
    int destination_length;
    wchar_t* wide_source;
    wchar_t* wide_destination;
    int ok = 0;
    DWORD flags = MOVEFILE_WRITE_THROUGH;
    source_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        source, -1, NULL, 0);
    destination_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                             destination, -1, NULL, 0);
    if (source_length <= 0 || destination_length <= 0) return 0;
    wide_source = (wchar_t*)malloc((size_t)source_length * sizeof(*wide_source));
    wide_destination = (wchar_t*)malloc((size_t)destination_length * sizeof(*wide_destination));
    if (replace_existing) flags |= MOVEFILE_REPLACE_EXISTING;
    if (wide_source && wide_destination &&
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source, -1,
                            wide_source, source_length) > 0 &&
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, destination, -1,
                            wide_destination, destination_length) > 0) {
        ok = MoveFileExW(wide_source, wide_destination, flags) != 0;
    }
    free(wide_source);
    free(wide_destination);
    return ok;
#else
    if (!replace_existing) {
        int exists;
        int is_directory;
        int is_read_only;
        if (!query_utf8_path(destination, &exists, &is_directory,
                             &is_read_only) || exists) {
            return 0;
        }
    }
    return rename(source, destination) == 0;
#endif
}

static int replace_utf8_file(const char* staging, const char* destination) {
    return move_utf8_file(staging, destination, 1);
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

typedef struct {
    const char* destination;
    const char* staging;
    char* backup;
    int had_original;
    int backup_active;
    int installed;
} ObjOutputTransaction;

static char* make_unique_auxiliary_filename(const char* filename,
                                            const char* purpose) {
    int attempt;
    for (attempt = 0; attempt < 64; attempt++) {
        char* candidate = make_auxiliary_filename(filename, purpose);
        int exists;
        int is_directory;
        int is_read_only;
        if (!candidate) return NULL;
        if (!query_utf8_path(candidate, &exists, &is_directory,
                             &is_read_only)) {
            free(candidate);
            return NULL;
        }
        if (!exists) return candidate;
        free(candidate);
    }
    return NULL;
}

static int output_paths_are_same(const char* first, const char* second) {
#ifdef _WIN32
    /* The derived MTL name can differ only by ASCII extension case from the
       requested output (for example, "model.MTL"). Windows resolves those
       names to the same destination, so reject the collision explicitly. */
    return _stricmp(first, second) == 0;
#else
    return strcmp(first, second) == 0;
#endif
}

static int prepare_output_transaction(ObjOutputTransaction* output) {
    int is_directory;
    int is_read_only;
    if (!query_utf8_path(output->destination, &output->had_original,
                         &is_directory, &is_read_only)) {
        return 0;
    }
    if (is_directory || is_read_only) {
        return 0;
    }
    if (output->had_original) {
        output->backup = make_unique_auxiliary_filename(output->destination,
                                                        "backup");
        if (!output->backup) return 0;
    }
    return 1;
}

static int backup_output(ObjOutputTransaction* output) {
    if (!output->had_original) return 1;
    if (!move_utf8_file(output->destination, output->backup, 0)) return 0;
    output->backup_active = 1;
    return 1;
}

static int restore_output(ObjOutputTransaction* output) {
    int ok = 1;
    if (output->installed) {
        if (!remove_utf8_file(output->destination)) ok = 0;
        output->installed = 0;
    }
    if (output->backup_active) {
        /* Replacement is intentional here: if deleting a just-installed file
           failed, restoring the user's original still takes precedence. */
        if (move_utf8_file(output->backup, output->destination, 1)) {
            output->backup_active = 0;
        } else {
            ok = 0;
        }
    }
    return ok;
}

static int rollback_output_pair(ObjOutputTransaction* obj,
                                ObjOutputTransaction* mtl) {
    int ok = 1;

    /* Remove both newly-installed files before restoring either original, so
       observers never see an old OBJ paired with the new MTL during rollback. */
    if (obj->installed && !remove_utf8_file(obj->destination)) ok = 0;
    obj->installed = 0;
    if (mtl->installed && !remove_utf8_file(mtl->destination)) ok = 0;
    mtl->installed = 0;

    if (!restore_output(obj)) ok = 0;
    if (!restore_output(mtl)) ok = 0;
    if (!remove_utf8_file(obj->staging)) ok = 0;
    if (!remove_utf8_file(mtl->staging)) ok = 0;
    return ok;
}

static int install_output_pair(ObjOutputTransaction* obj,
                               ObjOutputTransaction* mtl) {
    int rollback_ok;
    int cleanup_ok = 1;

    if (!prepare_output_transaction(obj) ||
        !prepare_output_transaction(mtl)) {
        fprintf(stderr,
                "OBJ export error: an output path is inaccessible or names a directory\n");
        remove_utf8_file(obj->staging);
        remove_utf8_file(mtl->staging);
        return 0;
    }

    /* Secure both prior files before installing either member of the pair.
       A failure at any later step can therefore restore the exact prior pair. */
    if (!backup_output(mtl) || !backup_output(obj)) {
        fprintf(stderr,
                "OBJ export error: could not secure the previous OBJ/MTL pair\n");
        rollback_ok = rollback_output_pair(obj, mtl);
        if (!rollback_ok) {
            fprintf(stderr,
                    "OBJ export recovery error: an original output could not be restored; preserve any .3dcad-backup file\n");
        }
        return 0;
    }

    /* Keep the dependency order: install the MTL first, then expose the OBJ
       which references it. Both originals remain recoverable until complete. */
    if (!replace_utf8_file(mtl->staging, mtl->destination)) {
        fprintf(stderr, "OBJ export error: could not install '%s'\n",
                mtl->destination);
        rollback_ok = rollback_output_pair(obj, mtl);
        if (!rollback_ok) {
            fprintf(stderr,
                    "OBJ export recovery error: an original output could not be restored; preserve any .3dcad-backup file\n");
        }
        return 0;
    }
    mtl->installed = 1;
    if (!replace_utf8_file(obj->staging, obj->destination)) {
        fprintf(stderr, "OBJ export error: could not install '%s'\n",
                obj->destination);
        rollback_ok = rollback_output_pair(obj, mtl);
        if (!rollback_ok) {
            fprintf(stderr,
                    "OBJ export recovery error: an original output could not be restored; preserve any .3dcad-backup file\n");
        }
        return 0;
    }
    obj->installed = 1;

    /* A successful move consumes each staging path. Removing the two backups
       is the final commit step and leaves no exporter-owned artifacts. */
    if (obj->backup_active) {
        if (remove_utf8_file(obj->backup)) {
            obj->backup_active = 0;
        } else {
            cleanup_ok = 0;
        }
    }
    if (mtl->backup_active) {
        if (remove_utf8_file(mtl->backup)) {
            mtl->backup_active = 0;
        } else {
            cleanup_ok = 0;
        }
    }
    if (!cleanup_ok) {
        fprintf(stderr,
                "OBJ export error: installed the new pair but could not remove an old backup\n");
        return 0;
    }
    return 1;
}

static char* make_mtl_filename(const char* filename) {
    const char* slash_forward = strrchr(filename, '/');
    const char* slash_backward = strrchr(filename, '\\');
    const char* basename = filename;
    const char* dot;
    size_t prefix_length;
    size_t result_length;
    char* result;

    if (slash_forward && slash_forward + 1 > basename) {
        basename = slash_forward + 1;
    }
    if (slash_backward && slash_backward + 1 > basename) {
        basename = slash_backward + 1;
    }
    dot = strrchr(basename, '.');
    prefix_length = dot ? (size_t)(dot - filename) : strlen(filename);
    if (prefix_length > SIZE_MAX - 5) {
        return NULL;
    }
    result_length = prefix_length + 4;
    result = (char*)malloc(result_length + 1);
    if (!result) {
        return NULL;
    }
    memcpy(result, filename, prefix_length);
    memcpy(result + prefix_length, ".mtl", 5);
    return result;
}

static const char* path_basename(const char* path) {
    const char* forward = strrchr(path, '/');
    const char* backward = strrchr(path, '\\');
    const char* result = path;

    if (forward) {
        result = forward + 1;
    }
    if (backward && backward + 1 > result) {
        result = backward + 1;
    }
    return result;
}

static int find_or_add_vertex(ObjExportVertex* vertices, int* vertex_count,
                              const CadPoint* point) {
    int i;

    if (!isfinite(point->pointx) || !isfinite(point->pointy) ||
        !isfinite(point->pointz)) {
        return -2;
    }
    for (i = 0; i < *vertex_count; i++) {
        if (vertices[i].x == point->pointx &&
            vertices[i].y == point->pointy &&
            vertices[i].z == point->pointz) {
            return i;
        }
    }
    if (*vertex_count == CAD_MAX_POINTS) {
        return -1;
    }
    vertices[*vertex_count].x = point->pointx;
    vertices[*vertex_count].y = point->pointy;
    vertices[*vertex_count].z = point->pointz;
    (*vertex_count)++;
    return *vertex_count - 1;
}

static int collect_obj_geometry(const CadCore* core,
                                ObjExportVertex* vertices, int* vertex_count,
                                ObjExportFace* faces, int* face_count) {
    int polygon_index;

    if (core->data.pointCount < 0 || core->data.pointCount > CAD_MAX_POINTS ||
        core->data.polygonCount < 0 || core->data.polygonCount > CAD_MAX_POLYGONS) {
        fprintf(stderr, "OBJ export error: CAD high-water counts are out of range\n");
        return 0;
    }

    *vertex_count = 0;
    *face_count = 0;
    /* Export real standalone editing points too. Coordinate deduplication
       collapses the linked-chain copies required by the native CAD model. */
    for (polygon_index = 0; polygon_index < core->data.pointCount; polygon_index++) {
        const CadPoint* point = &core->data.points[polygon_index];
        int vertex_index;
        if (point->flags == 0) {
            continue;
        }
        vertex_index = find_or_add_vertex(vertices, vertex_count, point);
        if (vertex_index == -2) {
            fprintf(stderr,
                    "OBJ export error: point %d has non-finite coordinates\n",
                    polygon_index);
            return 0;
        }
        if (vertex_index < 0) {
            fprintf(stderr,
                    "OBJ export error: more than %d unique active points\n",
                    CAD_MAX_POINTS);
            return 0;
        }
    }
    for (polygon_index = 0; polygon_index < core->data.polygonCount; polygon_index++) {
        const CadPolygon* polygon = &core->data.polygons[polygon_index];
        ObjExportFace* face;
        int16_t point_index;
        int16_t visited[CAD_MAX_FACE_POINTS];
        int i;

        if (polygon->flags == 0) {
            continue;
        }
        if (polygon->npoints < CAD_MIN_FACE_POINTS ||
            polygon->npoints > CAD_MAX_FACE_POINTS) {
            fprintf(stderr,
                    "OBJ export error: polygon %d has %u points; supported range is %d..%d\n",
                    polygon_index, (unsigned)polygon->npoints,
                    CAD_MIN_FACE_POINTS, CAD_MAX_FACE_POINTS);
            return 0;
        }
        if (*face_count == CAD_MAX_POLYGONS) {
            fprintf(stderr, "OBJ export error: polygon capacity exceeded\n");
            return 0;
        }

        face = &faces[*face_count];
        face->count = polygon->npoints;
        face->color = polygon->color;
        point_index = polygon->firstPoint;

        for (i = 0; i < face->count; i++) {
            const CadPoint* point;
            int j;
            int vertex_index;

            if (point_index < 0 || point_index >= core->data.pointCount ||
                point_index >= CAD_MAX_POINTS) {
                fprintf(stderr,
                        "OBJ export error: polygon %d has a broken point chain at element %d\n",
                        polygon_index, i);
                return 0;
            }
            point = &core->data.points[point_index];
            if (point->flags == 0) {
                fprintf(stderr,
                        "OBJ export error: polygon %d references deleted point %d\n",
                        polygon_index, point_index);
                return 0;
            }
            for (j = 0; j < i; j++) {
                if (visited[j] == point_index) {
                    fprintf(stderr,
                            "OBJ export error: polygon %d contains a cyclic point chain\n",
                            polygon_index);
                    return 0;
                }
            }
            visited[i] = point_index;

            vertex_index = find_or_add_vertex(vertices, vertex_count, point);
            if (vertex_index == -2) {
                fprintf(stderr,
                        "OBJ export error: point %d has non-finite coordinates\n",
                        point_index);
                return 0;
            }
            if (vertex_index < 0) {
                fprintf(stderr,
                        "OBJ export error: more than %d unique referenced points\n",
                        CAD_MAX_POINTS);
                return 0;
            }
            face->vertex[i] = vertex_index;
            point_index = point->nextPoint;
        }
        if (point_index != INVALID_INDEX) {
            fprintf(stderr,
                    "OBJ export error: polygon %d point chain is longer than its declared size\n",
                    polygon_index);
            return 0;
        }
        (*face_count)++;
    }
    return 1;
}

static void color_index_to_rgb(uint8_t color, float* red, float* green, float* blue) {
    if (color < 16) {
        float gray = (float)color / 15.0f;
        *red = gray;
        *green = gray;
        *blue = gray;
    } else {
        int hue = (color - 16) % 6;
        const float low = 0.24f;
        const float high = 0.8f;

        *red = low;
        *green = low;
        *blue = low;
        if (hue == 0 || hue == 3 || hue == 4) *red = high;
        if (hue == 1 || hue == 3 || hue == 5) *green = high;
        if (hue == 2 || hue == 4 || hue == 5) *blue = high;
    }
}

int CadExport_OBJ(const CadCore* core, const char* filename) {
    ObjExportVertex vertices[CAD_MAX_POINTS];
    ObjExportFace faces[CAD_MAX_POLYGONS];
    int vertex_count;
    int face_count;
    unsigned char used_colors[256] = {0};
    int color_count = 0;
    char* mtl_filename;
    char* obj_staging = NULL;
    char* mtl_staging = NULL;
    const char* mtl_basename;
    FILE* obj_file = NULL;
    FILE* mtl_file = NULL;
    ObjOutputTransaction obj_output = {0};
    ObjOutputTransaction mtl_output = {0};
    int i;
    int ok = 1;
    char diagnostic[256];

    if (!core || !filename || filename[0] == '\0') {
        fprintf(stderr, "OBJ export error: core and filename are required\n");
        return 0;
    }
    if (!CadCore_ValidateDocument(core, diagnostic, sizeof(diagnostic))) {
        fprintf(stderr, "OBJ export error: invalid CAD topology: %s\n",
                diagnostic[0] ? diagnostic : "validation failed");
        return 0;
    }
    if (!collect_obj_geometry(core, vertices, &vertex_count, faces, &face_count)) {
        return 0;
    }

    mtl_filename = make_mtl_filename(filename);
    if (!mtl_filename) {
        fprintf(stderr, "OBJ export error: could not construct the MTL filename\n");
        return 0;
    }
    if (output_paths_are_same(filename, mtl_filename)) {
        fprintf(stderr,
                "OBJ export error: OBJ and MTL output paths must be different\n");
        free(mtl_filename);
        return 0;
    }
    mtl_basename = path_basename(mtl_filename);
    obj_staging = make_unique_auxiliary_filename(filename, "staging");
    mtl_staging = make_unique_auxiliary_filename(mtl_filename, "staging");
    if (!obj_staging || !mtl_staging) {
        fprintf(stderr, "OBJ export error: could not construct staging filenames\n");
        free(obj_staging);
        free(mtl_staging);
        free(mtl_filename);
        return 0;
    }

    for (i = 0; i < face_count; i++) {
        if (!used_colors[faces[i].color]) {
            used_colors[faces[i].color] = 1;
            color_count++;
        }
    }

    obj_file = open_utf8_file(obj_staging, "wb");
    if (!obj_file) {
        fprintf(stderr, "OBJ export error: could not create a staging file for '%s'\n",
                filename);
        free(obj_staging);
        free(mtl_staging);
        free(mtl_filename);
        return 0;
    }
    mtl_file = open_utf8_file(mtl_staging, "wb");
    if (!mtl_file) {
        fprintf(stderr, "OBJ export error: could not create a staging file for '%s'\n",
                mtl_filename);
        fclose(obj_file);
        remove_utf8_file(obj_staging);
        free(obj_staging);
        free(mtl_staging);
        free(mtl_filename);
        return 0;
    }

    if (fprintf(obj_file,
                "# OBJ file exported from 3DCad\n"
                "# Unique active vertices: %d, faces/lines: %d\n"
                "mtllib %s\n\n",
                vertex_count, face_count, mtl_basename) < 0) {
        ok = 0;
    }
    for (i = 0; ok && i < vertex_count; i++) {
        if (fprintf(obj_file, "v %.17g %.17g %.17g\n",
                    vertices[i].x, vertices[i].y, vertices[i].z) < 0) {
            ok = 0;
        }
    }

    for (i = 0; ok && i < 256; i++) {
        if (used_colors[i]) {
            float red;
            float green;
            float blue;
            color_index_to_rgb((uint8_t)i, &red, &green, &blue);
            if (fprintf(mtl_file,
                        "newmtl material_%d\n"
                        "Ka %.3f %.3f %.3f\n"
                        "Kd %.3f %.3f %.3f\n"
                        "Ks 0.500 0.500 0.500\n"
                        "Ns 32.0\n"
                        "d 1.0\n\n",
                        i, red * 0.2f, green * 0.2f, blue * 0.2f,
                        red, green, blue) < 0) {
                ok = 0;
            }
        }
    }

    for (i = 0; ok && i < face_count; i++) {
        int j;
        const char* record = faces[i].count == 2 ? "l" : "f";
        if (fprintf(obj_file, "usemtl material_%u\n%s",
                    (unsigned)faces[i].color, record) < 0) {
            ok = 0;
            break;
        }
        for (j = 0; j < faces[i].count; j++) {
            if (fprintf(obj_file, " %d", faces[i].vertex[j] + 1) < 0) {
                ok = 0;
                break;
            }
        }
        if (ok && fputc('\n', obj_file) == EOF) {
            ok = 0;
        }
    }

    if (ferror(obj_file) || ferror(mtl_file)) ok = 0;
    if (!finish_staged_file(obj_file)) ok = 0;
    obj_file = NULL;
    if (!finish_staged_file(mtl_file)) ok = 0;
    mtl_file = NULL;

    if (!ok) {
        fprintf(stderr, "OBJ export error: failed while writing '%s' or '%s'\n",
                 filename, mtl_filename);
        remove_utf8_file(obj_staging);
        remove_utf8_file(mtl_staging);
        free(obj_staging);
        free(mtl_staging);
        free(mtl_filename);
        return 0;
    }

    obj_output.destination = filename;
    obj_output.staging = obj_staging;
    mtl_output.destination = mtl_filename;
    mtl_output.staging = mtl_staging;
    if (!install_output_pair(&obj_output, &mtl_output)) {
        free(obj_output.backup);
        free(mtl_output.backup);
        free(obj_staging);
        free(mtl_staging);
        free(mtl_filename);
        return 0;
    }

    fprintf(stdout,
            "Exported OBJ: %s (%d unique vertices, %d faces/lines, %d materials)\n",
            filename, vertex_count, face_count, color_count);
    fprintf(stdout, "Exported MTL: %s\n", mtl_filename);
    free(obj_output.backup);
    free(mtl_output.backup);
    free(obj_staging);
    free(mtl_staging);
    free(mtl_filename);
    return 1;
}
