#define _CRT_SECURE_NO_WARNINGS

#include "cad_file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* Endianness conversion helpers */
static inline int16_t swap_int16(int16_t value) {
    return (int16_t)(((value & 0xFF) << 8) | ((value & 0xFF00) >> 8));
}

static inline uint16_t swap_uint16(uint16_t value) {
    return (uint16_t)(((value & 0xFF) << 8) | ((value & 0xFF00) >> 8));
}

static inline double swap_double(double value) {
    union { double d; uint64_t i; } u;
    u.d = value;
    u.i = ((u.i & 0xFF00000000000000ULL) >> 56) |
          ((u.i & 0x00FF000000000000ULL) >> 40) |
          ((u.i & 0x0000FF0000000000ULL) >> 24) |
          ((u.i & 0x000000FF00000000ULL) >> 8)  |
          ((u.i & 0x00000000FF000000ULL) << 8)  |
          ((u.i & 0x0000000000FF0000ULL) << 24) |
          ((u.i & 0x000000000000FF00ULL) << 40) |
          ((u.i & 0x00000000000000FFULL) << 56);
    return u.d;
}

/* Check if system is little-endian (Windows/x86 is little-endian) */
static inline int is_little_endian(void) {
    union { uint16_t i; uint8_t c[2]; } u;
    u.i = 0x0102;
    return u.c[0] == 0x02; /* Little-endian: LSB first */
}

void CadFile_Init(CadFileData* data) {
    if (!data) return;
    memset(data, 0, sizeof(CadFileData));
}

void CadFile_Clear(CadFileData* data) {
    if (!data) return;
    CadFile_Init(data);
}

CadPoint* CadFile_GetPoint(CadFileData* data, int16_t index) {
    if (!data || index < 0 || index >= CAD_MAX_POINTS) return NULL;
    return &data->points[index];
}

CadPolygon* CadFile_GetPolygon(CadFileData* data, int16_t index) {
    if (!data || index < 0 || index >= CAD_MAX_POLYGONS) return NULL;
    return &data->polygons[index];
}

CadObject* CadFile_GetObject(CadFileData* data, int16_t index) {
    if (!data || index < 0 || index >= CAD_MAX_OBJECTS) return NULL;
    return &data->objects[index];
}

int CadFile_Load(const char* filename, CadFileData* data) {
    if (!filename || !data) {
        fprintf(stderr, "Error: Invalid parameters to CadFile_Load\n");
        return 0;
    }
    
    /* Convert UTF-8 filename to wide string for Windows */
    wchar_t wfilename[MAX_PATH * 2] = {0};
    int wlen = MultiByteToWideChar(CP_UTF8, 0, filename, -1, wfilename, sizeof(wfilename) / sizeof(wfilename[0]));
    if (wlen <= 0) {
        fprintf(stderr, "Error: Failed to convert filename to wide string: '%s'\n", filename);
        return 0;
    }
    
    FILE* fp = _wfopen(wfilename, L"rb");
    if (!fp) {
        fprintf(stderr, "Error: Could not open file '%s' for reading\n", filename);
        return 0;
    }
    
    CadFile_Init(data);
    
    uint8_t tag;
    int16_t index;
    size_t bytes_read = 0;
    
    /* Check file size */
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    if (file_size == 0) {
        fprintf(stderr, "Error: File is empty\n");
        fclose(fp);
        return 0;
    }
    
    fprintf(stdout, "Loading CAD file (size: %ld bytes)...\n", file_size);
    
    while (fread(&tag, sizeof(uint8_t), 1, fp) == 1) {
        bytes_read++;
        switch (tag) {
        case CAD_TAG_OBJECT:
            if (fread(&index, sizeof(int16_t), 1, fp) != 1) {
                fprintf(stderr, "Error: Unexpected end of file while reading object index (at byte %zu)\n", bytes_read);
                fclose(fp);
                return 0;
            }
            /* Convert from big-endian if needed (file format appears to be big-endian) */
            if (is_little_endian()) {
                index = swap_int16(index);
            }
            /* Debug: print first few indices to understand format */
            if (data->objectCount < 5) {
                fprintf(stdout, "Debug: Object tag, index=%d (0x%04X) at byte %zu\n", index, (unsigned short)index, bytes_read);
            }
            if (index >= 0 && index < CAD_MAX_OBJECTS) {
                CadObject temp_obj;
                if (fread(&temp_obj, sizeof(CadObject), 1, fp) != 1) {
                    fprintf(stderr, "Error: Failed to read object data for index %d (at byte %zu)\n", index, bytes_read);
                    fclose(fp);
                    return 0;
                }
                /* Convert endianness for multi-byte fields */
                if (is_little_endian()) {
                    temp_obj.parentObject = swap_int16(temp_obj.parentObject);
                    temp_obj.nextBrother = swap_int16(temp_obj.nextBrother);
                    temp_obj.childObject = swap_int16(temp_obj.childObject);
                    temp_obj.firstPolygon = swap_int16(temp_obj.firstPolygon);
                    temp_obj.offsetx = swap_double(temp_obj.offsetx);
                    temp_obj.offsety = swap_double(temp_obj.offsety);
                    temp_obj.offsetz = swap_double(temp_obj.offsetz);
                }
                data->objects[index] = temp_obj;
                if (index >= data->objectCount) data->objectCount = index + 1;
            } else {
                fprintf(stderr, "Warning: Object index %d out of bounds (0-%d), skipping\n", index, CAD_MAX_OBJECTS - 1);
                /* Skip the data for this invalid index */
                fseek(fp, sizeof(CadObject), SEEK_CUR);
            }
            break;
            
        case CAD_TAG_POLYGON: {
            if (fread(&index, sizeof(int16_t), 1, fp) != 1) {
                fprintf(stderr, "Error: Unexpected end of file while reading polygon index (at byte %zu)\n", bytes_read);
                fclose(fp);
                return 0;
            }
            /* Convert from big-endian if needed (file format appears to be big-endian) */
            if (is_little_endian()) {
                index = swap_int16(index);
            }
            /* The file format appears to store indices as byte offsets from base address */
            /* Convert byte offset to array index */
            int actual_index = -1;
            
            /* Check if it's a valid direct index first */
            if (index >= 0 && index < CAD_MAX_POLYGONS) {
                actual_index = index;
            } else {
                /* Try interpreting as byte offset - divide by structure size */
                /* CadPolygon is typically 16 bytes with alignment */
                const int poly_size = sizeof(CadPolygon);
                if (index > 0 && (index % poly_size) == 0) {
                    actual_index = index / poly_size;
                    if (actual_index >= CAD_MAX_POLYGONS) {
                        actual_index = -1; /* Still out of bounds */
                    }
                } else {
                    /* Try as unsigned 16-bit value */
                    uint16_t uindex = (uint16_t)index;
                    if (uindex < CAD_MAX_POLYGONS) {
                        actual_index = uindex;
                    }
                }
            }
            
            if (actual_index >= 0 && actual_index < CAD_MAX_POLYGONS) {
                index = actual_index;
                CadPolygon temp_poly;
                if (fread(&temp_poly, sizeof(CadPolygon), 1, fp) != 1) {
                    fprintf(stderr, "Error: Failed to read polygon data for index %d (at byte %zu)\n", index, bytes_read);
                    fclose(fp);
                    return 0;
                }
                /* Convert endianness for multi-byte fields */
                if (is_little_endian()) {
                    temp_poly.nextPolygon = swap_int16(temp_poly.nextPolygon);
                    temp_poly.firstPoint = swap_int16(temp_poly.firstPoint);
                    temp_poly.animation = swap_int16(temp_poly.animation);
                    temp_poly.both = swap_int16(temp_poly.both);
                }
                data->polygons[index] = temp_poly;
                if (index >= data->polygonCount) data->polygonCount = index + 1;
            } else {
                fprintf(stderr, "Warning: Polygon index %d out of bounds (0-%d), skipping\n", index, CAD_MAX_POLYGONS - 1);
                /* Skip the data for this invalid index */
                fseek(fp, sizeof(CadPolygon), SEEK_CUR);
            }
            break;
        }
            
        case CAD_TAG_POINT: {
            if (fread(&index, sizeof(int16_t), 1, fp) != 1) {
                fprintf(stderr, "Error: Unexpected end of file while reading point index (at byte %zu)\n", bytes_read);
                fclose(fp);
                return 0;
            }
            /* Convert from big-endian if needed (file format appears to be big-endian) */
            if (is_little_endian()) {
                index = swap_int16(index);
            }
            /* The file format appears to store indices as byte offsets from base address */
            /* Convert byte offset to array index */
            int actual_index = -1;
            
            /* Check if it's a valid direct index first */
            if (index >= 0 && index < CAD_MAX_POINTS) {
                actual_index = index;
            } else {
                /* Try interpreting as byte offset - divide by structure size */
                /* CadPoint is typically 32 bytes with alignment (2+2+2+8+8+8 = 30, aligned to 32) */
                const int point_size = sizeof(CadPoint);
                if (index > 0 && (index % point_size) == 0) {
                    actual_index = index / point_size;
                    if (actual_index >= CAD_MAX_POINTS) {
                        actual_index = -1; /* Still out of bounds */
                    }
                } else {
                    /* Try as unsigned 16-bit value */
                    uint16_t uindex = (uint16_t)index;
                    if (uindex < CAD_MAX_POINTS) {
                        actual_index = uindex;
                    }
                }
            }
            
            if (actual_index >= 0 && actual_index < CAD_MAX_POINTS) {
                index = actual_index;
                CadPoint temp_point;
                if (fread(&temp_point, sizeof(CadPoint), 1, fp) != 1) {
                    fprintf(stderr, "Error: Failed to read point data for index %d (at byte %zu)\n", index, bytes_read);
                    fclose(fp);
                    return 0;
                }
                /* Convert endianness for multi-byte fields */
                if (is_little_endian()) {
                    temp_point.nextPoint = swap_int16(temp_point.nextPoint);
                    temp_point.pointx = swap_double(temp_point.pointx);
                    temp_point.pointy = swap_double(temp_point.pointy);
                    temp_point.pointz = swap_double(temp_point.pointz);
                }
                data->points[index] = temp_point;
                if (index >= data->pointCount) data->pointCount = index + 1;
            } else {
                fprintf(stderr, "Warning: Point index %d out of bounds (0-%d), skipping\n", index, CAD_MAX_POINTS - 1);
                /* Skip the data for this invalid index */
                fseek(fp, sizeof(CadPoint), SEEK_CUR);
            }
            break;
        }
            
        default:
            /* Unknown tag - this might indicate a different file format */
            fprintf(stderr, "Error: Unknown tag %d (0x%02X) encountered at byte %zu (expected 0=Object, 1=Polygon, 2=Point)\n", tag, tag, bytes_read);
            fprintf(stderr, "This might indicate the file uses a different format or is corrupted.\n");
            /* Try to read a few more bytes to see what's in the file */
            uint8_t peek[16];
            long pos = ftell(fp);
            if (fread(peek, 1, sizeof(peek), fp) > 0) {
                fprintf(stderr, "Next 16 bytes: ");
                for (int i = 0; i < 16 && i < sizeof(peek); i++) {
                    fprintf(stderr, "%02X ", peek[i]);
                }
                fprintf(stderr, "\n");
                fseek(fp, pos, SEEK_SET);
            }
            fclose(fp);
            return 0;
        }
    }
    
    fclose(fp);
    return 1;
}

int CadFile_Save(const char* filename, const CadFileData* data) {
    if (!filename || !data) {
        fprintf(stderr, "Error: Invalid parameters to CadFile_Save\n");
        return 0;
    }
    
    /* Convert UTF-8 filename to wide string for Windows */
    wchar_t wfilename[MAX_PATH * 2] = {0};
    int wlen = MultiByteToWideChar(CP_UTF8, 0, filename, -1, wfilename, sizeof(wfilename) / sizeof(wfilename[0]));
    if (wlen <= 0) {
        fprintf(stderr, "Error: Failed to convert filename to wide string: '%s'\n", filename);
        return 0;
    }
    
    FILE* fp = _wfopen(wfilename, L"wb");
    if (!fp) {
        fprintf(stderr, "Error: Could not open file '%s' for writing\n", filename);
        return 0;
    }
    
    /* Write all objects */
    for (int i = 0; i < data->objectCount; i++) {
        if (data->objects[i].flags != 0) {
            uint8_t tag = CAD_TAG_OBJECT;
            int16_t index = (int16_t)i;
            
            /* Convert index to big-endian if needed */
            if (is_little_endian()) {
                index = swap_int16(index);
            }
            
            fwrite(&tag, sizeof(uint8_t), 1, fp);
            fwrite(&index, sizeof(int16_t), 1, fp);
            
            /* Convert object to big-endian and write */
            CadObject obj = data->objects[i];
            if (is_little_endian()) {
                obj.parentObject = swap_int16(obj.parentObject);
                obj.nextBrother = swap_int16(obj.nextBrother);
                obj.childObject = swap_int16(obj.childObject);
                obj.firstPolygon = swap_int16(obj.firstPolygon);
                obj.offsetx = swap_double(obj.offsetx);
                obj.offsety = swap_double(obj.offsety);
                obj.offsetz = swap_double(obj.offsetz);
            }
            fwrite(&obj, sizeof(CadObject), 1, fp);
        }
    }
    
    /* Write all polygons */
    for (int i = 0; i < data->polygonCount; i++) {
        if (data->polygons[i].flags != 0) {
            uint8_t tag = CAD_TAG_POLYGON;
            int16_t index = (int16_t)i;
            
            /* Convert index to big-endian if needed */
            if (is_little_endian()) {
                index = swap_int16(index);
            }
            
            fwrite(&tag, sizeof(uint8_t), 1, fp);
            fwrite(&index, sizeof(int16_t), 1, fp);
            
            /* Convert polygon to big-endian and write */
            CadPolygon poly = data->polygons[i];
            if (is_little_endian()) {
                poly.nextPolygon = swap_int16(poly.nextPolygon);
                poly.firstPoint = swap_int16(poly.firstPoint);
                poly.animation = swap_int16(poly.animation);
                poly.both = swap_int16(poly.both);
            }
            fwrite(&poly, sizeof(CadPolygon), 1, fp);
        }
    }
    
    /* Write all points */
    for (int i = 0; i < data->pointCount; i++) {
        if (data->points[i].flags != 0) {
            uint8_t tag = CAD_TAG_POINT;
            int16_t index = (int16_t)i;
            
            /* Convert index to big-endian if needed */
            if (is_little_endian()) {
                index = swap_int16(index);
            }
            
            fwrite(&tag, sizeof(uint8_t), 1, fp);
            fwrite(&index, sizeof(int16_t), 1, fp);
            
            /* Convert point to big-endian and write */
            CadPoint pt = data->points[i];
            if (is_little_endian()) {
                pt.nextPoint = swap_int16(pt.nextPoint);
                pt.pointx = swap_double(pt.pointx);
                pt.pointy = swap_double(pt.pointy);
                pt.pointz = swap_double(pt.pointz);
            }
            fwrite(&pt, sizeof(CadPoint), 1, fp);
        }
    }
    
    fclose(fp);
    return 1;
}

