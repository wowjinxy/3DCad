#define _CRT_SECURE_NO_WARNINGS

#include "cad_import_obj.h"

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

#define OBJ_LINE_CAPACITY 8192

typedef struct {
    double x;
    double y;
    double z;
} ObjVertex;

typedef struct {
    int vertex[CAD_MAX_FACE_POINTS];
    int count;
    uint8_t color;
} ObjFace;

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

/* Returns 1 for a line, 0 for EOF, and -1 for an overlong/read-error line. */
static int read_obj_line(FILE* fp, char* line, size_t capacity, int* line_number) {
    size_t length;
    int ch;

    if (!fgets(line, (int)capacity, fp)) {
        return ferror(fp) ? -1 : 0;
    }
    (*line_number)++;

    length = strlen(line);
    if (length > 0 && line[length - 1] != '\n' && !feof(fp)) {
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

static int parse_obj_vertex(char* text, ObjVertex* vertex) {
    double values[4];
    int value_count = 0;
    char* cursor = text;

    while (1) {
        char* end;
        cursor = skip_space(cursor);
        if (*cursor == '\0' || *cursor == '#') {
            break;
        }
        if (value_count >= 4) {
            /* Vertex-color extensions are harmless, but every extra token must
               still be a finite number rather than malformed input. */
            double ignored;
            errno = 0;
            ignored = strtod(cursor, &end);
            if (end == cursor || errno == ERANGE || !isfinite(ignored) ||
                (*end != '\0' && *end != '#' && !isspace((unsigned char)*end))) {
                return 0;
            }
            value_count++;
            cursor = end;
            continue;
        }

        errno = 0;
        values[value_count] = strtod(cursor, &end);
        if (end == cursor || errno == ERANGE || !isfinite(values[value_count]) ||
            (*end != '\0' && *end != '#' && !isspace((unsigned char)*end))) {
            return 0;
        }
        value_count++;
        cursor = end;
    }

    if (value_count < 3) {
        return 0;
    }
    if (value_count == 4 && values[3] == 0.0) {
        return 0;
    }

    vertex->x = values[0];
    vertex->y = values[1];
    vertex->z = values[2];
    if (value_count == 4 && values[3] != 1.0) {
        vertex->x /= values[3];
        vertex->y /= values[3];
        vertex->z /= values[3];
    }
    return isfinite(vertex->x) && isfinite(vertex->y) && isfinite(vertex->z);
}

static int parse_optional_obj_index(const char* start, const char* end) {
    char* parsed_end;
    long value;

    if (start == end) {
        return 1;
    }
    errno = 0;
    value = strtol(start, &parsed_end, 10);
    return parsed_end == end && errno != ERANGE && value != 0 &&
           value >= INT_MIN && value <= INT_MAX;
}

static int parse_obj_reference(const char* start, size_t length,
                               int defined_vertices, int* index_out) {
    const char* token_end = start + length;
    const char* first_slash = NULL;
    const char* second_slash = NULL;
    const char* cursor;
    char* parsed_end;
    long long raw_index;
    long long resolved;

    for (cursor = start; cursor < token_end; cursor++) {
        if (*cursor == '/') {
            if (!first_slash) {
                first_slash = cursor;
            } else if (!second_slash) {
                second_slash = cursor;
            } else {
                return 0;
            }
        }
    }
    if (!first_slash) {
        first_slash = token_end;
    }

    errno = 0;
    raw_index = strtoll(start, &parsed_end, 10);
    if (parsed_end != first_slash || errno == ERANGE || raw_index == 0) {
        return 0;
    }

    if (first_slash < token_end) {
        const char* texture_end = second_slash ? second_slash : token_end;
        if ((!second_slash && first_slash + 1 == token_end) ||
            (second_slash && second_slash + 1 == token_end)) {
            return 0;
        }
        if (!parse_optional_obj_index(first_slash + 1, texture_end)) {
            return 0;
        }
        if (second_slash && !parse_optional_obj_index(second_slash + 1, token_end)) {
            return 0;
        }
    }

    /* Negative references are relative to the vertices defined at this line,
       not to the final number of vertices in the file. */
    resolved = raw_index > 0 ? raw_index - 1 : (long long)defined_vertices + raw_index;
    if (resolved < 0 || resolved >= defined_vertices || resolved > INT_MAX) {
        return 0;
    }

    *index_out = (int)resolved;
    return 1;
}

static int parse_obj_face(char* text, int vertex_count, uint8_t color,
                          ObjFace* face, int line_number) {
    char* cursor = text;
    int count = 0;

    while (1) {
        char* token_start;
        char* token_end;
        cursor = skip_space(cursor);
        if (*cursor == '\0' || *cursor == '#') {
            break;
        }

        if (count == CAD_MAX_FACE_POINTS) {
            fprintf(stderr,
                    "OBJ import error at line %d: polygon exceeds the %d-point CAD limit\n",
                    line_number, CAD_MAX_FACE_POINTS);
            return 0;
        }

        token_start = cursor;
        while (*cursor != '\0' && *cursor != '#' && !isspace((unsigned char)*cursor)) {
            cursor++;
        }
        token_end = cursor;

        if (!parse_obj_reference(token_start, (size_t)(token_end - token_start),
                                 vertex_count, &face->vertex[count])) {
            fprintf(stderr,
                    "OBJ import error at line %d: invalid or out-of-range vertex reference '%.*s'\n",
                    line_number, (int)(token_end - token_start), token_start);
            return 0;
        }
        count++;
    }

    if (count < CAD_MIN_FACE_POINTS) {
        fprintf(stderr,
                "OBJ import error at line %d: a face or line needs at least %d points\n",
                line_number, CAD_MIN_FACE_POINTS);
        return 0;
    }

    face->count = count;
    face->color = color;
    return 1;
}

static uint8_t parse_material_color(char* text, uint8_t fallback) {
    char* cursor = skip_space(text);
    const char prefix[] = "material_";
    char* end;
    long value;

    if (strncmp(cursor, prefix, sizeof(prefix) - 1) != 0) {
        return fallback;
    }
    cursor += sizeof(prefix) - 1;
    errno = 0;
    value = strtol(cursor, &end, 10);
    if (end == cursor || errno == ERANGE || value < 0 || value > 255) {
        return fallback;
    }
    end = skip_space(end);
    return (*end == '\0' || *end == '#') ? (uint8_t)value : fallback;
}

static int build_obj_core(CadCore* parsed, const ObjVertex* vertices,
                          int vertex_count, const ObjFace* faces, int face_count) {
    unsigned char represented[CAD_MAX_POINTS] = {0};
    int face_index;

    for (face_index = 0; face_index < face_count; face_index++) {
        const ObjFace* face = &faces[face_index];
        int16_t first_point = INVALID_INDEX;
        int16_t polygon;
        int i;

        for (i = face->count - 1; i >= 0; i--) {
            int source_vertex = face->vertex[i];
            int16_t point = CadCore_AddPoint(parsed,
                                             vertices[source_vertex].x,
                                             vertices[source_vertex].y,
                                             vertices[source_vertex].z);
            if (point == INVALID_INDEX) {
                fprintf(stderr,
                        "OBJ import error: CAD point capacity (%d) exceeded while building polygon %d\n",
                        CAD_MAX_POINTS, face_index + 1);
                return 0;
            }
            parsed->data.points[point].nextPoint = first_point;
            first_point = point;
            represented[source_vertex] = 1;
        }

        polygon = CadCore_AddPolygon(parsed, first_point, face->color,
                                     (uint8_t)face->count);
        if (polygon == INVALID_INDEX) {
            fprintf(stderr,
                    "OBJ import error: CAD polygon capacity (%d) exceeded\n",
                    CAD_MAX_POLYGONS);
            return 0;
        }
        parsed->data.polygons[polygon].animation = INVALID_INDEX;
    }

    /* Preserve genuine standalone OBJ vertices, but do not create the old
       importer's artificial orphan copy of every vertex used by a face. */
    for (face_index = 0; face_index < vertex_count; face_index++) {
        if (!represented[face_index] &&
            CadCore_AddPoint(parsed, vertices[face_index].x,
                             vertices[face_index].y,
                             vertices[face_index].z) == INVALID_INDEX) {
            fprintf(stderr,
                    "OBJ import error: CAD point capacity (%d) exceeded by standalone vertices\n",
                    CAD_MAX_POINTS);
            return 0;
        }
    }

    parsed->isDirty = 1;
    return 1;
}

int CadImport_OBJ(CadCore* core, const char* filename) {
    FILE* fp;
    ObjVertex vertices[CAD_MAX_POINTS];
    ObjFace faces[CAD_MAX_POLYGONS];
    int vertex_count = 0;
    int face_count = 0;
    int line_number = 0;
    uint8_t current_color = 0;
    char line[OBJ_LINE_CAPACITY];
    int read_result;
    CadCore* parsed;

    if (!core || !filename || filename[0] == '\0') {
        fprintf(stderr, "OBJ import error: core and filename are required\n");
        return 0;
    }

    fp = open_utf8_file(filename, "rb");
    if (!fp) {
        fprintf(stderr, "OBJ import error: could not open '%s' for reading\n", filename);
        return 0;
    }

    while ((read_result = read_obj_line(fp, line, sizeof(line), &line_number)) > 0) {
        char* cursor = skip_space(line);
        char* keyword_end;
        size_t keyword_length;

        if (*cursor == '\0' || *cursor == '#') {
            continue;
        }
        keyword_end = cursor;
        while (*keyword_end != '\0' && !isspace((unsigned char)*keyword_end)) {
            keyword_end++;
        }
        keyword_length = (size_t)(keyword_end - cursor);

        if (keyword_length == 1 && cursor[0] == 'v') {
            if (vertex_count == CAD_MAX_POINTS) {
                fprintf(stderr,
                        "OBJ import error at line %d: source exceeds the %d-vertex CAD limit\n",
                        line_number, CAD_MAX_POINTS);
                fclose(fp);
                return 0;
            }
            if (!parse_obj_vertex(keyword_end, &vertices[vertex_count])) {
                fprintf(stderr,
                        "OBJ import error at line %d: malformed or non-finite vertex\n",
                        line_number);
                fclose(fp);
                return 0;
            }
            vertex_count++;
        } else if ((keyword_length == 1 && cursor[0] == 'f') ||
                   (keyword_length == 1 && cursor[0] == 'l')) {
            ObjFace parsed_face;
            int is_line = cursor[0] == 'l';
            int records_needed;
            int i;

            if (!parse_obj_face(keyword_end, vertex_count, current_color,
                                &parsed_face, line_number)) {
                fclose(fp);
                return 0;
            }
            records_needed = is_line ? parsed_face.count - 1 : 1;
            if (records_needed > CAD_MAX_POLYGONS - face_count) {
                fprintf(stderr,
                        "OBJ import error at line %d: source exceeds the %d-polygon CAD limit\n",
                        line_number, CAD_MAX_POLYGONS);
                fclose(fp);
                return 0;
            }
            if (is_line) {
                /* A CAD colored line has exactly two endpoints. Preserve OBJ
                   polylines by importing each adjacent pair as one line. */
                for (i = 0; i < records_needed; i++) {
                    faces[face_count].vertex[0] = parsed_face.vertex[i];
                    faces[face_count].vertex[1] = parsed_face.vertex[i + 1];
                    faces[face_count].count = 2;
                    faces[face_count].color = parsed_face.color;
                    face_count++;
                }
            } else {
                faces[face_count++] = parsed_face;
            }
        } else if (keyword_length == 6 && strncmp(cursor, "usemtl", 6) == 0) {
            current_color = parse_material_color(keyword_end, current_color);
        }
    }

    if (read_result < 0) {
        fprintf(stderr,
                "OBJ import error at line %d: line is longer than %d bytes or the file could not be read\n",
                line_number, OBJ_LINE_CAPACITY - 1);
        fclose(fp);
        return 0;
    }
    if (fclose(fp) != 0) {
        fprintf(stderr, "OBJ import error: failed while closing '%s'\n", filename);
        return 0;
    }
    if (vertex_count == 0) {
        fprintf(stderr, "OBJ import error: '%s' contains no vertices\n", filename);
        return 0;
    }
    parsed = (CadCore*)malloc(sizeof(*parsed));
    if (!parsed) {
        fprintf(stderr, "OBJ import error: not enough memory for a temporary document\n");
        return 0;
    }
    CadCore_Init(parsed);
    if (!build_obj_core(parsed, vertices, vertex_count, faces, face_count)) {
        CadCore_Destroy(parsed);
        free(parsed);
        return 0;
    }

    /* Commit only after the entire source has parsed and fit in the CAD model. */
    CadCore_Destroy(core);
    *core = *parsed;
    free(parsed);

    fprintf(stdout, "Imported OBJ: %d source vertices, %d faces/lines, %d CAD points\n",
            vertex_count, face_count, core->data.pointCount);
    return 1;
}
