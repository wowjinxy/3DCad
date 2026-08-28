#include "editor_controller.h"

#include <stdio.h>
#include <string.h>

static CadCommandState command_state_default(void) {
    CadCommandState state;
    memset(&state, 0, sizeof(state));
    state.enabled = 1;
    state.shortcut = "";
    return state;
}

static CadResult controller_error(CadStatus status, const char* message) {
    CadResult result = CadResult_Ok(CAD_FORMAT_AUTO);
    result.status = status;
    result.errorCount = 1;
    result.diagnosticCount = 1;
    result.diagnostics[0].severity = CAD_DIAGNOSTIC_ERROR;
    result.diagnostics[0].code = status;
    result.diagnostics[0].recordTag = -1;
    result.diagnostics[0].recordIndex = -1;
    if (message) {
        snprintf(result.diagnostics[0].message,
                 sizeof(result.diagnostics[0].message), "%s", message);
    }
    return result;
}

static void disable(CadCommandState* state, const char* reason) {
    if (!state) return;
    state->enabled = 0;
    if (reason) {
        snprintf(state->disabledReason, sizeof(state->disabledReason),
                 "%s", reason);
    }
}

static int command_changes_topology(CadCommandId command) {
    switch (command) {
    case CAD_COMMAND_EDIT_PASTE:
    case CAD_COMMAND_OPTION_CHANGE_FIRST_POINT:
    case CAD_COMMAND_OPTION_FACE_SUPPORT:
    case CAD_COMMAND_MERGE_GRID:
    case CAD_COMMAND_MERGE_POINTS:
    case CAD_COMMAND_MERGE_POLYGONS:
    case CAD_COMMAND_MERGE_ALL:
    case CAD_COMMAND_POLYGON_SORT:
        return 1;
    default:
        return 0;
    }
}

static int tool_changes_topology(CadToolId tool) {
    switch (tool) {
    case CAD_TOOL_POINT_CREATE:
    case CAD_TOOL_FACE_CREATE:
    case CAD_TOOL_FACE_INSERT_POINT:
    case CAD_TOOL_POINT_DELETE:
    case CAD_TOOL_FACE_DELETE:
    case CAD_TOOL_MIRROR:
    case CAD_TOOL_FACE_REVERSE:
    case CAD_TOOL_FACE_COPY:
    case CAD_TOOL_FACE_CUT:
    case CAD_TOOL_FACE_SIDE:
        return 1;
    default:
        return 0;
    }
}

static const char* command_shortcut(CadCommandId command) {
    switch (command) {
    case CAD_COMMAND_FILE_NEW: return "Ctrl+N";
    case CAD_COMMAND_FILE_OPEN: return "Ctrl+O";
    case CAD_COMMAND_FILE_SAVE: return "Ctrl+S";
    case CAD_COMMAND_FILE_SAVE_AS: return "Ctrl+Shift+S";
    case CAD_COMMAND_FILE_QUIT: return "Ctrl+Q";
    case CAD_COMMAND_EDIT_UNDO: return "Ctrl+Z";
    case CAD_COMMAND_EDIT_REDO: return "Ctrl+Y";
    case CAD_COMMAND_EDIT_COPY: return "Ctrl+C";
    case CAD_COMMAND_EDIT_PASTE: return "Ctrl+V";
    case CAD_COMMAND_OPTION_SELECT_ALL: return "Ctrl+A";
    case CAD_COMMAND_OPTION_DESELECT_ALL: return "Ctrl+Shift+A";
    case CAD_COMMAND_WINDOW_HOME: return "Home";
    default: return "";
    }
}

void EditorController_Init(EditorController* controller,
                           CadDocument* document) {
    if (!controller) return;
    memset(controller, 0, sizeof(*controller));
    controller->document = document;
    EditorTool_Init(&controller->tool, document);
}

void EditorController_BindDocument(EditorController* controller,
                                   CadDocument* document) {
    if (!controller) return;
    EditorTool_BindDocument(&controller->tool, document);
    controller->document = document;
}

CadCommandState EditorController_GetCommandState(
    const EditorController* controller, CadCommandId command,
    const EditorCommandContext* context) {
    CadCommandState state = command_state_default();
    const CadDocument* document = controller ? controller->document : NULL;
    state.shortcut = command_shortcut(command);
    if (command <= CAD_COMMAND_NONE ||
        command > CAD_COMMAND_OPTION_DESELECT_ALL) {
        disable(&state, "Unknown editor command");
        return state;
    }
    if (!document) {
        disable(&state, "No document is attached to the editor controller");
        return state;
    }
    if (command_changes_topology(command) && CadDocument_HasAnimation(document)) {
        disable(&state,
                "Fixed-topology animation is active; make a static copy before changing topology");
        return state;
    }
    switch (command) {
    case CAD_COMMAND_EDIT_UNDO:
        if (!CadDocument_CanUndo(document)) disable(&state, "Nothing to undo");
        break;
    case CAD_COMMAND_EDIT_REDO:
        if (!CadDocument_CanRedo(document)) disable(&state, "Nothing to redo");
        break;
    case CAD_COMMAND_EDIT_COPY:
        if (!context || (!context->selectedPointCount &&
                         !context->selectedPolygonCount))
            disable(&state, "Select points or faces to copy");
        break;
    case CAD_COMMAND_EDIT_PASTE:
        if (!context || !context->clipboardHasData)
            disable(&state, "The editor clipboard is empty");
        break;
    case CAD_COMMAND_OPTION_DESELECT_ALL:
        if (!context || (!context->selectedPointCount &&
                         !context->selectedPolygonCount))
            disable(&state, "Nothing is selected");
        break;
    case CAD_COMMAND_OPTION_CHANGE_FIRST_POINT:
    case CAD_COMMAND_OPTION_FACE_SUPPORT:
    case CAD_COMMAND_OPTION_FACE_INFORMATION:
        if (!context || !context->selectedPolygonCount)
            disable(&state, "Select one or more faces first");
        break;
    case CAD_COMMAND_WINDOW_TOP:
    case CAD_COMMAND_WINDOW_3D:
    case CAD_COMMAND_WINDOW_FRONT:
    case CAD_COMMAND_WINDOW_RIGHT:
        if (context) {
            int index = command == CAD_COMMAND_WINDOW_TOP ? 0 :
                        command == CAD_COMMAND_WINDOW_3D ? 1 :
                        command == CAD_COMMAND_WINDOW_FRONT ? 2 : 3;
            state.checked = context->viewVisible[index] != 0;
        }
        break;
    case CAD_COMMAND_WINDOW_COORDINATES:
        state.checked = context && context->coordinatesVisible;
        break;
    case CAD_COMMAND_WINDOW_TOOL_PALETTE:
        state.checked = context && context->toolPaletteVisible;
        break;
    case CAD_COMMAND_WINDOW_TEN_KEY:
        state.checked = context && context->statePanelVisible;
        break;
    case CAD_COMMAND_FILE_ANIMATION:
        state.checked = context && context->animationPanelVisible;
        break;
    case CAD_COMMAND_OPTION_WIREFRAME:
        state.checked = context && context->wireframe3D;
        break;
    case CAD_COMMAND_OPTION_SOLID:
        state.checked = !context || !context->wireframe3D;
        break;
    default:
        break;
    }
    return state;
}

CadCommandState EditorController_GetToolState(
    const EditorController* controller, CadToolId tool,
    const EditorCommandContext* context) {
    CadCommandState state = command_state_default();
    const CadDocument* document = controller ? controller->document : NULL;
    if (!document) {
        disable(&state, "No document is attached to the editor controller");
        return state;
    }
    if (tool < 0 || tool >= CAD_TOOL_COUNT) {
        disable(&state, "Unknown editor tool");
        return state;
    }
    state.checked = context && context->activeTool == tool;
    if (tool == CAD_TOOL_TRANSFER) {
        disable(&state, "SF2 Transfer/export is deferred");
        return state;
    }
    if (tool == CAD_TOOL_PRIMITIVE) {
        disable(&state, "The recovered Primitive tool was historically inactive");
        return state;
    }
    if (tool_changes_topology(tool) && CadDocument_HasAnimation(document)) {
        disable(&state,
                "Fixed-topology animation is active; make a static copy before changing topology");
        return state;
    }
    if (tool == CAD_TOOL_UNDO && !CadDocument_CanUndo(document))
        disable(&state, "Nothing to undo");
    if ((tool == CAD_TOOL_FACE_MOVE || tool == CAD_TOOL_FACE_ROTATE ||
         tool == CAD_TOOL_FACE_SCALE || tool == CAD_TOOL_FACE_COLOR ||
         tool == CAD_TOOL_FACE_DELETE || tool == CAD_TOOL_FACE_REVERSE ||
         tool == CAD_TOOL_FACE_COPY || tool == CAD_TOOL_FACE_CUT ||
         tool == CAD_TOOL_FACE_SIDE || tool == CAD_TOOL_MIRROR) &&
        (!context || !context->selectedPolygonCount))
        disable(&state, "Select one or more faces first");
    if ((tool == CAD_TOOL_POINT_MOVE || tool == CAD_TOOL_POINT_ROTATE ||
         tool == CAD_TOOL_POINT_SCALE || tool == CAD_TOOL_POINT_DELETE ||
         tool == CAD_TOOL_POINT_FLIP) &&
        (!context || !context->selectedPointCount))
        disable(&state, "Select one or more points first");
    return state;
}

CadResult EditorController_BeginEdit(EditorController* controller,
                                     CadToolId tool,
                                     const char* historyLabel) {
    EditorCommandContext context;
    CadCommandState state;
    if (!controller || !controller->document)
        return controller_error(CAD_STATUS_INVALID_ARGUMENT,
                                "The editor controller is not bound to a document");
    if (tool < 0 || tool >= CAD_TOOL_COUNT)
        return controller_error(CAD_STATUS_INVALID_ARGUMENT,
                                "Cannot begin an edit with an unknown tool");
    memset(&context, 0, sizeof(context));
    context.activeTool = tool;
    context.selectedPointCount =
        controller->document->core.selection.pointCount;
    context.selectedPolygonCount =
        controller->document->core.selection.polygonCount;
    state = EditorController_GetToolState(controller, tool, &context);
    if (!state.enabled)
        return controller_error(CAD_STATUS_INVALID_ARGUMENT,
                                state.disabledReason[0]
                                    ? state.disabledReason
                                    : "The editor tool is disabled");
    return EditorTool_BeginNamed(&controller->tool, tool, historyLabel);
}

CadResult EditorController_BeginCommandEdit(EditorController* controller,
                                            const char* historyLabel) {
    if (!controller || !controller->document)
        return controller_error(CAD_STATUS_INVALID_ARGUMENT,
                                "The editor controller is not bound to a document");
    return EditorTool_BeginNamed(&controller->tool, CAD_TOOL_NONE,
                                 historyLabel);
}

CadResult EditorController_UpdateEdit(EditorController* controller) {
    return EditorTool_Update(controller ? &controller->tool : NULL);
}

CadResult EditorController_CommitEdit(EditorController* controller) {
    return EditorTool_Commit(controller ? &controller->tool : NULL);
}

void EditorController_CancelEdit(EditorController* controller) {
    if (controller) EditorTool_Cancel(&controller->tool);
}

int EditorController_IsEditing(const EditorController* controller) {
    return controller && EditorTool_IsActive(&controller->tool);
}
