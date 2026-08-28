#include "cad_file.h"
#include "cad_codec.h"
#include "platform_fs.h"

#include <stdio.h>
#include <string.h>

static void print_failure(const char* operation, const char* filename,
                          const CadResult* result) {
    const char* message = CadStatus_Name(result ? result->status
                                                : CAD_STATUS_IO_ERROR);
    if (result && result->diagnosticCount)
        message = result->diagnostics[0].message;
    fprintf(stderr, "CAD %s failed for '%s': %s\n",
            operation, filename ? filename : "(null)", message);
}

void CadFile_Init(CadFileData* data) {
    int i;
    int frame;
    if (!data) return;
    memset(data, 0, sizeof(*data));
    for (i = 0; i < CAD_MAX_OBJECTS; ++i) {
        data->objects[i].parentObject = -1;
        data->objects[i].nextBrother = -1;
        data->objects[i].childObject = -1;
        data->objects[i].firstPolygon = -1;
    }
    for (i = 0; i < CAD_MAX_POLYGONS; ++i) {
        data->polygons[i].nextPolygon = -1;
        data->polygons[i].firstPoint = -1;
        data->polygons[i].animation = -1;
        data->polygons[i].both = -1;
    }
    for (i = 0; i < CAD_MAX_POINTS; ++i)
        data->points[i].nextPoint = -1;
    for (i = 0; i < CAD_MAX_ANIMATION_INDICES; ++i) {
        for (frame = 0; frame < CAD_ANIMATION_FRAMES; ++frame)
            data->animationIndices[i].frame[frame] = -1;
    }
    for (i = 0; i < CAD_MAX_ANIMATION_POINTS; ++i)
        data->animationPoints[i].nextPoint = -1;
}

void CadFile_Clear(CadFileData* data) {
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
    CadResult result;
    uint8_t* bytes = NULL;
    size_t size = 0;
    if (!filename || !data) return 0;
    result = CadPlatform_ReadFile(filename, CAD_PLATFORM_DEFAULT_FILE_LIMIT,
                                  &bytes, &size);
    if (CadResult_IsSuccess(&result))
        result = CadCodec_Decode(bytes, size, CAD_FORMAT_AUTO, data);
    CadPlatform_Free(bytes);
    if (!CadResult_IsSuccess(&result)) {
        print_failure("load", filename, &result);
        return 0;
    }
    return 1;
}

int CadFile_Save(const char* filename, const CadFileData* data) {
    CadResult result;
    uint8_t* bytes = NULL;
    size_t size = 0;
    if (!filename || !data) return 0;
    result = CadCodec_Encode(data, CAD_FORMAT_X11_STREAM, &bytes, &size);
    if (CadResult_IsSuccess(&result))
        result = CadPlatform_WriteFileAtomic(filename, bytes, size);
    CadCodec_FreeBuffer(bytes);
    if (!CadResult_IsSuccess(&result)) {
        print_failure("save", filename, &result);
        return 0;
    }
    return 1;
}
