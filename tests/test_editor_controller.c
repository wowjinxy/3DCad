#include "editor_controller.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", \
                __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

int main(void) {
    CadDocument document;
    EditorController controller;
    EditorCommandContext context;
    CadCommandState state;
    CadResult result;

    memset(&context, 0, sizeof(context));
    context.activeTool = CAD_TOOL_POINT_SELECT;
    CadDocument_Init(&document);
    EditorController_Init(&controller, &document);

    state = EditorController_GetCommandState(
        &controller, CAD_COMMAND_EDIT_UNDO, &context);
    CHECK(!state.enabled);
    CHECK(strstr(state.disabledReason, "Nothing") != NULL);
    CHECK(strcmp(state.shortcut, "Ctrl+Z") == 0);
    state = EditorController_GetCommandState(
        &controller, CAD_COMMAND_FILE_SAVE_AS, &context);
    CHECK(state.enabled);
    CHECK(strcmp(state.shortcut, "Ctrl+Shift+S") == 0);
    state = EditorController_GetCommandState(
        &controller, CAD_COMMAND_OPTION_SELECT_ALL, &context);
    CHECK(state.enabled);
    CHECK(strcmp(state.shortcut, "Ctrl+A") == 0);
    state = EditorController_GetCommandState(
        &controller, CAD_COMMAND_OPTION_DESELECT_ALL, &context);
    CHECK(!state.enabled);
    CHECK(strstr(state.disabledReason, "Nothing") != NULL);
    CHECK(strcmp(state.shortcut, "Ctrl+Shift+A") == 0);
    context.selectedPointCount = 1;
    state = EditorController_GetCommandState(
        &controller, CAD_COMMAND_OPTION_DESELECT_ALL, &context);
    CHECK(state.enabled);
    context.selectedPointCount = 0;

    state = EditorController_GetToolState(
        &controller, CAD_TOOL_PRIMITIVE, &context);
    CHECK(!state.enabled);
    CHECK(strstr(state.disabledReason, "inactive") != NULL);
    state = EditorController_GetToolState(
        &controller, CAD_TOOL_POINT_SELECT, &context);
    CHECK(state.enabled && state.checked);
    state = EditorController_GetCommandState(
        &controller, (CadCommandId)9999, &context);
    CHECK(!state.enabled);
    CHECK(strstr(state.disabledReason, "Unknown") != NULL);
    result = EditorController_UpdateEdit(NULL);
    CHECK(!CadResult_IsSuccess(&result));
    result = EditorController_CommitEdit(NULL);
    CHECK(!CadResult_IsSuccess(&result));

    context.viewVisible[1] = 1;
    context.wireframe3D = 1;
    context.animationPanelVisible = 1;
    state = EditorController_GetCommandState(
        &controller, CAD_COMMAND_WINDOW_3D, &context);
    CHECK(state.enabled && state.checked);
    state = EditorController_GetCommandState(
        &controller, CAD_COMMAND_OPTION_WIREFRAME, &context);
    CHECK(state.checked);
    state = EditorController_GetCommandState(
        &controller, CAD_COMMAND_FILE_ANIMATION, &context);
    CHECK(state.checked);
    context.palettePanelVisible = 1;
    state = EditorController_GetCommandState(
        &controller, CAD_COMMAND_WINDOW_PALETTE_EDITOR, &context);
    CHECK(state.checked);
    state = EditorController_GetCommandState(
        &controller, CAD_COMMAND_PALETTE_SAVE_COL_AS, &context);
    CHECK(!state.enabled);
    CHECK(strstr(state.disabledReason, "No COL") != NULL);
    result = CadDocument_NewPalette(&document, CAD_PALETTE_FORMAT_COL);
    CHECK(CadResult_IsSuccess(&result));
    state = EditorController_GetCommandState(
        &controller, CAD_COMMAND_PALETTE_SAVE_COL, &context);
    CHECK(state.enabled); /* Save routes an unnamed resource through Save As. */
    state = EditorController_GetCommandState(
        &controller, CAD_COMMAND_PALETTE_SAVE_COL_AS, &context);
    CHECK(state.enabled);
    state = EditorController_GetCommandState(
        &controller, CAD_COMMAND_PALETTE_APPLY_SELECTED, &context);
    CHECK(!state.enabled);

    result = EditorController_BeginEdit(
        &controller, CAD_TOOL_POINT_CREATE, "Create Point");
    CHECK(CadResult_IsSuccess(&result));
    CHECK(CadCore_AddPoint(&document.core, 1.0, 2.0, 3.0) >= 0);
    result = EditorController_UpdateEdit(&controller);
    CHECK(CadResult_IsSuccess(&result));
    result = EditorController_CommitEdit(&controller);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(strcmp(CadDocument_GetUndoLabel(&document), "Create Point") == 0);

    result = EditorController_BeginCommandEdit(&controller, "Timeline Edit");
    CHECK(CadResult_IsSuccess(&result));
    document.core.data.points[0].pointx = 9.0;
    result = EditorController_UpdateEdit(&controller);
    CHECK(CadResult_IsSuccess(&result));
    result = EditorController_CommitEdit(&controller);
    CHECK(CadResult_IsSuccess(&result));
    CHECK(strcmp(CadDocument_GetUndoLabel(&document), "Timeline Edit") == 0);

    document.core.data.animationIndices[0].flags = 1;
    state = EditorController_GetCommandState(
        &controller, CAD_COMMAND_PALETTE_SAVE_COL_AS, &context);
    CHECK(state.enabled); /* animation does not block palette resources */
    state = EditorController_GetToolState(
        &controller, CAD_TOOL_FACE_CREATE, &context);
    CHECK(!state.enabled);
    CHECK(strstr(state.disabledReason, "Fixed-topology") != NULL);
    state = EditorController_GetToolState(
        &controller, CAD_TOOL_POINT_MOVE, &context);
    CHECK(!state.enabled); /* selection requirement, not animation */
    result = EditorController_BeginEdit(
        &controller, (CadToolId)9999, "Unknown Tool");
    CHECK(!CadResult_IsSuccess(&result));
    CHECK(!EditorController_IsEditing(&controller));
    context.selectedPointCount = 1;
    context.selectedPolygonCount = 1;
    state = EditorController_GetCommandState(
        &controller, CAD_COMMAND_PALETTE_APPLY_SELECTED, &context);
    CHECK(state.enabled);
    state = EditorController_GetToolState(
        &controller, CAD_TOOL_POINT_MOVE, &context);
    CHECK(state.enabled);
    result = EditorController_BeginEdit(
        &controller, CAD_TOOL_FACE_CREATE, "Blocked Animated Face");
    CHECK(!CadResult_IsSuccess(&result));
    CHECK(!EditorController_IsEditing(&controller));

    {
        static const CadToolId blockedTools[] = {
            CAD_TOOL_POINT_CREATE, CAD_TOOL_FACE_CREATE,
            CAD_TOOL_FACE_INSERT_POINT, CAD_TOOL_POINT_DELETE,
            CAD_TOOL_FACE_DELETE, CAD_TOOL_MIRROR,
            CAD_TOOL_FACE_REVERSE, CAD_TOOL_FACE_COPY,
            CAD_TOOL_FACE_CUT, CAD_TOOL_FACE_SIDE
        };
        static const CadToolId editableTools[] = {
            CAD_TOOL_POINT_MOVE, CAD_TOOL_POINT_ROTATE,
            CAD_TOOL_POINT_SCALE, CAD_TOOL_POINT_FLIP,
            CAD_TOOL_FACE_MOVE, CAD_TOOL_FACE_ROTATE,
            CAD_TOOL_FACE_SCALE, CAD_TOOL_FACE_COLOR,
            CAD_TOOL_STATE
        };
        size_t i;
        for (i = 0; i < sizeof(blockedTools) / sizeof(blockedTools[0]); ++i) {
            state = EditorController_GetToolState(
                &controller, blockedTools[i], &context);
            CHECK(!state.enabled);
            CHECK(strstr(state.disabledReason, "Fixed-topology") != NULL);
        }
        for (i = 0; i < sizeof(editableTools) / sizeof(editableTools[0]); ++i) {
            state = EditorController_GetToolState(
                &controller, editableTools[i], &context);
            CHECK(state.enabled);
        }
    }

    {
        static const CadCommandId blockedCommands[] = {
            CAD_COMMAND_EDIT_PASTE,
            CAD_COMMAND_OPTION_CHANGE_FIRST_POINT,
            CAD_COMMAND_OPTION_FACE_SUPPORT,
            CAD_COMMAND_MERGE_GRID,
            CAD_COMMAND_MERGE_POINTS,
            CAD_COMMAND_MERGE_POLYGONS,
            CAD_COMMAND_MERGE_ALL,
            CAD_COMMAND_POLYGON_SORT
        };
        size_t i;
        context.clipboardHasData = 1;
        for (i = 0;
             i < sizeof(blockedCommands) / sizeof(blockedCommands[0]); ++i) {
            state = EditorController_GetCommandState(
                &controller, blockedCommands[i], &context);
            CHECK(!state.enabled);
            CHECK(strstr(state.disabledReason, "Fixed-topology") != NULL);
        }
        state = EditorController_GetCommandState(
            &controller, CAD_COMMAND_EDIT_COPY, &context);
        CHECK(state.enabled);
        state = EditorController_GetCommandState(
            &controller, CAD_COMMAND_OPTION_FACE_INFORMATION, &context);
        CHECK(state.enabled);
    }

    CadDocument_Destroy(&document);
    if (failures) {
        fprintf(stderr, "%d editor controller test(s) failed\n", failures);
        return 1;
    }
    puts("All editor controller tests passed.");
    return 0;
}
