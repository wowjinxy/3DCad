#include "editor_tool.h"

#include <stdio.h>
#include <string.h>

static CadResult editor_tool_error(CadStatus status, const char* message) {
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

void EditorTool_Init(EditorTool* tool, CadDocument* document) {
    if (!tool) return;
    memset(tool, 0, sizeof(*tool));
    tool->document = document;
    tool->id = CAD_TOOL_NONE;
    tool->phase = EDITOR_TOOL_IDLE;
}

void EditorTool_BindDocument(EditorTool* tool, CadDocument* document) {
    if (!tool) return;
    EditorTool_Cancel(tool);
    tool->document = document;
}

int EditorTool_IsActive(const EditorTool* tool) {
    return tool && tool->phase != EDITOR_TOOL_IDLE;
}

CadResult EditorTool_BeginNamed(EditorTool* tool, CadToolId id,
                                const char* historyLabel) {
    CadResult result;
    if (!tool || !tool->document)
        return editor_tool_error(CAD_STATUS_INVALID_ARGUMENT,
                                 "The editor tool is not bound to a document");
    if (EditorTool_IsActive(tool))
        return editor_tool_error(CAD_STATUS_INVALID_ARGUMENT,
                                 "The editor tool already has an active edit");
    result = CadDocument_BeginEditNamed(tool->document, historyLabel);
    if (!CadResult_IsSuccess(&result)) return result;
    tool->id = id;
    tool->phase = EDITOR_TOOL_ACTIVE;
    return result;
}

CadResult EditorTool_Begin(EditorTool* tool, CadToolId id) {
    return EditorTool_BeginNamed(tool, id, "Edit");
}

CadResult EditorTool_Update(EditorTool* tool) {
    if (!tool || !tool->document || !EditorTool_IsActive(tool) ||
        !tool->document->transactionBefore) {
        return editor_tool_error(CAD_STATUS_INVALID_ARGUMENT,
                                 "No editor tool edit is active");
    }
    tool->phase = EDITOR_TOOL_UPDATED;
    return CadResult_Ok(tool->document->sourceFormat);
}

CadResult EditorTool_Commit(EditorTool* tool) {
    CadResult result;
    if (!tool || !tool->document || !EditorTool_IsActive(tool))
        return editor_tool_error(CAD_STATUS_INVALID_ARGUMENT,
                                 "No editor tool edit is active");
    result = CadDocument_CommitEdit(tool->document);
    tool->id = CAD_TOOL_NONE;
    tool->phase = EDITOR_TOOL_IDLE;
    return result;
}

void EditorTool_Cancel(EditorTool* tool) {
    if (!tool) return;
    if (tool->document && tool->document->transactionBefore)
        CadDocument_CancelEdit(tool->document);
    tool->id = CAD_TOOL_NONE;
    tool->phase = EDITOR_TOOL_IDLE;
}
