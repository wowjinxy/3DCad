#pragma once

#include "editor_commands.h"
#include "editor_tool.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAD_COMMAND_DISABLED_REASON_CAPACITY 160

typedef struct CadCommandState {
    int enabled;
    int checked;
    const char* shortcut;
    char disabledReason[CAD_COMMAND_DISABLED_REASON_CAPACITY];
} CadCommandState;

/* UI-owned facts are supplied as plain values so capability queries remain
   portable and independently testable. */
typedef struct EditorCommandContext {
    CadToolId activeTool;
    int selectedPointCount;
    int selectedPolygonCount;
    int clipboardHasData;
    int viewVisible[4];
    int coordinatesVisible;
    int toolPaletteVisible;
    int statePanelVisible;
    int animationPanelVisible;
    int palettePanelVisible;
    int wireframe3D;
} EditorCommandContext;

typedef struct EditorController {
    CadDocument* document;
    EditorTool tool;
} EditorController;

void EditorController_Init(EditorController* controller,
                           CadDocument* document);
void EditorController_BindDocument(EditorController* controller,
                                   CadDocument* document);

CadCommandState EditorController_GetCommandState(
    const EditorController* controller, CadCommandId command,
    const EditorCommandContext* context);
CadCommandState EditorController_GetToolState(
    const EditorController* controller, CadToolId tool,
    const EditorCommandContext* context);

CadResult EditorController_BeginEdit(EditorController* controller,
                                     CadToolId tool,
                                     const char* historyLabel);
/* Menu, keyboard, timeline, importer, and other non-palette mutations share
   the same validated transaction boundary without inventing a tool ID. */
CadResult EditorController_BeginCommandEdit(EditorController* controller,
                                            const char* historyLabel);
CadResult EditorController_UpdateEdit(EditorController* controller);
CadResult EditorController_CommitEdit(EditorController* controller);
void EditorController_CancelEdit(EditorController* controller);
int EditorController_IsEditing(const EditorController* controller);

#ifdef __cplusplus
}
#endif
