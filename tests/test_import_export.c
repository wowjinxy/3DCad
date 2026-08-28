#define _CRT_SECURE_NO_WARNINGS

#include "cad_core.h"
#include "cad_export_3dg1.h"
#include "cad_export_obj.h"
#include "cad_import_3dg1.h"
#include "cad_import_obj.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

static int failures;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,          \
                    #condition);                                                \
            failures++;                                                         \
        }                                                                       \
    } while (0)

static unsigned long process_id(void) {
#ifdef _WIN32
    return (unsigned long)GetCurrentProcessId();
#else
    return (unsigned long)getpid();
#endif
}

static CadCore* allocate_core(void) {
    CadCore* core = (CadCore*)malloc(sizeof(*core));
    CHECK(core != NULL);
    if (core) {
        CadCore_Init(core);
    }
    return core;
}

static void make_path(char* path, size_t capacity, const char* label,
                      const char* extension) {
    snprintf(path, capacity, "3dcad-io-%lu-%s.%s",
             process_id(), label, extension);
}

static int write_bytes(const char* path, const void* bytes, size_t size) {
    FILE* file = fopen(path, "wb");
    int ok;
    if (!file) {
        return 0;
    }
    ok = fwrite(bytes, 1, size, file) == size;
    if (fclose(file) != 0) {
        ok = 0;
    }
    return ok;
}

static void remove_utf8(const char* path) {
#ifdef _WIN32
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                     path, -1, NULL, 0);
    if (length > 0) {
        wchar_t* wide = (wchar_t*)malloc((size_t)length * sizeof(wchar_t));
        if (wide) {
            if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                    path, -1, wide, length) > 0) {
                DeleteFileW(wide);
            }
            free(wide);
        }
    }
#else
    remove(path);
#endif
}

static char* read_file(const char* path) {
    FILE* file = fopen(path, "rb");
    long length;
    char* contents;
    if (!file || fseek(file, 0, SEEK_END) != 0) {
        if (file) fclose(file);
        return NULL;
    }
    length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    contents = (char*)malloc((size_t)length + 1);
    if (!contents) {
        fclose(file);
        return NULL;
    }
    if (fread(contents, 1, (size_t)length, file) != (size_t)length) {
        free(contents);
        fclose(file);
        return NULL;
    }
    contents[length] = '\0';
    fclose(file);
    return contents;
}

static void check_first_face(const CadCore* core, int expected_count,
                             int expected_color) {
    const CadPolygon* polygon = &core->data.polygons[0];
    int16_t point = polygon->firstPoint;
    int count = 0;
    CHECK(polygon->flags != 0);
    CHECK(polygon->npoints == expected_count);
    CHECK(polygon->color == expected_color);
    while (point != INVALID_INDEX && count < expected_count) {
        CHECK(point >= 0 && point < core->data.pointCount);
        if (point < 0 || point >= core->data.pointCount) break;
        CHECK(core->data.points[point].flags != 0);
        point = core->data.points[point].nextPoint;
        count++;
    }
    CHECK(count == expected_count);
}

static void check_valid_document(const CadCore* core) {
    char diagnostic[256];
    diagnostic[0] = '\0';
    if (!CadCore_ValidateDocument(core, diagnostic, sizeof(diagnostic))) {
        fprintf(stderr, "Document validation failed: %s\n", diagnostic);
        failures++;
    }
}

static void test_obj_negative_lines_and_transaction(void) {
    static const char valid_obj[] =
        "# negative indices are relative to this face\n"
        "v 0 0 0\n"
        "v 10 0 0\n"
        "v 0 10 0\n"
        "usemtl material_7\n"
        "f -3/1/1 -2//1 -1/2/1\n"
        "usemtl material_9\n"
        "l 1 2 3\n";
    static const char invalid_obj[] =
        "v 1 2 3\n"
        "f 0 1\n";
    char input[128];
    char invalid[128];
    char output[128];
    char material[128];
    char* exported;
    CadCore* core;
    CadCore* roundtrip;
    CadCore* before_failure;
    int old_points;
    int old_polygons;
    double old_x;

    make_path(input, sizeof(input), "valid", "obj");
    make_path(invalid, sizeof(invalid), "invalid", "obj");
    make_path(output, sizeof(output), "roundtrip", "obj");
    make_path(material, sizeof(material), "roundtrip", "mtl");
    CHECK(write_bytes(input, valid_obj, sizeof(valid_obj) - 1));
    CHECK(write_bytes(invalid, invalid_obj, sizeof(invalid_obj) - 1));

    core = allocate_core();
    roundtrip = allocate_core();
    before_failure = allocate_core();
    if (!core || !roundtrip || !before_failure) goto cleanup;
    CHECK(CadImport_OBJ(core, input));
    CHECK(core->data.objectCount == 1);
    CHECK(core->data.polygonCount == 3);
    CHECK(core->data.pointCount == 7); /* no extra three-vertex orphan table */
    check_first_face(core, 3, 7);
    CHECK(core->data.polygons[1].npoints == 2);
    CHECK(core->data.polygons[1].color == 9);
    CHECK(core->data.polygons[2].npoints == 2);
    CHECK(core->data.polygons[2].color == 9);
    CHECK(core->data.points[core->data.polygons[0].firstPoint].pointx == 0.0);
    check_valid_document(core);

    old_points = core->data.pointCount;
    old_polygons = core->data.polygonCount;
    old_x = core->data.points[core->data.polygons[0].firstPoint].pointx;
    *before_failure = *core;
    CHECK(!CadImport_OBJ(core, invalid));
    CHECK(memcmp(core, before_failure, sizeof(*core)) == 0);
    CHECK(core->data.pointCount == old_points);
    CHECK(core->data.polygonCount == old_polygons);
    CHECK(core->data.points[core->data.polygons[0].firstPoint].pointx == old_x);

    CHECK(CadExport_OBJ(core, output));
    exported = read_file(output);
    CHECK(exported != NULL);
    if (exported) {
        CHECK(strstr(exported, "\nf ") != NULL);
        CHECK(strstr(exported, "\nl ") != NULL);
        CHECK(strstr(exported, "usemtl material_7") != NULL);
        CHECK(strstr(exported, "usemtl material_9") != NULL);
        free(exported);
    }

    CHECK(CadImport_OBJ(roundtrip, output));
    CHECK(roundtrip->data.polygonCount == 3);
    check_first_face(roundtrip, 3, 7);
    CHECK(roundtrip->data.polygons[1].npoints == 2);
    CHECK(roundtrip->data.polygons[1].color == 9);
    CHECK(roundtrip->data.polygons[2].npoints == 2);
    CHECK(roundtrip->data.polygons[2].color == 9);
    check_valid_document(roundtrip);

cleanup:
    if (roundtrip) { CadCore_Destroy(roundtrip); free(roundtrip); }
    if (before_failure) { CadCore_Destroy(before_failure); free(before_failure); }
    if (core) { CadCore_Destroy(core); free(core); }
    remove(input);
    remove(invalid);
    remove(output);
    remove(material);
}

static void test_3dg1_dedup_dos_eof_and_roundtrip(void) {
    static const unsigned char input_data[] =
        "3DG1\r\n"
        "4\r\n"
        "0 0 0\r\n"
        "10 0 0\r\n"
        "0 10 0\r\n"
        "0 0 0\r\n"
        "3 0 1 2 7\r\n"
        "2 3 1 9\r\n"
        "\x1a"
        "this text after DOS EOF must be ignored";
    static const char invalid_data[] =
        "3DG1\n"
        "2\n"
        "0 0 0\n"
        "1 0 0\n"
        "2 0 99 1\n";
    char input[128];
    char invalid[128];
    char output[128];
    CadCore* core;
    CadCore* roundtrip;
    CadCore* before_failure;
    char* exported;
    int old_points;
    int old_polygons;

    make_path(input, sizeof(input), "valid", "3dg1");
    make_path(invalid, sizeof(invalid), "invalid", "3dg1");
    make_path(output, sizeof(output), "roundtrip", "3dg1");
    CHECK(write_bytes(input, input_data, sizeof(input_data) - 1));
    CHECK(write_bytes(invalid, invalid_data, sizeof(invalid_data) - 1));

    core = allocate_core();
    roundtrip = allocate_core();
    before_failure = allocate_core();
    if (!core || !roundtrip || !before_failure) goto cleanup;
    CHECK(CadImport_3DG1(core, input));
    CHECK(core->data.objectCount == 1);
    CHECK(core->data.polygonCount == 2);
    CHECK(core->data.pointCount == 5);
    check_first_face(core, 3, 7);
    CHECK(core->data.polygons[1].npoints == 2);
    CHECK(core->data.polygons[1].color == 9);
    check_valid_document(core);

    old_points = core->data.pointCount;
    old_polygons = core->data.polygonCount;
    *before_failure = *core;
    CHECK(!CadImport_3DG1(core, invalid));
    CHECK(memcmp(core, before_failure, sizeof(*core)) == 0);
    CHECK(core->data.pointCount == old_points);
    CHECK(core->data.polygonCount == old_polygons);

    CHECK(CadExport_3DG1(core, output));
    exported = read_file(output);
    CHECK(exported != NULL);
    if (exported) {
        CHECK(strstr(exported, "3DG1\n3\n") == exported);
        CHECK(strstr(exported, " 7\n") != NULL);
        CHECK(strstr(exported, " 9\n") != NULL);
        free(exported);
    }

    CHECK(CadImport_3DG1(roundtrip, output));
    CHECK(roundtrip->data.polygonCount == 2);
    check_first_face(roundtrip, 3, 7);
    CHECK(roundtrip->data.polygons[1].npoints == 2);
    CHECK(roundtrip->data.polygons[1].color == 9);
    check_valid_document(roundtrip);

cleanup:
    if (roundtrip) { CadCore_Destroy(roundtrip); free(roundtrip); }
    if (before_failure) { CadCore_Destroy(before_failure); free(before_failure); }
    if (core) { CadCore_Destroy(core); free(core); }
    remove(input);
    remove(invalid);
    remove(output);
}

static void test_maximum_face_size(void) {
    char path[128];
    char data[4096];
    size_t used = 0;
    int i;
    CadCore* core;

    make_path(path, sizeof(path), "max-face", "3dg1");
    used += (size_t)snprintf(data + used, sizeof(data) - used,
                             "3DG1\n%d\n", CAD_MAX_FACE_POINTS);
    for (i = 0; i < CAD_MAX_FACE_POINTS; i++) {
        used += (size_t)snprintf(data + used, sizeof(data) - used,
                                 "%d %d 0\n", i, i & 1);
    }
    used += (size_t)snprintf(data + used, sizeof(data) - used,
                             "%d", CAD_MAX_FACE_POINTS);
    for (i = 0; i < CAD_MAX_FACE_POINTS; i++) {
        used += (size_t)snprintf(data + used, sizeof(data) - used, " %d", i);
    }
    used += (size_t)snprintf(data + used, sizeof(data) - used, " 255\n\x1a");
    CHECK(used < sizeof(data));
    CHECK(write_bytes(path, data, used));

    core = allocate_core();
    if (core) {
        CHECK(CadImport_3DG1(core, path));
        CHECK(core->data.polygonCount == 1);
        CHECK(core->data.polygons[0].npoints == CAD_MAX_FACE_POINTS);
        CHECK(core->data.polygons[0].color == 255);
        check_valid_document(core);
        CadCore_Destroy(core);
        free(core);
    }
    remove(path);
}

static void test_unicode_paths(void) {
    char obj_path[160];
    char mtl_path[160];
    char three_dg1_path[160];
    CadCore* source;
    CadCore* imported;
    int16_t first;
    int16_t second;

    snprintf(obj_path, sizeof(obj_path),
             "3dcad-io-%lu-unicode-\xe6\xa8\xa1\xe5\x9e\x8b.obj", process_id());
    snprintf(mtl_path, sizeof(mtl_path),
             "3dcad-io-%lu-unicode-\xe6\xa8\xa1\xe5\x9e\x8b.mtl", process_id());
    snprintf(three_dg1_path, sizeof(three_dg1_path),
             "3dcad-io-%lu-unicode-\xe6\xa8\xa1\xe5\x9e\x8b.3dg1", process_id());

    source = allocate_core();
    imported = allocate_core();
    if (!source || !imported) goto cleanup;
    first = CadCore_AddPoint(source, 1.0, 2.0, 3.0);
    second = CadCore_AddPoint(source, 4.0, 5.0, 6.0);
    CHECK(first != INVALID_INDEX && second != INVALID_INDEX);
    if (first != INVALID_INDEX && second != INVALID_INDEX) {
        source->data.points[first].nextPoint = second;
        CHECK(CadCore_AddPolygon(source, first, 12, 2) != INVALID_INDEX);
    }

    CHECK(CadExport_OBJ(source, obj_path));
    CHECK(CadImport_OBJ(imported, obj_path));
    CHECK(imported->data.polygonCount == 1);
    check_valid_document(imported);
    CadCore_Clear(imported);

    CHECK(CadExport_3DG1(source, three_dg1_path));
    CHECK(CadImport_3DG1(imported, three_dg1_path));
    CHECK(imported->data.polygonCount == 1);
    check_valid_document(imported);

cleanup:
    if (imported) { CadCore_Destroy(imported); free(imported); }
    if (source) { CadCore_Destroy(source); free(source); }
    remove_utf8(obj_path);
    remove_utf8(mtl_path);
    remove_utf8(three_dg1_path);
}

static void test_capacity_failure_is_transactional(void) {
    char path[128];
    FILE* file;
    int polygon_count = CAD_MAX_POINTS / CAD_MAX_FACE_POINTS + 1;
    int i;
    int j;
    CadCore* core;
    CadCore* before_failure;

    make_path(path, sizeof(path), "capacity", "obj");
    file = fopen(path, "wb");
    CHECK(file != NULL);
    if (!file) {
        return;
    }
    for (i = 0; i < CAD_MAX_FACE_POINTS; i++) {
        fprintf(file, "v %d %d 0\n", i, i & 1);
    }
    for (i = 0; i < polygon_count; i++) {
        fputc('f', file);
        for (j = 0; j < CAD_MAX_FACE_POINTS; j++) {
            fprintf(file, " %d", j + 1);
        }
        fputc('\n', file);
    }
    CHECK(fclose(file) == 0);

    core = allocate_core();
    before_failure = allocate_core();
    if (core && before_failure) {
        CHECK(CadCore_AddPoint(core, 99.0, 98.0, 97.0) != INVALID_INDEX);
        *before_failure = *core;
        CHECK(!CadImport_OBJ(core, path));
        CHECK(memcmp(core, before_failure, sizeof(*core)) == 0);
    }
    if (before_failure) { CadCore_Destroy(before_failure); free(before_failure); }
    if (core) { CadCore_Destroy(core); free(core); }
    remove(path);
}

static void make_simple_triangle(CadCore* core) {
    int16_t a;
    int16_t b;
    int16_t c;
    if (!core) return;
    a = CadCore_AddPoint(core, 0.0, 0.0, 0.0);
    b = CadCore_AddPoint(core, 10.0, 0.0, 0.0);
    c = CadCore_AddPoint(core, 0.0, 10.0, 0.0);
    CHECK(a != INVALID_INDEX && b != INVALID_INDEX && c != INVALID_INDEX);
    if (a == INVALID_INDEX || b == INVALID_INDEX || c == INVALID_INDEX) return;
    core->data.points[a].nextPoint = b;
    core->data.points[b].nextPoint = c;
    core->data.points[c].nextPoint = INVALID_INDEX;
    CHECK(CadCore_AddPolygon(core, a, 17, 3) != INVALID_INDEX);
}

static void check_file_equals(const char* path, const char* expected) {
    char* contents = read_file(path);
    CHECK(contents != NULL);
    if (contents) {
        CHECK(strcmp(contents, expected) == 0);
        free(contents);
    }
}

static int count_export_artifacts(const char* destination) {
    static const char marker[] = ".3dcad-";
    const char* basename = destination;
    const char* forward = strrchr(destination, '/');
    const char* backward = strrchr(destination, '\\');
    char prefix[640];
    size_t prefix_length;
    int count = 0;
    int written;

    if (forward) basename = forward + 1;
    if (backward && backward + 1 > basename) basename = backward + 1;
    written = snprintf(prefix, sizeof(prefix), "%s%s", basename, marker);
    if (written < 0 || (size_t)written >= sizeof(prefix)) {
        return -1;
    }
    prefix_length = strlen(prefix);
#ifdef _WIN32
    {
        WIN32_FIND_DATAA data;
        char pattern[648];
        HANDLE search;
        written = snprintf(pattern, sizeof(pattern), "%s*", prefix);
        if (written < 0 || (size_t)written >= sizeof(pattern)) {
            return -1;
        }
        search = FindFirstFileA(pattern, &data);
        if (search == INVALID_HANDLE_VALUE) {
            return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;
        }
        do {
            if (strncmp(data.cFileName, prefix, prefix_length) == 0) count++;
        } while (FindNextFileA(search, &data));
        FindClose(search);
    }
#else
    {
        DIR* directory = opendir(".");
        struct dirent* entry;
        if (!directory) return -1;
        while ((entry = readdir(directory)) != NULL) {
            if (strncmp(entry->d_name, prefix, prefix_length) == 0) count++;
        }
        closedir(directory);
    }
#endif
    return count;
}

static void test_obj_pair_replacement_and_collision(void) {
    static const char obj_sentinel[] = "old pair OBJ\n";
    static const char mtl_sentinel[] = "old pair MTL\n";
    static const char collision_sentinel[] = "not an OBJ destination\n";
    char obj_path[128];
    char mtl_path[128];
    char collision_path[128];
    CadCore* core = allocate_core();
    char* obj_contents;
    char* mtl_contents;

    if (!core) return;
    make_simple_triangle(core);
    make_path(obj_path, sizeof(obj_path), "atomic-pair", "obj");
    make_path(mtl_path, sizeof(mtl_path), "atomic-pair", "mtl");
    make_path(collision_path, sizeof(collision_path), "same-output", "mtl");
    CHECK(write_bytes(obj_path, obj_sentinel, sizeof(obj_sentinel) - 1));
    CHECK(write_bytes(mtl_path, mtl_sentinel, sizeof(mtl_sentinel) - 1));
    CHECK(CadExport_OBJ(core, obj_path));
    obj_contents = read_file(obj_path);
    mtl_contents = read_file(mtl_path);
    CHECK(obj_contents != NULL && strstr(obj_contents, "mtllib ") != NULL);
    CHECK(mtl_contents != NULL && strstr(mtl_contents, "newmtl material_17") != NULL);
    free(obj_contents);
    free(mtl_contents);
    CHECK(count_export_artifacts(obj_path) == 0);
    CHECK(count_export_artifacts(mtl_path) == 0);

    CHECK(write_bytes(collision_path, collision_sentinel,
                      sizeof(collision_sentinel) - 1));
    CHECK(!CadExport_OBJ(core, collision_path));
    check_file_equals(collision_path, collision_sentinel);
    CHECK(count_export_artifacts(collision_path) == 0);

    remove_utf8(obj_path);
    remove_utf8(mtl_path);
    remove_utf8(collision_path);
    CadCore_Destroy(core);
    free(core);
}

#ifdef _WIN32
static HANDLE open_without_delete_sharing(const char* path) {
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                     path, -1, NULL, 0);
    wchar_t* wide = length > 0
        ? (wchar_t*)malloc((size_t)length * sizeof(*wide)) : NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    if (wide && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                    path, -1, wide, length) > 0) {
        file = CreateFileW(wide, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    free(wide);
    return file;
}

static void test_obj_pair_partial_backup_rolls_back(void) {
    static const char obj_sentinel[] = "locked old OBJ\n";
    static const char mtl_sentinel[] = "old MTL moved before the failure\n";
    char obj_path[128];
    char mtl_path[128];
    CadCore* core = allocate_core();
    HANDLE locked_obj;

    if (!core) return;
    make_simple_triangle(core);
    make_path(obj_path, sizeof(obj_path), "backup-rollback", "obj");
    make_path(mtl_path, sizeof(mtl_path), "backup-rollback", "mtl");
    CHECK(write_bytes(obj_path, obj_sentinel, sizeof(obj_sentinel) - 1));
    CHECK(write_bytes(mtl_path, mtl_sentinel, sizeof(mtl_sentinel) - 1));
    locked_obj = open_without_delete_sharing(obj_path);
    CHECK(locked_obj != INVALID_HANDLE_VALUE);
    if (locked_obj != INVALID_HANDLE_VALUE) {
        /* MTL backup happens first. The locked OBJ then forces the second
           backup to fail, exercising restoration of the already-moved MTL. */
        CHECK(!CadExport_OBJ(core, obj_path));
        CloseHandle(locked_obj);
    }
    check_file_equals(obj_path, obj_sentinel);
    check_file_equals(mtl_path, mtl_sentinel);
    CHECK(count_export_artifacts(obj_path) == 0);
    CHECK(count_export_artifacts(mtl_path) == 0);

    remove_utf8(obj_path);
    remove_utf8(mtl_path);
    CadCore_Destroy(core);
    free(core);
}
#endif

static void test_invalid_topology_does_not_overwrite_exports(void) {
    static const char obj_sentinel[] = "existing OBJ must survive\n";
    static const char mtl_sentinel[] = "existing MTL must survive\n";
    static const char three_dg1_sentinel[] = "existing 3DG1 must survive\n";
    char obj_path[128];
    char mtl_path[128];
    char three_dg1_path[128];
    CadCore* core = allocate_core();
    int16_t extra;
    int16_t point;

    make_path(obj_path, sizeof(obj_path), "atomic-invalid", "obj");
    make_path(mtl_path, sizeof(mtl_path), "atomic-invalid", "mtl");
    make_path(three_dg1_path, sizeof(three_dg1_path), "atomic-invalid", "3dg1");
    CHECK(write_bytes(obj_path, obj_sentinel, sizeof(obj_sentinel) - 1));
    CHECK(write_bytes(mtl_path, mtl_sentinel, sizeof(mtl_sentinel) - 1));
    CHECK(write_bytes(three_dg1_path, three_dg1_sentinel,
                      sizeof(three_dg1_sentinel) - 1));

    if (core) {
        make_simple_triangle(core);
        extra = CadCore_AddPoint(core, 20.0, 20.0, 20.0);
        CHECK(extra != INVALID_INDEX);
        point = core->data.polygons[0].firstPoint;
        if (point != INVALID_INDEX) point = core->data.points[point].nextPoint;
        if (point != INVALID_INDEX) point = core->data.points[point].nextPoint;
        CHECK(point != INVALID_INDEX);
        if (point != INVALID_INDEX && extra != INVALID_INDEX) {
            /* The face still declares three points, but its chain contains a
               fourth. Export validation must fail before touching targets. */
            core->data.points[point].nextPoint = extra;
            CHECK(!CadExport_OBJ(core, obj_path));
            CHECK(!CadExport_3DG1(core, three_dg1_path));
        }
        CadCore_Destroy(core);
        free(core);
    }

    check_file_equals(obj_path, obj_sentinel);
    check_file_equals(mtl_path, mtl_sentinel);
    check_file_equals(three_dg1_path, three_dg1_sentinel);
    remove(obj_path);
    remove(mtl_path);
    remove(three_dg1_path);
}

/* Locate a legal destination filename whose component/path limit prevents
   adding even eight more bytes. Export staging suffixes are always longer,
   giving a deterministic open failure without test-only hooks or disk-full
   assumptions. The already-existing destination must remain byte-identical. */
static int make_unextendable_existing_path(char* path, size_t capacity,
                                           const char* extension,
                                           const char* sentinel) {
    char prefix[96];
    char probe[512];
    size_t prefix_length;
    size_t extension_length = strlen(extension);
    int target_length;

    snprintf(prefix, sizeof(prefix), "3dcad-io-%lu-atomic-limit-", process_id());
    prefix_length = strlen(prefix);
    for (target_length = 250; target_length >= 80; --target_length) {
        size_t fill_length;
        size_t path_length;
        if ((size_t)target_length + 1 > capacity ||
            (size_t)target_length <= prefix_length + extension_length) {
            continue;
        }
        memcpy(path, prefix, prefix_length);
        fill_length = (size_t)target_length - prefix_length - extension_length;
        memset(path + prefix_length, 'x', fill_length);
        memcpy(path + prefix_length + fill_length, extension,
               extension_length + 1);
        if (!write_bytes(path, sentinel, strlen(sentinel))) continue;

        path_length = strlen(path);
        if (path_length + 8 + 1 > sizeof(probe)) {
            remove(path);
            continue;
        }
        memcpy(probe, path, path_length);
        memcpy(probe + path_length, "12345678", 9);
        if (!write_bytes(probe, "probe", 5)) return 1;
        remove(probe);
        remove(path);
    }
    path[0] = '\0';
    return 0;
}

static void test_staging_open_failure_preserves_existing(void) {
    static const char obj_sentinel[] = "old OBJ\n";
    static const char mtl_sentinel[] = "old MTL\n";
    static const char three_dg1_sentinel[] = "old 3DG1\n";
    char obj_path[512];
    char mtl_path[512];
    char three_dg1_path[512];
    CadCore* core = allocate_core();
    char* dot;

    if (!core) return;
    make_simple_triangle(core);

    CHECK(make_unextendable_existing_path(obj_path, sizeof(obj_path), ".obj",
                                          obj_sentinel));
    if (obj_path[0]) {
        memcpy(mtl_path, obj_path, strlen(obj_path) + 1);
        dot = strrchr(mtl_path, '.');
        CHECK(dot != NULL);
        if (dot) memcpy(dot, ".mtl", 5);
        CHECK(write_bytes(mtl_path, mtl_sentinel, sizeof(mtl_sentinel) - 1));
        CHECK(!CadExport_OBJ(core, obj_path));
        check_file_equals(obj_path, obj_sentinel);
        check_file_equals(mtl_path, mtl_sentinel);
        remove(obj_path);
        remove(mtl_path);
    }

    CHECK(make_unextendable_existing_path(three_dg1_path,
                                          sizeof(three_dg1_path), ".3dg1",
                                          three_dg1_sentinel));
    if (three_dg1_path[0]) {
        CHECK(!CadExport_3DG1(core, three_dg1_path));
        check_file_equals(three_dg1_path, three_dg1_sentinel);
        remove(three_dg1_path);
    }

    CadCore_Destroy(core);
    free(core);
}

int main(void) {
    test_obj_negative_lines_and_transaction();
    test_3dg1_dedup_dos_eof_and_roundtrip();
    test_maximum_face_size();
    test_unicode_paths();
    test_capacity_failure_is_transactional();
    test_invalid_topology_does_not_overwrite_exports();
    test_staging_open_failure_preserves_existing();
    test_obj_pair_replacement_and_collision();
#ifdef _WIN32
    test_obj_pair_partial_backup_rolls_back();
#endif

    if (failures != 0) {
        fprintf(stderr, "%d import/export test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    puts("Import/export tests passed");
    return EXIT_SUCCESS;
}
