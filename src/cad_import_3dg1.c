#define _CRT_SECURE_NO_WARNINGS

#include "cad_import_3dg1.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#ifndef CP_UTF8
#define CP_UTF8 65001
#endif
#endif

#define THREEDG1_LINE_CAPACITY 8192

typedef struct {
    double x;
    double y;
    double z;
} ThreeDg1Vertex;

typedef struct {
    int vertex[CAD_MAX_FACE_POINTS];
    int count;
    uint8_t color;
} ThreeDg1Face;

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

/* DOS text files may contain 0x1A either on its own or directly after the
   final record. Text after the marker is deliberately ignored. */
static int read_3dg1_line(FILE* fp, char* line, size_t capacity,
                          int* line_number, int* dos_eof) {
    size_t length;
    char* marker;
    int ch;

    if (*dos_eof) {
        return 0;
    }
    if (!fgets(line, (int)capacity, fp)) {
        return ferror(fp) ? -1 : 0;
    }
    (*line_number)++;

    marker = (char*)memchr(line, 0x1a, strlen(line));
    if (marker) {
        *marker = '\0';
        *dos_eof = 1;
    }

    length = strlen(line);
    if (!*dos_eof && length > 0 && line[length - 1] != '\n' && !feof(fp)) {
        do {
            ch = fgetc(fp);
        } while (ch != '\n' && ch != EOF);
        return -1;
    }
    while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
        line[--length] = '\0';
    }
    return 1;
}

static char* skip_space(char* text) {
    while (*text != '\0' && isspace((unsigned char)*text)) {
        text++;
    }
    return text;
}

static int next_data_line(FILE* fp, char* line, size_t capacity,
                          int* line_number, int* dos_eof) {
    int result;
    while ((result = read_3dg1_line(fp, line, capacity, line_number, dos_eof)) > 0) {
        char* cursor = skip_space(line);
        if (*cursor != '\0' && *cursor != '#') {
            if (cursor != line) {
                memmove(line, cursor, strlen(cursor) + 1);
            }
            return 1;
        }
    }
    return result;
}

static int parse_integer(char** cursor_in_out, long* value_out) {
    char* cursor = skip_space(*cursor_in_out);
    char* end;
    long value;

    if (*cursor == '\0' || *cursor == '#') {
        return 0;
    }
    errno = 0;
    value = strtol(cursor, &end, 10);
    if (end == cursor || errno == ERANGE ||
        (*end != '\0' && *end != '#' && !isspace((unsigned char)*end))) {
        return 0;
    }
    *cursor_in_out = end;
    *value_out = value;
    return 1;
}

static int no_more_tokens(char* cursor) {
    cursor = skip_space(cursor);
    return *cursor == '\0' || *cursor == '#';
}

static int parse_vertex_line(char* line, ThreeDg1Vertex* vertex) {
    char* cursor = line;
    char* end;
    double* values[3] = {&vertex->x, &vertex->y, &vertex->z};
    int i;

    for (i = 0; i < 3; i++) {
        cursor = skip_space(cursor);
        errno = 0;
        *values[i] = strtod(cursor, &end);
        if (end == cursor || errno == ERANGE || !isfinite(*values[i]) ||
            (*end != '\0' && *end != '#' && !isspace((unsigned char)*end))) {
            return 0;
        }
        cursor = end;
    }
    return no_more_tokens(cursor);
}

static int parse_face_line(char* line, int vertex_count,
                           ThreeDg1Face* face, int line_number) {
    char* cursor = line;
    long value;
    int i;

    if (!parse_integer(&cursor, &value) || value < CAD_MIN_FACE_POINTS ||
        value > CAD_MAX_FACE_POINTS) {
        fprintf(stderr,
                "3DG1 import error at line %d: face size must be in the range %d..%d\n",
                line_number, CAD_MIN_FACE_POINTS, CAD_MAX_FACE_POINTS);
        return 0;
    }
    face->count = (int)value;

    for (i = 0; i < face->count; i++) {
        if (!parse_integer(&cursor, &value) || value < 0 || value >= vertex_count) {
            fprintf(stderr,
                    "3DG1 import error at line %d: vertex reference %d is missing or out of range\n",
                    line_number, i + 1);
            return 0;
        }
        face->vertex[i] = (int)value;
    }
    if (!parse_integer(&cursor, &value) || value < 0 || value > 255) {
        fprintf(stderr,
                "3DG1 import error at line %d: color must be in the range 0..255\n",
                line_number);
        return 0;
    }
    face->color = (uint8_t)value;
    if (!no_more_tokens(cursor)) {
        fprintf(stderr,
                "3DG1 import error at line %d: unexpected data after the face color\n",
                line_number);
        return 0;
    }
    return 1;
}

static int build_3dg1_core(CadCore* parsed,
                           const ThreeDg1Vertex* vertices, int vertex_count,
                           const ThreeDg1Face* faces, int face_count) {
    int canonical[CAD_MAX_POINTS];
    unsigned char represented[CAD_MAX_POINTS] = {0};
    int i;

    for (i = 0; i < vertex_count; i++) {
        int j;
        canonical[i] = i;
        for (j = 0; j < i; j++) {
            if (vertices[j].x == vertices[i].x &&
                vertices[j].y == vertices[i].y &&
                vertices[j].z == vertices[i].z) {
                canonical[i] = canonical[j];
                break;
            }
        }
    }

    for (i = 0; i < face_count; i++) {
        int16_t first_point = INVALID_INDEX;
        int16_t polygon;
        int j;

        for (j = faces[i].count - 1; j >= 0; j--) {
            int canonical_vertex = canonical[faces[i].vertex[j]];
            int16_t point = CadCore_AddPoint(parsed,
                                             vertices[canonical_vertex].x,
                                             vertices[canonical_vertex].y,
                                             vertices[canonical_vertex].z);
            if (point == INVALID_INDEX) {
                fprintf(stderr,
                        "3DG1 import error: CAD point capacity (%d) exceeded while building face %d\n",
                        CAD_MAX_POINTS, i + 1);
                return 0;
            }
            parsed->data.points[point].nextPoint = first_point;
            first_point = point;
            represented[canonical_vertex] = 1;
        }

        polygon = CadCore_AddPolygon(parsed, first_point, faces[i].color,
                                     (uint8_t)faces[i].count);
        if (polygon == INVALID_INDEX) {
            fprintf(stderr,
                    "3DG1 import error: CAD polygon capacity (%d) exceeded\n",
                    CAD_MAX_POLYGONS);
            return 0;
        }
        parsed->data.polygons[polygon].animation = INVALID_INDEX;
    }

    /* Keep source vertices that were not represented by a face chain. Exact
       duplicate coordinates intentionally share a single CAD point. */
    for (i = 0; i < vertex_count; i++) {
        if (canonical[i] != i) {
            continue;
        }
        if (!represented[i] &&
            CadCore_AddPoint(parsed, vertices[i].x, vertices[i].y,
                             vertices[i].z) == INVALID_INDEX) {
            fprintf(stderr,
                    "3DG1 import error: CAD point capacity (%d) exceeded by standalone vertices\n",
                    CAD_MAX_POINTS);
            return 0;
        }
    }

    parsed->isDirty = 1;
    return 1;
}

int CadImport_3DG1(CadCore* core, const char* filename) {
    FILE* fp;
    ThreeDg1Vertex vertices[CAD_MAX_POINTS];
    ThreeDg1Face faces[CAD_MAX_POLYGONS];
    int vertex_count;
    int face_count = 0;
    int line_number = 0;
    int dos_eof = 0;
    int result;
    char line[THREEDG1_LINE_CAPACITY];
    char* cursor;
    long value;
    int i;
    CadCore* parsed;

    if (!core || !filename || filename[0] == '\0') {
        fprintf(stderr, "3DG1 import error: core and filename are required\n");
        return 0;
    }
    fp = open_utf8_file(filename, "rb");
    if (!fp) {
        fprintf(stderr, "3DG1 import error: could not open '%s' for reading\n", filename);
        return 0;
    }

    result = next_data_line(fp, line, sizeof(line), &line_number, &dos_eof);
    cursor = line;
    if (result <= 0) {
        fprintf(stderr, "3DG1 import error: '%s' has no header\n", filename);
        fclose(fp);
        return 0;
    }
    if (strlen(cursor) >= 3 &&
        (unsigned char)cursor[0] == 0xef &&
        (unsigned char)cursor[1] == 0xbb &&
        (unsigned char)cursor[2] == 0xbf) {
        cursor += 3;
    }
    if (strncmp(cursor, "3DG1", 4) != 0 || !no_more_tokens(cursor + 4)) {
        fprintf(stderr,
                "3DG1 import error at line %d: expected the '3DG1' header\n",
                line_number);
        fclose(fp);
        return 0;
    }

    result = next_data_line(fp, line, sizeof(line), &line_number, &dos_eof);
    cursor = line;
    if (result <= 0 || !parse_integer(&cursor, &value) || !no_more_tokens(cursor) ||
        value < 0 || value > CAD_MAX_POINTS) {
        fprintf(stderr,
                "3DG1 import error at line %d: vertex count must be in the range 0..%d\n",
                line_number, CAD_MAX_POINTS);
        fclose(fp);
        return 0;
    }
    vertex_count = (int)value;

    for (i = 0; i < vertex_count; i++) {
        result = next_data_line(fp, line, sizeof(line), &line_number, &dos_eof);
        if (result <= 0 || !parse_vertex_line(line, &vertices[i])) {
            fprintf(stderr,
                    "3DG1 import error at line %d: malformed or missing vertex %d of %d\n",
                    line_number, i + 1, vertex_count);
            fclose(fp);
            return 0;
        }
    }

    while ((result = next_data_line(fp, line, sizeof(line),
                                    &line_number, &dos_eof)) > 0) {
        if (face_count == CAD_MAX_POLYGONS) {
            fprintf(stderr,
                    "3DG1 import error at line %d: source exceeds the %d-polygon CAD limit\n",
                    line_number, CAD_MAX_POLYGONS);
            fclose(fp);
            return 0;
        }
        if (!parse_face_line(line, vertex_count, &faces[face_count], line_number)) {
            fclose(fp);
            return 0;
        }
        face_count++;
    }
    if (result < 0) {
        fprintf(stderr,
                "3DG1 import error at line %d: line is longer than %d bytes or the file could not be read\n",
                line_number, THREEDG1_LINE_CAPACITY - 1);
        fclose(fp);
        return 0;
    }
    if (fclose(fp) != 0) {
        fprintf(stderr, "3DG1 import error: failed while closing '%s'\n", filename);
        return 0;
    }

    parsed = (CadCore*)malloc(sizeof(*parsed));
    if (!parsed) {
        fprintf(stderr, "3DG1 import error: not enough memory for a temporary document\n");
        return 0;
    }
    CadCore_Init(parsed);
    if (!build_3dg1_core(parsed, vertices, vertex_count, faces, face_count)) {
        CadCore_Destroy(parsed);
        free(parsed);
        return 0;
    }

    CadCore_Destroy(core);
    *core = *parsed;
    free(parsed);

    fprintf(stdout,
            "Imported 3DG1: %d source vertices, %d faces, %d CAD points\n",
            vertex_count, face_count, core->data.pointCount);
    return 1;
}
