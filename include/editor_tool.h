#pragma once

#include "cad_document.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Values are stable semantic identities.  They currently match resource slots
   for compatibility, but GUI behavior never dispatches on anonymous integers. */
typedef enum CadToolId {
    CAD_TOOL_POINT_SELECT = 0,
    CAD_TOOL_FACE_SELECT,
    CAD_TOOL_POINT_CREATE,
    CAD_TOOL_FACE_CREATE,
    CAD_TOOL_FACE_INSERT_POINT,
    CAD_TOOL_FACE_COLOR,
    CAD_TOOL_POINT_MOVE,
    CAD_TOOL_FACE_MOVE,
    CAD_TOOL_POINT_ROTATE,
    CAD_TOOL_FACE_ROTATE,
    CAD_TOOL_POINT_SCALE,
    CAD_TOOL_FACE_SCALE,
    CAD_TOOL_POINT_DELETE,
    CAD_TOOL_FACE_DELETE,
    CAD_TOOL_POINT_FLIP,
    CAD_TOOL_MIRROR,
    CAD_TOOL_FACE_REVERSE,
    CAD_TOOL_FACE_COPY,
    CAD_TOOL_FACE_CUT,
    CAD_TOOL_FACE_SIDE,
    CAD_TOOL_STATE,
    CAD_TOOL_TRANSFER,
    CAD_TOOL_PRIMITIVE,
    CAD_TOOL_UNDO,
    CAD_TOOL_COUNT,
    CAD_TOOL_NONE = -1
} CadToolId;

enum {
    CAD_TOOL_FLAG_POINT = 1u << 0,
    CAD_TOOL_FLAG_FACE = 1u << 1,
    CAD_TOOL_FLAG_IMMEDIATE = 1u << 2,
    CAD_TOOL_FLAG_DISABLED = 1u << 3
};

typedef struct CadToolDescriptor {
    CadToolId id;
    const char* name;
    const char* help;
    uint16_t flags;
} CadToolDescriptor;

typedef enum EditorToolPhase {
    EDITOR_TOOL_IDLE = 0,
    EDITOR_TOOL_ACTIVE,
    EDITOR_TOOL_UPDATED
} EditorToolPhase;

/* Shared edit-gesture controller.  Every input path uses the same document
   transaction contract: begin before mutation, update while applying a
   gesture, then either commit one undo entry or cancel atomically. */
typedef struct EditorTool {
    CadDocument* document;
    CadToolId id;
    EditorToolPhase phase;
} EditorTool;

void EditorTool_Init(EditorTool* tool, CadDocument* document);
void EditorTool_BindDocument(EditorTool* tool, CadDocument* document);
int EditorTool_IsActive(const EditorTool* tool);
CadResult EditorTool_Begin(EditorTool* tool, CadToolId id);
CadResult EditorTool_BeginNamed(EditorTool* tool, CadToolId id,
                                const char* historyLabel);
CadResult EditorTool_Update(EditorTool* tool);
CadResult EditorTool_Commit(EditorTool* tool);
void EditorTool_Cancel(EditorTool* tool);

#ifdef __cplusplus
}
#endif
