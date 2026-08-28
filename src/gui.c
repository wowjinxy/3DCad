#define _CRT_SECURE_NO_WARNINGS

#include "gui.h"
#include "render_gl.h"
#include "font_win32.h"
#include "cad_core.h"
#include "file_dialog.h"
#include "cad_view.h"
#include "animation_panel.h"
#include "desktop_layout.h"
#include "cad_export_obj.h"
#include "cad_export_3dg1.h"
#include "cad_import_3dg1.h"
#include "cad_import_asm.h"
#include "cad_import_obj.h"
#include "cad_document.h"
#include "cad_animation.h"
#include "cad_anm_codec.h"
#include "cad_geometry.h"
#include "editor_commands.h"
#include "editor_controller.h"
#include "editor_tool.h"
#include "platform_fs.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <SDL3/SDL_opengl.h>
#include <SDL3/SDL_timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif
#include <ctype.h>
#include <stdarg.h>

#define TOOL_COUNT CAD_TOOL_COUNT
#define GUI_HISTORY_LIMIT 64
#define GUI_PATH_CAPACITY 4096

typedef CadAnimationPanelRect Rect;

static int pt_in_rect(int px, int py, Rect r) {
    return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
}

/* Check if point is near a window edge (for resizing) */
/* Returns edge flags: 1=left, 2=right, 4=top, 8=bottom */
static int get_resize_edge(int px, int py, Rect r, int threshold) {
    int edge = 0;
    if (px >= r.x - threshold && px < r.x + threshold) edge |= 1; /* Left */
    if (px >= r.x + r.w - threshold && px < r.x + r.w + threshold) edge |= 2; /* Right */
    if (py >= r.y - threshold && py < r.y + threshold) edge |= 4; /* Top */
    if (py >= r.y + r.h - threshold && py < r.y + r.h + threshold) edge |= 8; /* Bottom */
    return edge;
}

typedef struct GuiWin {
    const char* title;
    Rect r;
    int draggable;
} GuiWin;

typedef enum GuiPointerOwner {
    GUI_POINTER_NONE = 0,
    GUI_POINTER_MENU,
    GUI_POINTER_PALETTE,
    GUI_POINTER_WINDOW,
    GUI_POINTER_VIEW,
    GUI_POINTER_AREA_SELECT,
    GUI_POINTER_TRANSFORM,
    GUI_POINTER_SHAPE_BROWSER,
    GUI_POINTER_SCROLLBAR,
    GUI_POINTER_ANIMATION
} GuiPointerOwner;

/* Auxiliary editor windows form one desktop layer above the modeling views.
   IDs are stable array indices; aux_z_order is stored back-to-front. */
typedef enum GuiAuxWindowId {
    GUI_AUX_TOOL_PALETTE = 0,
    GUI_AUX_COORDINATES,
    GUI_AUX_ANIMATION,
    GUI_AUX_SHAPE_BROWSER,
    GUI_AUX_COUNT
} GuiAuxWindowId;

struct GuiState {
    FontWin32* font;
    GuiCommand pending_command;

    /* CAD core */
    CadDocument document;
    CadCore* cad; /* Stable convenience alias for document.core. */
    EditorController controller;

    CadAnimationSession animation;
    CadAnimationInfo animation_info;
    CadScene scene;
    double animation_now;
    int scene_valid;
    int animation_info_valid;
    
    /* View states */
    CadView views[4];        /* One view state per view window */

    /* Layout windows (match screenshot-ish geometry) */
    GuiWin toolPalette;      /* 120x410 left */
    GuiWin view[4];          /* 4 view windows */
    GuiWin coordBox;         /* coordinates/info */
    GuiWin animationWindow;  /* Animation window */
    GuiWin stateWindow;      /* Numeric STATE / TenKey transform panel */

    /* Menu bar */
    const char* menus[5];
    int menu_count;
    int menu_open; /* index or -1 */
    int menu_hover_item; /* 0-based within open menu, -1 none */
    
    /* Submenu state */
    int submenu_open; /* 0=none, 5=import, 6=export (using menu item index) */
    int submenu_hover_item; /* 0-based within submenu, -1 none */
    Rect submenu_rect; /* Submenu position/size */

    /* Tool icons */
    RG_Texture* tool_icons[TOOL_COUNT];
    CadToolId selected_tool;

    /* Input ownership: a press is routed to exactly one UI layer until release. */
    GuiPointerOwner pointer_owner;
    int pointer_view;

    /* Responsive desktop state. */
    int layout_width;
    int layout_height;
    int auto_layout;
    int view_visible[4];
    int view_z_order[4]; /* back-to-front desktop stacking order */
    int aux_z_order[GUI_AUX_COUNT]; /* back-to-front auxiliary stack */
    int tool_palette_visible;
    int coordinates_visible;
    int animation_visible;
    int animation_docked;

    /* Numeric transform panel. */
    int state_visible;
    int state_active_field;
    int state_face_target;
    int state_replace_on_input;
    char state_values[15][32];

    /* User-facing feedback and title state. */
    char status_text[256];
    char title_text[384];

    /* Snapshot history and the editor-local clipboard. */
    CadCore* clipboard;
    int clipboard_has_data;
    
    /* Animation icons */
    RG_Texture* anim_icons[12]; /* Animation control icons */
    
    /* Shape browser window */
    GuiWin shapeBrowserWindow;
    char** shape_names;        /* Array of shape name strings */
    int shape_count;           /* Number of shapes found */
    int shape_selected;        /* Selected shape index, or -1 */
    int shape_scroll_offset;   /* Scroll offset for shape list */
    char shape_folder_path[GUI_PATH_CAPACITY];
    CadAsmTextSource* shape_asm_sources;
    size_t shape_asm_source_count;
    CadAsmTextSource* shape_constant_sources;
    size_t shape_constant_source_count;
    CadAsmCatalog* shape_catalog;
    CadCore* shape_preview;
    CadView shape_preview_view;
    int shape_preview_valid;
    int shape_search_active;
    char shape_search[128];

    /* Dragging */
    GuiWin* drag_win;
    int drag_off_x;
    int drag_off_y;
    
    /* Resizing */
    GuiWin* resize_win;
    int resize_edge; /* 0=none, 1=left, 2=right, 4=top, 8=bottom (can combine) */
    int resize_start_x;
    int resize_start_y;
    int resize_window_x;
    int resize_window_y;
    int resize_start_w;
    int resize_start_h;
    
    /* View interaction */
    int view_interacting; /* Index of view being interacted with, or -1 */
    int view_right_interacting; /* Index of view being right-click interacted with, or -1 */
    int view_middle_interacting;
    int scrollbar_view;
    int scrollbar_axis; /* 1 = horizontal, 2 = vertical */
    int scrollbar_drag_offset;
    int last_mouse_x;
    int last_mouse_y;
    
    /* Point move state */
    int point_move_active; /* generic transform drag */
    int point_move_view;
    int transform_history_pushed;

    /* Guided two-view point placement. */
    int point_pending;
    int point_pending_view;
    unsigned point_known_axes; /* bit 0=X, 1=Y, 2=Z */
    double point_pending_x;
    double point_pending_y;
    double point_pending_z;

    /* Rectangle selection. */
    int area_select_armed;
    int area_select_active;
    int area_select_view;
    int area_start_x;
    int area_start_y;
    int area_end_x;
    int area_end_y;
    
    /* View window scaling (individual scale per view) */
    float view_scale[4]; /* Scale factor for each view window (default 1.0) */
    
    /* Timeline interaction.  Copy is intentionally a two-step operation:
       arm it on the source frame, then click a destination in the strip. */
    int anim_scrubbing;
    int anim_copy_mode; /* 0 none, 1 complete pose, 2 selected points */
    int anim_copy_source;
};

static void raise_aux(GuiState* g, int aux_id);

static int MenuBarHeight(void) { return 20; }

/* Forward declarations */
static void scan_asm_folder_for_shapes(GuiState* g, const char* folder_path);
static int load_shape_from_asm(GuiState* g, CadCore* core,
                               const char* shape_name);
static void free_asm_browser_sources(GuiState* g);

/* -------------------------------------------------------------------------
   Menu definitions (ported from 3DCad/include/MenuRes.h)
   ------------------------------------------------------------------------- */
static const CadMenuItemDescriptor fileMenuItems[] = {
    { CAD_COMMAND_FILE_NEW, "(N)New", 0 },
    { CAD_COMMAND_FILE_OPEN, "(O)Open...", 0 },
    { CAD_COMMAND_FILE_SAVE, "(S)Save", 0 },
    { CAD_COMMAND_FILE_SAVE_AS, "Save As...", 0 },
    { CAD_COMMAND_NONE, "Import >", CAD_MENU_ITEM_SUBMENU },
    { CAD_COMMAND_NONE, "Export >", CAD_MENU_ITEM_SUBMENU },
    { CAD_COMMAND_NONE, "-", CAD_MENU_ITEM_SEPARATOR },
    { CAD_COMMAND_FILE_LOAD_COLOR, "Load Color...", 0 },
    { CAD_COMMAND_FILE_LOAD_PALETTE, "Load Palette...", 0 },
    { CAD_COMMAND_FILE_ANIMATION, "Animation data...", 0 },
    { CAD_COMMAND_FILE_OPEN_SHAPE_FOLDER, "Open Shape Folder...", 0 },
    { CAD_COMMAND_NONE, "-", CAD_MENU_ITEM_SEPARATOR },
    { CAD_COMMAND_FILE_QUIT, "(Q)Quit", 0 }
};

/* Import submenu */
static const CadMenuItemDescriptor importSubMenuItems[] = {
    { CAD_COMMAND_FILE_IMPORT_3DG1, ".3dg1 (Fundoshi)", 0 },
    { CAD_COMMAND_FILE_IMPORT_OBJ, ".obj (Wavefront)", 0 },
    { CAD_COMMAND_FILE_IMPORT_ANM, ".anm (3DAN / 3DGI)", 0 }
};

/* Export submenu */
static const CadMenuItemDescriptor exportSubMenuItems[] = {
    { CAD_COMMAND_FILE_EXPORT_3DG1, ".3dg1 (Fundoshi)", 0 },
    { CAD_COMMAND_FILE_EXPORT_OBJ, ".obj (Wavefront)", 0 },
    { CAD_COMMAND_FILE_EXPORT_ANM_3DAN, ".anm (3DAN)", 0 },
    { CAD_COMMAND_FILE_EXPORT_ANM_3DGI, ".anm (3DGI legacy header)", 0 }
};

static const CadMenuItemDescriptor editMenuItems[] = {
    { CAD_COMMAND_EDIT_UNDO, "(U)Undo", 0 },
    { CAD_COMMAND_EDIT_REDO, "(Y)Redo", 0 },
    { CAD_COMMAND_EDIT_PASTE, "Paste", 0 },
    { CAD_COMMAND_NONE, "-", CAD_MENU_ITEM_SEPARATOR },
    { CAD_COMMAND_EDIT_COPY, "Copy", 0 }
};

static const CadMenuItemDescriptor windowMenuItems[] = {
    { CAD_COMMAND_WINDOW_TOP, "Top", 0 },
    { CAD_COMMAND_WINDOW_FRONT, "Front", 0 },
    { CAD_COMMAND_WINDOW_RIGHT, "Right", 0 },
    { CAD_COMMAND_WINDOW_3D, "3D View", 0 },
    { CAD_COMMAND_NONE, "-", CAD_MENU_ITEM_SEPARATOR },
    { CAD_COMMAND_WINDOW_COORDINATES, "(C)Coordinates", 0 },
    { CAD_COMMAND_WINDOW_TOOL_PALETTE, "Tool palette", 0 },
    { CAD_COMMAND_WINDOW_TEN_KEY, "TenKey", 0 },
    { CAD_COMMAND_NONE, "-", CAD_MENU_ITEM_SEPARATOR },
    { CAD_COMMAND_WINDOW_CLEAN_UP, "Clean Up", 0 },
    { CAD_COMMAND_WINDOW_HOME, "Home", 0 },
    { CAD_COMMAND_NONE, "-", CAD_MENU_ITEM_SEPARATOR },
    { CAD_COMMAND_WINDOW_RESET_SCALES, "All Scales Reset", 0 }
};

static const CadMenuItemDescriptor optionMenuItems[] = {
    { CAD_COMMAND_OPTION_AREA_SELECT, "Area Select", 0 },
    { CAD_COMMAND_OPTION_SELECT_ALL, "Select All (Ctrl+A)", 0 },
    { CAD_COMMAND_OPTION_DESELECT_ALL, "Deselect All (Ctrl+Shift+A)", 0 },
    { CAD_COMMAND_OPTION_CHANGE_FIRST_POINT, "Change First Point", 0 },
    { CAD_COMMAND_OPTION_FLAT_CHECK, "Flat Check", 0 },
    { CAD_COMMAND_OPTION_FACE_SUPPORT, "F.Support", 0 },
    { CAD_COMMAND_OPTION_FACE_INFORMATION, "F.Information", 0 },
    { CAD_COMMAND_NONE, "-", CAD_MENU_ITEM_SEPARATOR },
    { CAD_COMMAND_OPTION_WIREFRAME, "Wire Frame", 0 },
    { CAD_COMMAND_OPTION_SOLID, "Solid", 0 }
};

static const CadMenuItemDescriptor mergeMenuItems[] = {
    { CAD_COMMAND_MERGE_STATUS, "Merge Status", 0 },
    { CAD_COMMAND_MERGE_GRID, "Grid Merge", 0 },
    { CAD_COMMAND_MERGE_POINTS, "Point Merge", 0 },
    { CAD_COMMAND_MERGE_POLYGONS, "Polygon Merge", 0 },
    { CAD_COMMAND_MERGE_ALL, "All Merge", 0 },
    { CAD_COMMAND_NONE, "-", CAD_MENU_ITEM_SEPARATOR },
    { CAD_COMMAND_POLYGON_SORT, "Polygon Sort", 0 }
};

typedef struct MenuDescriptor {
    const char* title;
    const CadMenuItemDescriptor* items;
    int count;
} MenuDescriptor;

#define ARRAY_COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static const MenuDescriptor menuDescriptors[] = {
    { "File", fileMenuItems, ARRAY_COUNT(fileMenuItems) },
    { "Edit", editMenuItems, ARRAY_COUNT(editMenuItems) },
    { "Windows", windowMenuItems, ARRAY_COUNT(windowMenuItems) },
    { "Options", optionMenuItems, ARRAY_COUNT(optionMenuItems) },
    { "Merge", mergeMenuItems, ARRAY_COUNT(mergeMenuItems) }
};

static const CadToolDescriptor toolDescriptors[CAD_TOOL_COUNT] = {
    { CAD_TOOL_POINT_SELECT, "Point Select", "Click points; click again to deselect", CAD_TOOL_FLAG_POINT },
    { CAD_TOOL_FACE_SELECT, "Face Select", "Click a face or line to toggle it", CAD_TOOL_FLAG_FACE },
    { CAD_TOOL_POINT_CREATE, "Point", "Define a point from two orthographic views", CAD_TOOL_FLAG_POINT },
    { CAD_TOOL_FACE_CREATE, "Make Face", "Choose ordered points; right-click the final point", CAD_TOOL_FLAG_FACE },
    { CAD_TOOL_FACE_INSERT_POINT, "Insert Point", "Click an edge to insert its midpoint", CAD_TOOL_FLAG_FACE },
    { CAD_TOOL_FACE_COLOR, "Color", "Click faces to advance their indexed color", CAD_TOOL_FLAG_FACE },
    { CAD_TOOL_POINT_MOVE, "Move Points", "Drag selected points", CAD_TOOL_FLAG_POINT },
    { CAD_TOOL_FACE_MOVE, "Move Faces", "Drag selected faces", CAD_TOOL_FLAG_FACE },
    { CAD_TOOL_POINT_ROTATE, "Rotate Points", "Drag selected points around their center", CAD_TOOL_FLAG_POINT },
    { CAD_TOOL_FACE_ROTATE, "Rotate Faces", "Drag selected faces around their center", CAD_TOOL_FLAG_FACE },
    { CAD_TOOL_POINT_SCALE, "Scale Points", "Drag selected points around their center", CAD_TOOL_FLAG_POINT },
    { CAD_TOOL_FACE_SCALE, "Scale Faces", "Drag selected faces around their center", CAD_TOOL_FLAG_FACE },
    { CAD_TOOL_POINT_DELETE, "Delete Points", "Delete selected points safely", CAD_TOOL_FLAG_POINT | CAD_TOOL_FLAG_IMMEDIATE },
    { CAD_TOOL_FACE_DELETE, "Delete Faces", "Delete selected faces", CAD_TOOL_FLAG_FACE | CAD_TOOL_FLAG_IMMEDIATE },
    { CAD_TOOL_POINT_FLIP, "Flip Points", "Reflect selected points across their X center", CAD_TOOL_FLAG_POINT | CAD_TOOL_FLAG_IMMEDIATE },
    { CAD_TOOL_MIRROR, "Mirror Faces", "Create an X-mirrored copy of selected faces", CAD_TOOL_FLAG_FACE | CAD_TOOL_FLAG_IMMEDIATE },
    { CAD_TOOL_FACE_REVERSE, "Reverse Face", "Reverse selected face winding", CAD_TOOL_FLAG_FACE | CAD_TOOL_FLAG_IMMEDIATE },
    { CAD_TOOL_FACE_COPY, "Copy Faces", "Duplicate selected faces", CAD_TOOL_FLAG_FACE | CAD_TOOL_FLAG_IMMEDIATE },
    { CAD_TOOL_FACE_CUT, "Cut Faces", "Triangulate selected polygon faces", CAD_TOOL_FLAG_FACE | CAD_TOOL_FLAG_IMMEDIATE },
    { CAD_TOOL_FACE_SIDE, "Face Side", "Create and pair reverse sides", CAD_TOOL_FLAG_FACE | CAD_TOOL_FLAG_IMMEDIATE },
    { CAD_TOOL_STATE, "State", "Open numeric translate/rotate/scale controls", CAD_TOOL_FLAG_IMMEDIATE },
    { CAD_TOOL_TRANSFER, "Transfer", "SF2 transfer/export is deferred", CAD_TOOL_FLAG_DISABLED },
    { CAD_TOOL_PRIMITIVE, "Primitive", "The historical primitive tool was inactive", CAD_TOOL_FLAG_DISABLED },
    { CAD_TOOL_UNDO, "Undo", "Undo the last edit", CAD_TOOL_FLAG_IMMEDIATE }
};

/* Recovered X11 palette sequence.  Resource slots stay stable while the
   compact SDL palette reads in the original point/face workflow order. */
static const CadToolId toolPaletteOrder[CAD_TOOL_COUNT] = {
    CAD_TOOL_POINT_CREATE, CAD_TOOL_PRIMITIVE, CAD_TOOL_FACE_CREATE,
    CAD_TOOL_POINT_SELECT, CAD_TOOL_FACE_SELECT, CAD_TOOL_POINT_MOVE,
    CAD_TOOL_FACE_MOVE, CAD_TOOL_POINT_FLIP, CAD_TOOL_MIRROR,
    CAD_TOOL_POINT_ROTATE, CAD_TOOL_FACE_ROTATE, CAD_TOOL_POINT_SCALE,
    CAD_TOOL_FACE_SCALE, CAD_TOOL_FACE_COPY, CAD_TOOL_FACE_REVERSE,
    CAD_TOOL_FACE_SIDE, CAD_TOOL_FACE_INSERT_POINT, CAD_TOOL_FACE_CUT,
    CAD_TOOL_FACE_COLOR, CAD_TOOL_TRANSFER, CAD_TOOL_STATE,
    CAD_TOOL_POINT_DELETE, CAD_TOOL_FACE_DELETE, CAD_TOOL_UNDO
};

static EditorCommandContext editor_command_context(const GuiState* g) {
    EditorCommandContext context;
    memset(&context, 0, sizeof(context));
    if (!g || !g->cad) return context;
    context.selectedPointCount = g->cad->selection.pointCount;
    context.selectedPolygonCount = g->cad->selection.polygonCount;
    context.clipboardHasData = g->clipboard_has_data;
    for (int i = 0; i < 4; ++i) context.viewVisible[i] = g->view_visible[i];
    context.coordinatesVisible = g->coordinates_visible;
    context.toolPaletteVisible = g->tool_palette_visible;
    context.animationPanelVisible = g->animation_visible;
    context.statePanelVisible = g->state_visible;
    context.wireframe3D = g->views[1].wireframe;
    context.activeTool = g->selected_tool;
    return context;
}

static const MenuDescriptor* menu_for_index(int idx) {
    switch (idx) {
    case 0: case 1: case 2: case 3: case 4: return &menuDescriptors[idx];
    default: return NULL;
    }
}

/* Normalize legacy strings:
   - "-" is a separator
   - Leading spaces are padding
   - "(X)Text" format: keep as-is (shortcut visible)
   - Old "NNew" format: strip the first letter (backwards compatibility) */
static const char* menu_display_text(const char* s) {
    if (!s) return "";
    if (s[0] == '-' && s[1] == '\0') return "-";
    while (*s == ' ') s++;
    /* Keep "(X)Text" format as-is - shortcuts should be visible */
    /* Handle old "NNew" double-letter format (backwards compatibility) */
    if (s[0] && s[1] && isupper((unsigned char)s[0]) && s[1] == s[0]) {
        return s + 1;
    }
    return s;
}

static const char* menu_item_display_text(const GuiState* g,
                                          const CadMenuItemDescriptor* item,
                                          char* buffer, size_t capacity) {
    const char* history_label = NULL;
    if (!item) return "";
    if (g && item->command == CAD_COMMAND_EDIT_UNDO)
        history_label = CadDocument_GetUndoLabel(&g->document);
    else if (g && item->command == CAD_COMMAND_EDIT_REDO)
        history_label = CadDocument_GetRedoLabel(&g->document);
    if (history_label && history_label[0] && buffer && capacity) {
        snprintf(buffer, capacity, "%s%s %s",
                 item->command == CAD_COMMAND_EDIT_UNDO ? "(U)" : "(Y)",
                 item->command == CAD_COMMAND_EDIT_UNDO ? "Undo" : "Redo",
                 history_label);
        return buffer;
    }
    return menu_display_text(item->label);
}

/* -------------------------------------------------------------------------
   Menu action handlers
   ------------------------------------------------------------------------- */
static void gui_set_status(GuiState* g, const char* format, ...) {
    if (!g || !format) return;
    va_list args;
    va_start(args, format);
    vsnprintf(g->status_text, sizeof(g->status_text), format, args);
    va_end(args);
    fprintf(stdout, "%s\n", g->status_text);
}

static void apply_document_palette(GuiState* g) {
    if (!g) return;
    if (!g->document.paletteValid) {
        for (int i = 0; i < 4; ++i) CadView_ClearPalette(&g->views[i]);
        return;
    }
    uint8_t rgba[256][4];
    for (int i = 0; i < 256; ++i) {
        rgba[i][0] = g->document.palette[i].r;
        rgba[i][1] = g->document.palette[i].g;
        rgba[i][2] = g->document.palette[i].b;
        rgba[i][3] = g->document.palette[i].a;
    }
    for (int i = 0; i < 4; ++i) CadView_SetPalette(&g->views[i], &rgba[0][0]);
}

static const char* cad_result_message(const CadResult* result) {
    if (!result) return "unknown error";
    CadDiagnosticSeverity preferred = CadResult_IsSuccess(result)
                                          ? CAD_DIAGNOSTIC_WARNING
                                          : CAD_DIAGNOSTIC_ERROR;
    for (int i = 0; i < result->diagnosticCount; ++i) {
        if (result->diagnostics[i].severity == preferred &&
            result->diagnostics[i].message[0])
            return result->diagnostics[i].message;
    }
    for (int i = 0; i < result->diagnosticCount; ++i) {
        if (result->diagnostics[i].message[0])
            return result->diagnostics[i].message;
    }
    return CadStatus_Name(result->status);
}

static void format_warning_diagnostics(const CadResult* result,
                                       char* output, size_t output_size) {
    size_t used = 0;
    unsigned shown = 0;
    if (!output || output_size == 0) return;
    output[0] = '\0';
    if (!result) return;
    for (size_t i = 0; i < result->diagnosticCount && shown < 3; ++i) {
        const CadDiagnostic* diagnostic = &result->diagnostics[i];
        int written;
        if (diagnostic->severity != CAD_DIAGNOSTIC_WARNING ||
            !diagnostic->message[0]) continue;
        written = snprintf(output + used, output_size - used, "%s%s",
                           shown ? "\n" : "", diagnostic->message);
        if (written < 0 || (size_t)written >= output_size - used) {
            output[output_size - 1] = '\0';
            return;
        }
        used += (size_t)written;
        ++shown;
    }
}

static void animation_invalidate(GuiState* g) {
    if (!g) return;
    g->animation.cacheValid = 0;
    g->animation_info_valid = 0;
    g->scene_valid = 0;
    g->anim_scrubbing = 0;
    g->anim_copy_mode = 0;
}

/* Build one immutable displayed pose per GUI iteration.  CadScene keeps the
   stable document topology and the evaluated coordinates separate, so every
   view, picker, overlay, and coordinate read observes the exact same pose. */
static void animation_update_scene(GuiState* g, double now_seconds) {
    CadResult result;
    if (!g || !g->cad) return;
    g->animation_now = now_seconds;
    if (!g->animation_info_valid) {
        result = CadAnimation_Inspect(&g->cad->data, &g->animation_info);
        if (!CadResult_IsSuccess(&result)) {
            if (g->scene_valid)
                gui_set_status(g, "Animation preview unavailable: %s",
                               cad_result_message(&result));
            g->scene_valid = 0;
            return;
        }
        g->animation_info_valid = 1;
    }
    result = CadAnimationSession_Evaluate(&g->animation, &g->cad->data,
                                          now_seconds, &g->scene);
    if (!CadResult_IsSuccess(&result)) {
        if (g->scene_valid)
            gui_set_status(g, "Animation preview unavailable: %s",
                           cad_result_message(&result));
        g->scene_valid = 0;
        return;
    }
    g->scene_valid = 1;
}

static int gui_scene_point(const GuiState* g, int16_t point_index,
                           CadPosition* position) {
    const CadPoint* point;
    if (!g || !g->cad || !position || point_index < 0 ||
        point_index >= g->cad->data.pointCount) return 0;
    point = &g->cad->data.points[point_index];
    if (!point->flags) return 0;
    if (g->scene_valid && CadScene_GetPoint(&g->scene, point_index, position))
        return isfinite(position->x) && isfinite(position->y) &&
               isfinite(position->z);
    position->x = point->pointx;
    position->y = point->pointy;
    position->z = point->pointz;
    return isfinite(position->x) && isfinite(position->y) &&
           isfinite(position->z);
}

static int16_t gui_find_nearest_point(const GuiState* g, int view_index,
                                      int screen_x, int screen_y,
                                      Rect viewport, int threshold_pixels) {
    if (!g || !g->cad || view_index < 0 || view_index >= 4) return INVALID_INDEX;
    if (g->scene_valid)
        return CadView_FindNearestScenePoint(&g->views[view_index], &g->scene,
                                             screen_x, screen_y,
                                             viewport.x, viewport.y,
                                             viewport.w, viewport.h,
                                             threshold_pixels);
    return CadView_FindNearestPoint(&g->views[view_index], g->cad,
                                    screen_x, screen_y,
                                    viewport.x, viewport.y,
                                    viewport.w, viewport.h,
                                    threshold_pixels);
}

static int gui_find_points_at_location(const GuiState* g, int view_index,
                                       int screen_x, int screen_y,
                                       Rect viewport, int threshold_pixels,
                                       double world_threshold,
                                       int16_t* indices, int capacity) {
    if (!g || !g->cad || view_index < 0 || view_index >= 4) return 0;
    if (g->scene_valid)
        return CadView_FindScenePointsAtLocation(
            &g->views[view_index], &g->scene, screen_x, screen_y,
            viewport.x, viewport.y, viewport.w, viewport.h,
            threshold_pixels, world_threshold, indices, capacity);
    return CadView_FindPointsAtLocation(
        &g->views[view_index], g->cad, screen_x, screen_y,
        viewport.x, viewport.y, viewport.w, viewport.h,
        threshold_pixels, world_threshold, indices, capacity);
}

static int16_t gui_find_nearest_polygon(const GuiState* g, int view_index,
                                        int screen_x, int screen_y,
                                        Rect viewport, int threshold_pixels) {
    if (!g || !g->cad || view_index < 0 || view_index >= 4) return INVALID_INDEX;
    if (g->scene_valid)
        return CadView_FindNearestScenePolygon(
            &g->views[view_index], &g->scene, screen_x, screen_y,
            viewport.x, viewport.y, viewport.w, viewport.h,
            threshold_pixels);
    return CadView_FindNearestPolygon(
        &g->views[view_index], g->cad, screen_x, screen_y,
        viewport.x, viewport.y, viewport.w, viewport.h,
        threshold_pixels);
}

static void reset_interaction(GuiState* g) {
    if (!g) return;
    int cancelled_edit = 0;
    /* A document edit is inseparable from the gesture that owns it.  Any
       workflow reset must roll that gesture back before clearing capture, so
       menus, file dialogs, and document replacement cannot strand a partial
       drag or a stale EditorTool phase. */
    if (EditorController_IsEditing(&g->controller) || g->document.transactionBefore) {
        EditorController_CancelEdit(&g->controller);
        g->cad = &g->document.core;
        cancelled_edit = 1;
    }
    g->drag_win = NULL;
    g->resize_win = NULL;
    g->resize_edge = 0;
    g->view_interacting = -1;
    g->view_right_interacting = -1;
    g->view_middle_interacting = -1;
    g->scrollbar_view = -1;
    g->scrollbar_axis = 0;
    g->scrollbar_drag_offset = 0;
    g->point_move_active = 0;
    g->point_move_view = -1;
    g->transform_history_pushed = 0;
    g->point_pending = 0;
    g->point_pending_view = -1;
    g->point_known_axes = 0;
    g->area_select_active = 0;
    g->area_select_armed = 0;
    g->pointer_owner = GUI_POINTER_NONE;
    g->pointer_view = -1;
    g->state_visible = 0;
    g->state_active_field = -1;
    g->state_replace_on_input = 1;
    if (cancelled_edit) animation_invalidate(g);
}

static void history_clear(GuiState* g) {
    if (!g) return;
    CadDocument_ClearHistory(&g->document);
}

static int history_begin(GuiState* g, CadToolId tool, int tool_edit,
                         const char* label) {
    if (!g || !g->cad) return 0;
    CadAnimationSession_BeginEdit(&g->animation, g->animation_now);
    if (EditorController_IsEditing(&g->controller) || g->document.transactionBefore)
        EditorController_CancelEdit(&g->controller);
    CadResult result = tool_edit
        ? EditorController_BeginEdit(&g->controller, tool,
                                     label && label[0] ? label : "Edit")
        : EditorController_BeginCommandEdit(&g->controller,
                                            label && label[0] ? label : "Edit");
    if (!CadResult_IsSuccess(&result)) {
        gui_set_status(g, "Edit cancelled: could not create an undo snapshot (%s)",
                       CadStatus_Name(result.status));
        return 0;
    }
    return 1;
}

static int history_push_named(GuiState* g, const char* label) {
    return history_begin(g, CAD_TOOL_NONE, 0, label);
}

static int history_push(GuiState* g) {
    const char* label = "Edit";
    int has_tool = g && g->selected_tool >= 0 &&
                   g->selected_tool < CAD_TOOL_COUNT;
    if (has_tool)
        label = toolDescriptors[g->selected_tool].name;
    return history_begin(g, has_tool ? g->selected_tool : CAD_TOOL_NONE,
                         has_tool, label);
}

static int history_commit(GuiState* g) {
    if (!g || !g->document.transactionBefore) return 0;
    CadResult result = EditorController_UpdateEdit(&g->controller);
    if (CadResult_IsSuccess(&result)) result = EditorController_CommitEdit(&g->controller);
    else EditorController_CancelEdit(&g->controller);
    g->cad = &g->document.core;
    if (!CadResult_IsSuccess(&result)) {
        gui_set_status(g, "Edit rolled back: could not commit its undo snapshot (%s)",
                       CadStatus_Name(result.status));
        return 0;
    }
    animation_invalidate(g);
    return 1;
}

static void history_cancel(GuiState* g) {
    if (!g) return;
    if (EditorController_IsEditing(&g->controller) || g->document.transactionBefore)
        EditorController_CancelEdit(&g->controller);
    g->cad = &g->document.core;
    animation_invalidate(g);
}

static int history_undo(GuiState* g) {
    if (!g || !CadDocument_CanUndo(&g->document)) {
        gui_set_status(g, "Nothing to undo");
        return 0;
    }
    char label[CAD_DOCUMENT_HISTORY_LABEL_CAPACITY];
    snprintf(label, sizeof(label), "%s",
             CadDocument_GetUndoLabel(&g->document));
    CadDocument_Undo(&g->document);
    g->cad = &g->document.core;
    animation_invalidate(g);
    apply_document_palette(g);
    reset_interaction(g);
    gui_set_status(g, "Undo %s", label);
    return 1;
}

static int history_redo(GuiState* g) {
    if (!g || !CadDocument_CanRedo(&g->document)) {
        gui_set_status(g, "Nothing to redo");
        return 0;
    }
    char label[CAD_DOCUMENT_HISTORY_LABEL_CAPACITY];
    snprintf(label, sizeof(label), "%s",
             CadDocument_GetRedoLabel(&g->document));
    CadDocument_Redo(&g->document);
    g->cad = &g->document.core;
    animation_invalidate(g);
    apply_document_palette(g);
    reset_interaction(g);
    gui_set_status(g, "Redo %s", label);
    return 1;
}

static int save_document_as(GuiState* g) {
    char filename[GUI_PATH_CAPACITY];
    if (!g || !g->cad || !FileDialog_SaveCAD(filename, sizeof(filename))) return 0;
    CadResult result = CadDocument_Save(&g->document, filename);
    if (!CadResult_IsSuccess(&result)) {
        gui_set_status(g, "Could not save %s: %s", filename,
                       cad_result_message(&result));
        return 0;
    }
    g->cad = &g->document.core;
    gui_set_status(g, "Saved %s", filename);
    return 1;
}

static int save_document(GuiState* g) {
    if (!g || !g->cad) return 0;
    if (!g->document.savePath) return save_document_as(g);
    CadResult result = CadDocument_SaveCurrent(&g->document);
    if (!CadResult_IsSuccess(&result)) {
        gui_set_status(g, "Could not save %s: %s", g->document.savePath,
                       cad_result_message(&result));
        return 0;
    }
    g->cad = &g->document.core;
    gui_set_status(g, "Saved %s", g->document.savePath);
    return 1;
}

static int confirm_replace_document(GuiState* g, const char* action) {
    if (!g || !g->cad || !g->document.isDirty) return 1;
#ifdef _WIN32
    char prompt[512];
    snprintf(prompt, sizeof(prompt),
             "Save changes before %s?\n\nYes: save\nNo: discard\nCancel: keep editing",
             action ? action : "continuing");
    int answer = MessageBoxA(GetActiveWindow(), prompt, "3DCad - Unsaved changes",
                             MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON1);
    if (answer == IDCANCEL || answer == 0) return 0;
    if (answer == IDYES) return save_document(g);
    return answer == IDNO;
#else
    gui_set_status(g, "Unsaved changes: save before %s", action ? action : "continuing");
    return 0;
#endif
}

static void replace_document(GuiState* g, const CadCore* replacement,
                             const char* native_filename, int imported) {
    if (!g || !g->cad || !replacement) return;
    CadDocument_New(&g->document);
    EditorController_BindDocument(&g->controller, &g->document);
    g->document.core = *replacement;
    g->cad = &g->document.core;
    animation_invalidate(g);
    if (native_filename) {
        /* Native files are normally installed through CadDocument_Load. */
        g->document.sourceFormat = CAD_FORMAT_X11_STREAM;
    } else if (imported) {
        g->document.sourceFormat = CAD_FORMAT_AUTO;
        CadDocument_MarkDirty(&g->document);
    }
    history_clear(g);
    reset_interaction(g);
    for (int i = 0; i < 4; ++i) CadView_Reset(&g->views[i]);
    apply_document_palette(g);
}

/* Implemented below with the geometry helpers. */
static void editor_copy(GuiState* g);
static void editor_paste(GuiState* g);
static void editor_change_first_point(GuiState* g);
static void editor_flat_check(GuiState* g);
static void editor_face_support(GuiState* g);
static void editor_face_information(GuiState* g);
static void editor_grid_merge(GuiState* g);
static void editor_point_merge(GuiState* g);
static void editor_polygon_merge(GuiState* g);
static void editor_merge_all(GuiState* g);
static void editor_polygon_sort(GuiState* g);
static void layout_cleanup(GuiState* g, int win_w, int win_h);
static int frame_document_views(GuiState* g, int selection_only);
static int create_face_from_selection(GuiState* g);
static void update_face_creation_status(GuiState* g, int view_index);
static void activate_tool(GuiState* g, CadToolId tool);
static void animation_invalidate(GuiState* g);
static void animation_update_scene(GuiState* g, double now_seconds);
static void state_panel_open(GuiState* g);
static void state_panel_close(GuiState* g);
static int state_panel_apply(GuiState* g);
static void state_panel_key(GuiState* g, int key, unsigned modifiers);
static void gui_draw_interaction_overlays(GuiState* g, int view_index);

#ifdef _WIN32
static int gui_utf8_to_wide(const char* source, wchar_t* target, int capacity) {
    if (!source || !target || capacity <= 0) return 0;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source, -1,
                               target, capacity) > 0;
}

static int gui_wide_to_utf8(const wchar_t* source, char* target, int capacity) {
    if (!source || !target || capacity <= 0) return 0;
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, source, -1,
                               target, capacity, NULL, NULL) > 0;
}
#endif

static void execute_editor_command(GuiState* g, CadCommandId command) {
    char filename[GUI_PATH_CAPACITY];
    if (!g || !g->cad) return;

    if (EditorController_IsEditing(&g->controller) || g->document.transactionBefore)
        reset_interaction(g);

    switch (command) {
    case CAD_COMMAND_FILE_NEW:
        if (confirm_replace_document(g, "creating a new document")) {
            CadDocument_New(&g->document);
            EditorController_BindDocument(&g->controller, &g->document);
            g->cad = &g->document.core;
            animation_invalidate(g);
            reset_interaction(g);
            for (int i = 0; i < 4; ++i) CadView_Reset(&g->views[i]);
            gui_set_status(g, "New document");
        }
        break;
    case CAD_COMMAND_FILE_OPEN:
        if (FileDialog_OpenCAD(filename, sizeof(filename))) {
            CadDocument* temp = (CadDocument*)malloc(sizeof(*temp));
            if (!temp) {
                gui_set_status(g, "Not enough memory to open a document");
                break;
            }
            CadDocument_Init(temp);
            CadResult result = CadDocument_Load(temp, filename);
            if (!CadResult_IsSuccess(&result)) {
                gui_set_status(g, "Could not open %s: %s; current document unchanged",
                               filename, cad_result_message(&result));
            } else if (confirm_replace_document(g, "opening another document")) {
                CadDocument_Destroy(&g->document);
                g->document = *temp;
                memset(temp, 0, sizeof(*temp));
                EditorController_BindDocument(&g->controller, &g->document);
                g->cad = &g->document.core;
                animation_invalidate(g);
                reset_interaction(g);
                for (int i = 0; i < 4; ++i) CadView_Reset(&g->views[i]);
                apply_document_palette(g);
                if (result.warningCount) {
                    gui_set_status(g, "%s %s with %u warning(s): %s",
                                   result.format == CAD_FORMAT_LEGACY_PACKED
                                       ? "Imported legacy CAD" : "Opened",
                                   filename, result.warningCount,
                                   cad_result_message(&result));
                } else {
                    gui_set_status(g, result.format == CAD_FORMAT_LEGACY_PACKED
                        ? "Imported legacy CAD %s; use Save As" : "Opened %s", filename);
                }
            }
            CadDocument_Destroy(temp);
            free(temp);
        }
        break;
    case CAD_COMMAND_FILE_SAVE: save_document(g); break;
    case CAD_COMMAND_FILE_SAVE_AS: save_document_as(g); break;
    case CAD_COMMAND_FILE_IMPORT_ANM:
        if (FileDialog_Open(filename, sizeof(filename),
                            "ANM Files\0*.anm\0All Files\0*.*\0",
                            "Import 3DAN / 3DGI Animation")) {
            CadDocument* temp = (CadDocument*)malloc(sizeof(*temp));
            if (!temp) {
                gui_set_status(g, "Not enough memory to import ANM");
                break;
            }
            CadDocument_Init(temp);
            CadResult result = CadDocument_ImportAnm(temp, filename);
            if (!CadResult_IsSuccess(&result)) {
                gui_set_status(g, "ANM import failed: %s; current document unchanged",
                               cad_result_message(&result));
            } else if (confirm_replace_document(g, "importing an animation")) {
                CadDocument_Destroy(&g->document);
                g->document = *temp;
                memset(temp, 0, sizeof(*temp));
                EditorController_BindDocument(&g->controller, &g->document);
                g->cad = &g->document.core;
                animation_invalidate(g);
                reset_interaction(g);
                for (int i = 0; i < 4; ++i) CadView_Reset(&g->views[i]);
                g->animation_visible = 1;
                g->animation_docked = 1;
                raise_aux(g, GUI_AUX_ANIMATION);
                g->auto_layout = 1;
                if (result.warningCount) {
                    gui_set_status(g,
                                   "Imported %s ANM; Save As required; warning: %s",
                                   result.format == CAD_FORMAT_ANM_3DGI ? "3DGI" : "3DAN",
                                   cad_result_message(&result));
                } else {
                    gui_set_status(g,
                                   "Imported %s ANM; native CAD Save As is required",
                                   result.format == CAD_FORMAT_ANM_3DGI ? "3DGI" : "3DAN");
                }
            }
            CadDocument_Destroy(temp);
            free(temp);
        }
        break;
    case CAD_COMMAND_FILE_IMPORT_3DG1:
    case CAD_COMMAND_FILE_IMPORT_OBJ: {
        const int is_obj = command == CAD_COMMAND_FILE_IMPORT_OBJ;
        if (FileDialog_Open(filename, sizeof(filename),
                            is_obj ? "OBJ Files\0*.obj\0All Files\0*.*\0"
                                   : "3DG1 Files\0*.3dg1\0All Files\0*.*\0",
                            is_obj ? "Import OBJ" : "Import 3DG1")) {
            CadCore* temp = (CadCore*)malloc(sizeof(*temp));
            if (!temp) {
                gui_set_status(g, "Not enough memory to import a model");
                break;
            }
            CadCore_Init(temp);
            int ok = is_obj ? CadImport_OBJ(temp, filename) : CadImport_3DG1(temp, filename);
            if (!ok) {
                gui_set_status(g, "Import failed; current document was not changed");
            } else if (confirm_replace_document(g, "importing another model")) {
                replace_document(g, temp, NULL, 1);
                CadDocument_SetLastImportPath(&g->document, filename);
                gui_set_status(g, "Imported %s; use Save As for native CAD", filename);
            }
            free(temp);
        }
        break;
    }
    case CAD_COMMAND_FILE_EXPORT_3DG1:
    case CAD_COMMAND_FILE_EXPORT_OBJ: {
        const int is_obj = command == CAD_COMMAND_FILE_EXPORT_OBJ;
        if (FileDialog_Save(filename, sizeof(filename),
                            is_obj ? "OBJ Files\0*.obj\0All Files\0*.*\0"
                                   : "3DG1 Files\0*.3dg1\0All Files\0*.*\0",
                            is_obj ? "Export OBJ" : "Export 3DG1")) {
            int ok;
            if (g->document.lastImportPath &&
                CadPlatform_PathsEqual(filename,
                                       g->document.lastImportPath)) {
                gui_set_status(
                    g, "Export cancelled: imported source files are never overwritten");
                break;
            }
            ok = is_obj ? CadExport_OBJ(g->cad, filename)
                        : CadExport_3DG1(g->cad, filename);
            if (ok) CadDocument_SetLastExportPath(&g->document, filename);
            gui_set_status(g, ok ? "Exported %s" : "Export failed: %s", filename);
        }
        break;
    }
    case CAD_COMMAND_FILE_EXPORT_ANM_3DAN:
    case CAD_COMMAND_FILE_EXPORT_ANM_3DGI: {
        CadFormat format = command == CAD_COMMAND_FILE_EXPORT_ANM_3DGI
                               ? CAD_FORMAT_ANM_3DGI : CAD_FORMAT_ANM_3DAN;
        CadResult validation = CadAnmCodec_Validate(&g->cad->data);
        if (!CadResult_IsSuccess(&validation)) {
            gui_set_status(g, "ANM export is unavailable: %s",
                           cad_result_message(&validation));
            break;
        }
        if (validation.warningCount) {
            char warning[512];
            char details[384];
            format_warning_diagnostics(&validation, details, sizeof(details));
            snprintf(warning, sizeof(warning),
                     "ANM export reported %u compatibility warning(s).\n\n%s\n\nContinue?",
                     validation.warningCount,
                     details[0] ? details : cad_result_message(&validation));
#ifdef _WIN32
            if (MessageBoxA(GetActiveWindow(), warning,
                            "3DCad - ANM export warning",
                            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
                gui_set_status(g, "ANM export cancelled after validation warnings");
                break;
            }
#else
            gui_set_status(g, "%s", warning);
#endif
        }
        if (FileDialog_Save(filename, sizeof(filename),
                            "ANM Files\0*.anm\0All Files\0*.*\0",
                            format == CAD_FORMAT_ANM_3DGI
                                ? "Export 3DGI Animation"
                                : "Export 3DAN Animation")) {
            CadResult result = CadDocument_ExportAnm(&g->document, filename,
                                                     format);
            if (!CadResult_IsSuccess(&result)) {
                gui_set_status(g, "ANM export failed: %s",
                               cad_result_message(&result));
            } else if (result.warningCount) {
                gui_set_status(g, "Exported %s with %u warning(s): %s",
                               filename, result.warningCount,
                               cad_result_message(&result));
            } else {
                gui_set_status(g, "Exported %s deterministically as %s",
                               filename,
                               format == CAD_FORMAT_ANM_3DGI ? "3DGI" : "3DAN");
            }
        }
        break;
    }
    case CAD_COMMAND_FILE_LOAD_COLOR:
    case CAD_COMMAND_FILE_LOAD_PALETTE: {
        int palette = command == CAD_COMMAND_FILE_LOAD_PALETTE;
        if (FileDialog_Open(filename, sizeof(filename),
                            palette ? "Palette Files\0*.pal\0All Files\0*.*\0"
                                    : "Color Files\0*.col\0All Files\0*.*\0",
                             palette ? "Load Palette" : "Load Color")) {
            uint8_t* bytes = NULL;
            size_t size = 0;
            size_t required = palette ? sizeof(g->document.paletteData)
                                      : sizeof(g->document.colorData);
            CadResult read_result = CadPlatform_ReadFile(
                filename, CAD_PALETTE_DATA_SIZE, &bytes, &size);
            if (!CadResult_IsSuccess(&read_result)) {
                gui_set_status(g, "Could not load %s: %s", filename,
                               cad_result_message(&read_result));
            } else if ((palette && size != required) ||
                       (!palette && size < required)) {
                gui_set_status(g, palette
                    ? "%s must be exactly 0x%zX bytes"
                    : "%s must contain at least 0x%zX bytes",
                    filename, required);
            } else {
                CadRgba entries[256];
                for (int i = 0; i < 256; ++i) {
                    unsigned word = (unsigned)bytes[i * 2] |
                                    ((unsigned)bytes[i * 2 + 1] << 8);
                    entries[i].r = (uint8_t)(((word >> 0) & 31u) * 255u / 31u);
                    entries[i].g = (uint8_t)(((word >> 5) & 31u) * 255u / 31u);
                    entries[i].b = (uint8_t)(((word >> 10) & 31u) * 255u / 31u);
                    entries[i].a = 255;
                }
                if (history_push_named(g, palette ? "Load Palette" : "Load Color")) {
                    if (palette) {
                        memcpy(g->document.paletteData, bytes, required);
                        g->document.paletteDataSize = required;
                    } else {
                        memcpy(g->document.colorData, bytes, required);
                        g->document.colorDataSize = required;
                    }
                    if (!CadDocument_SetPalette(&g->document, entries,
                                                filename)) {
                        history_cancel(g);
                        gui_set_status(g,
                                       "Could not retain the palette source path");
                    } else if (history_commit(g)) {
                        apply_document_palette(g);
                        if (!palette && size > required) {
                            gui_set_status(g,
                                "Loaded first %zu color bytes from %s; %zu trailing byte(s) ignored",
                                required, filename, size - required);
                        } else {
                            gui_set_status(g, "Loaded %zu bytes from %s",
                                           required, filename);
                        }
                    }
                }
            }
            CadPlatform_Free(bytes);
        }
        break;
    }
    case CAD_COMMAND_FILE_ANIMATION:
        g->animation_visible = !g->animation_visible;
        if (g->animation_visible && g->animationWindow.r.w <= 0)
            g->animationWindow.r = (Rect){ 180, 680, 900, 126 };
        if (g->animation_visible) raise_aux(g, GUI_AUX_ANIMATION);
        g->auto_layout = 1;
        gui_set_status(g, "Animation timeline %s",
                       g->animation_visible ? "shown" : "hidden");
        break;
    case CAD_COMMAND_FILE_OPEN_SHAPE_FOLDER:
        if (FileDialog_SelectFolder(filename, sizeof(filename))) scan_asm_folder_for_shapes(g, filename);
        break;
    case CAD_COMMAND_FILE_QUIT: gui_request_quit(g); break;
    case CAD_COMMAND_EDIT_UNDO: history_undo(g); break;
    case CAD_COMMAND_EDIT_REDO: history_redo(g); break;
    case CAD_COMMAND_EDIT_COPY: editor_copy(g); break;
    case CAD_COMMAND_EDIT_PASTE: editor_paste(g); break;
    case CAD_COMMAND_WINDOW_TOP: g->view_visible[0] = !g->view_visible[0]; g->auto_layout = 1; break;
    case CAD_COMMAND_WINDOW_3D: g->view_visible[1] = !g->view_visible[1]; g->auto_layout = 1; break;
    case CAD_COMMAND_WINDOW_FRONT: g->view_visible[2] = !g->view_visible[2]; g->auto_layout = 1; break;
    case CAD_COMMAND_WINDOW_RIGHT: g->view_visible[3] = !g->view_visible[3]; g->auto_layout = 1; break;
    case CAD_COMMAND_WINDOW_COORDINATES:
        g->coordinates_visible = !g->coordinates_visible;
        if (g->coordinates_visible) raise_aux(g, GUI_AUX_COORDINATES);
        g->auto_layout = 1;
        break;
    case CAD_COMMAND_WINDOW_TOOL_PALETTE:
        g->tool_palette_visible = !g->tool_palette_visible;
        if (g->tool_palette_visible) raise_aux(g, GUI_AUX_TOOL_PALETTE);
        g->auto_layout = 1;
        break;
    case CAD_COMMAND_WINDOW_TEN_KEY:
        if (g->state_visible) state_panel_close(g);
        else state_panel_open(g);
        break;
    case CAD_COMMAND_WINDOW_CLEAN_UP:
        g->auto_layout = 1;
        layout_cleanup(g, g->layout_width, g->layout_height);
        gui_set_status(g, "Windows cleaned up to fit the client area");
        break;
    case CAD_COMMAND_WINDOW_HOME:
        if (!frame_document_views(g, 0)) {
            for (int i = 0; i < 4; ++i) CadView_Reset(&g->views[i]);
        }
        apply_document_palette(g);
        gui_set_status(g, "Home: framed the document in all views");
        break;
    case CAD_COMMAND_WINDOW_RESET_SCALES:
        for (int i = 0; i < 4; ++i) {
            g->view_scale[i] = 1.0f;
            CadView_SetZoom(&g->views[i], 1.0);
        }
        g->auto_layout = 1;
        layout_cleanup(g, g->layout_width, g->layout_height);
        gui_set_status(g, "All view scales reset");
        break;
    case CAD_COMMAND_OPTION_AREA_SELECT:
        g->area_select_armed = 1;
        gui_set_status(g, "Drag a rectangle in an orthographic view; Escape cancels");
        break;
    case CAD_COMMAND_OPTION_SELECT_ALL: CadCore_SelectAll(g->cad); gui_set_status(g, "Selected all"); break;
    case CAD_COMMAND_OPTION_DESELECT_ALL:
        CadCore_ClearSelection(g->cad);
        gui_set_status(g, "Deselected all");
        break;
    case CAD_COMMAND_OPTION_CHANGE_FIRST_POINT: editor_change_first_point(g); break;
    case CAD_COMMAND_OPTION_FLAT_CHECK: editor_flat_check(g); break;
    case CAD_COMMAND_OPTION_FACE_SUPPORT: editor_face_support(g); break;
    case CAD_COMMAND_OPTION_FACE_INFORMATION: editor_face_information(g); break;
    case CAD_COMMAND_OPTION_WIREFRAME: g->views[1].wireframe = 1; gui_set_status(g, "3D view: wireframe"); break;
    case CAD_COMMAND_OPTION_SOLID: g->views[1].wireframe = 0; gui_set_status(g, "3D view: solid"); break;
    case CAD_COMMAND_MERGE_STATUS:
        gui_set_status(g, "Grid %s; duplicate points %s; fully merged %s",
                       CadCore_AreCoordinatesMerged(g->cad) ? "OK" : "needed",
                       CadCore_ArePointsMerged(g->cad) ? "none" : "found",
                       CadCore_IsFullyMerged(g->cad) ? "yes" : "no");
        break;
    case CAD_COMMAND_MERGE_GRID: editor_grid_merge(g); break;
    case CAD_COMMAND_MERGE_POINTS: editor_point_merge(g); break;
    case CAD_COMMAND_MERGE_POLYGONS: editor_polygon_merge(g); break;
    case CAD_COMMAND_MERGE_ALL: editor_merge_all(g); break;
    case CAD_COMMAND_POLYGON_SORT: editor_polygon_sort(g); break;
    default: break;
    }
}

GuiState* gui_create(void) {
    GuiState* g = (GuiState*)calloc(1, sizeof(GuiState));
    if (!g) return NULL;

    CadDocument_Init(&g->document);
    g->cad = &g->document.core;
    EditorController_Init(&g->controller, &g->document);
    CadAnimationSession_Init(&g->animation);
    
    /* Initialize views - match window titles */
    CadView_Init(&g->views[0], CAD_VIEW_TOP);    /* "Top" */
    CadView_Init(&g->views[1], CAD_VIEW_3D);     /* "3D View" */
    CadView_Init(&g->views[2], CAD_VIEW_FRONT);  /* "Front" */
    CadView_Init(&g->views[3], CAD_VIEW_RIGHT);  /* "Right" */

    g->menu_count = ARRAY_COUNT(menuDescriptors);
    for (int i = 0; i < g->menu_count; ++i) g->menus[i] = menuDescriptors[i].title;
    g->menu_open = -1;
    g->menu_hover_item = -1;
    g->submenu_open = 0;
    g->submenu_hover_item = -1;
    g->submenu_rect = (Rect){0, 0, 0, 0};

    /* Initialize tool icons to NULL */
    for (int i = 0; i < TOOL_COUNT; i++) {
        g->tool_icons[i] = NULL;
    }
    for (int i = 0; i < 12; i++) {
        g->anim_icons[i] = NULL;
    }
    g->selected_tool = CAD_TOOL_NONE;
    g->point_move_active = 0;
    g->point_move_view = -1;
    g->view_interacting = -1;
    g->view_right_interacting = -1;
    g->view_middle_interacting = -1;
    g->scrollbar_view = -1;
    g->pointer_view = -1;
    g->point_pending_view = -1;
    g->area_select_view = -1;
    g->state_active_field = -1;
    g->auto_layout = 1;
    g->tool_palette_visible = 1;
    g->coordinates_visible = 1;
    g->animation_visible = 1;
    g->animation_docked = 1;
    for (int i = 0; i < 4; ++i) {
        g->view_visible[i] = 1;
        g->view_z_order[i] = i;
    }
    for (int i = 0; i < GUI_AUX_COUNT; ++i) g->aux_z_order[i] = i;
    snprintf(g->status_text, sizeof(g->status_text),
             "Ready - select a tool or use the views to inspect the model");

    g->clipboard = (CadCore*)calloc(1, sizeof(CadCore));
    if (!g->clipboard) {
        CadDocument_Destroy(&g->document);
        free(g);
        return NULL;
    }
    CadCore_Init(g->clipboard);
    g->shape_preview = (CadCore*)calloc(1, sizeof(CadCore));
    if (!g->shape_preview) {
        free(g->clipboard);
        CadDocument_Destroy(&g->document);
        free(g);
        return NULL;
    }
    CadCore_Init(g->shape_preview);
    CadView_Init(&g->shape_preview_view, CAD_VIEW_3D);
    
    /* Initialize individual view scales */
    for (int i = 0; i < 4; i++) {
        g->view_scale[i] = 1.0f; /* Default scale factor */
    }
    
    g->toolPalette.title = "Tool";
    g->toolPalette.r = (Rect){ 20, 20, 90, 668 };
    g->toolPalette.draggable = 1;

    /* 4 views (classic 2x2 grid on the right) - apply individual scales */
    const int baseX = 180, baseY = 20;
    const int baseWinW = 560, baseWinH = 330; /* Base window size */
    int winW0 = (int)(baseWinW * g->view_scale[0]);
    int winH0 = (int)(baseWinH * g->view_scale[0]);
    int winW1 = (int)(baseWinW * g->view_scale[1]);
    int winH1 = (int)(baseWinH * g->view_scale[1]);
    int winW2 = (int)(baseWinW * g->view_scale[2]);
    int winH2 = (int)(baseWinH * g->view_scale[2]);
    int winW3 = (int)(baseWinW * g->view_scale[3]);
    int winH3 = (int)(baseWinH * g->view_scale[3]);
    
    /* Position views accounting for different sizes */
    /* Top-left: Top view */
    g->view[0] = (GuiWin){ "Top",   { baseX + 0,     baseY + 0,     winW0, winH0 }, 1 };
    /* Top-right: 3D View */
    g->view[1] = (GuiWin){ "3D View",{ baseX + winW0,  baseY + 0,     winW1, winH1 }, 1 };
    /* Bottom-left: Front view */
    g->view[2] = (GuiWin){ "Front", { baseX + 0,     baseY + winH0,  winW2, winH2 }, 1 };
    /* Bottom-right: Right view */
    g->view[3] = (GuiWin){ "Right", { baseX + winW0,  baseY + winH0,  winW3, winH3 }, 1 };

    g->coordBox = (GuiWin){ "COORDINATES", { 20, 860, 425, 80 }, 1 };
    g->animationWindow = (GuiWin){ "ANIMATION", { 180, 680, 900, 126 }, 1 };
    g->stateWindow = (GuiWin){ "STATE / TENKEY", { 260, 90, 620, 350 }, 1 };

    g->shapeBrowserWindow = (GuiWin){ "SHAPE BROWSER", { 600, 300, 400, 500 }, 1 };
    g->shapeBrowserWindow.r.w = 0; /* Start hidden */
    g->shapeBrowserWindow.r.h = 0;
    g->shape_names = NULL;
    g->shape_count = 0;
    g->shape_selected = -1;
    g->shape_scroll_offset = 0;
    g->shape_search[0] = '\0';
    g->shape_folder_path[0] = '\0';

    g->anim_copy_source = -1;
    animation_invalidate(g);

    return g;
}

void gui_destroy(GuiState* g) {
    if (!g) return;
    free_asm_browser_sources(g);
    EditorController_CancelEdit(&g->controller);
    CadDocument_Destroy(&g->document);
    CadCore_Destroy(g->clipboard);
    free(g->clipboard);
    CadCore_Destroy(g->shape_preview);
    free(g->shape_preview);
    /* Free tool icons */
    for (int i = 0; i < TOOL_COUNT; i++) {
        if (g->tool_icons[i]) {
            rg_free_texture(g->tool_icons[i]);
        }
    }
    /* Free animation icons */
    for (int i = 0; i < 12; i++) {
        if (g->anim_icons[i]) {
            rg_free_texture(g->anim_icons[i]);
        }
    }

    /* Free shape names */
    if (g->shape_names) {
        for (int i = 0; i < g->shape_count; i++) {
            if (g->shape_names[i]) {
                free(g->shape_names[i]);
            }
        }
        free(g->shape_names);
        g->shape_names = NULL;
    }
    g->shape_count = 0;
    free(g);
}

static int asm_ascii_compare_ci(const char* left, const char* right) {
    unsigned char a;
    unsigned char b;
    if (!left) left = "";
    if (!right) right = "";
    do {
        a = (unsigned char)*left++;
        b = (unsigned char)*right++;
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
        if (a != b) return a < b ? -1 : 1;
    } while (a != 0);
    return 0;
}

static char* asm_copy_string(const char* source) {
    size_t size;
    char* copy;
    if (!source) source = "";
    size = strlen(source) + 1;
    copy = (char*)malloc(size);
    if (copy) memcpy(copy, source, size);
    return copy;
}

static void free_asm_source_array(CadAsmTextSource** sources,
                                  size_t* source_count) {
    size_t index;
    if (!sources || !source_count) return;
    if (*sources) {
        for (index = 0; index < *source_count; ++index) {
            free((void*)(*sources)[index].name);
            CadPlatform_Free((void*)(*sources)[index].bytes);
        }
        free(*sources);
    }
    *sources = NULL;
    *source_count = 0;
}

static void free_asm_browser_sources(GuiState* g) {
    if (!g) return;
    free_asm_source_array(&g->shape_asm_sources,
                          &g->shape_asm_source_count);
    free_asm_source_array(&g->shape_constant_sources,
                          &g->shape_constant_source_count);
    free(g->shape_catalog);
    g->shape_catalog = NULL;
}

static void free_shape_name_array(char*** names, int* count) {
    int index;
    if (!names || !count) return;
    if (*names) {
        for (index = 0; index < *count; ++index) free((*names)[index]);
        free(*names);
    }
    *names = NULL;
    *count = 0;
}

static int append_asm_source(CadAsmTextSource** sources, size_t* count,
                             size_t* capacity, const char* display_name,
                             const char* path, CadResult* failure) {
    CadResult result;
    uint8_t* bytes = NULL;
    size_t size = 0;
    char* copied_name;
    CadAsmTextSource* expanded;
    if (!sources || !count || !capacity || !display_name || !path) return 0;
    result = CadPlatform_ReadFile(path, CAD_ASM_MAX_INPUT_BYTES, &bytes, &size);
    if (!CadResult_IsSuccess(&result)) {
        if (failure) *failure = result;
        return 0;
    }
    if (size == 0) {
        CadPlatform_Free(bytes);
        return 1;
    }
    copied_name = asm_copy_string(display_name);
    if (!copied_name) {
        CadPlatform_Free(bytes);
        return 0;
    }
    if (*count == *capacity) {
        size_t next_capacity = *capacity ? *capacity * 2 : 16;
        expanded = (CadAsmTextSource*)realloc(
            *sources, next_capacity * sizeof(**sources));
        if (!expanded) {
            free(copied_name);
            CadPlatform_Free(bytes);
            return 0;
        }
        *sources = expanded;
        *capacity = next_capacity;
    }
    (*sources)[*count].name = copied_name;
    (*sources)[*count].bytes = bytes;
    (*sources)[*count].size = size;
    ++*count;
    return 1;
}

static int compare_asm_source(const void* left_value,
                              const void* right_value) {
    const CadAsmTextSource* left = (const CadAsmTextSource*)left_value;
    const CadAsmTextSource* right = (const CadAsmTextSource*)right_value;
    int comparison = asm_ascii_compare_ci(left->name, right->name);
    return comparison ? comparison : strcmp(left->name, right->name);
}

static const char* asm_result_message(const CadResult* result) {
    if (result && result->diagnosticCount &&
        result->diagnostics[0].message[0])
        return result->diagnostics[0].message;
    return "Unknown ASM import failure";
}

#ifdef _WIN32
static int collect_asm_folder(const char* folder_path,
                              CadAsmTextSource** sources,
                              size_t* source_count, size_t* source_capacity,
                              CadResult* failure) {
    wchar_t wide_folder[GUI_PATH_CAPACITY * 2];
    wchar_t search_path[GUI_PATH_CAPACITY * 2];
    WIN32_FIND_DATAW find_data;
    HANDLE find_handle;
    if (!gui_utf8_to_wide(folder_path, wide_folder,
                          ARRAY_COUNT(wide_folder)) ||
        _snwprintf_s(search_path, ARRAY_COUNT(search_path), _TRUNCATE,
                     L"%ls\\*.asm", wide_folder) < 0)
        return 0;
    find_handle = FindFirstFileW(search_path, &find_data);
    if (find_handle == INVALID_HANDLE_VALUE) return 0;
    do {
        char file_name[GUI_PATH_CAPACITY];
        char full_path[GUI_PATH_CAPACITY * 2];
        int written;
        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!gui_wide_to_utf8(find_data.cFileName, file_name,
                              ARRAY_COUNT(file_name))) {
            FindClose(find_handle);
            return 0;
        }
        written = snprintf(full_path, sizeof(full_path), "%s\\%s",
                           folder_path, file_name);
        if (written < 0 || (size_t)written >= sizeof(full_path) ||
            !append_asm_source(sources, source_count, source_capacity,
                               file_name, full_path, failure)) {
            FindClose(find_handle);
            return 0;
        }
    } while (FindNextFileW(find_handle, &find_data));
    FindClose(find_handle);
    return *source_count != 0;
}
#endif

static void collect_optional_constant_sources(
    const char* shapes_folder, CadAsmTextSource** sources,
    size_t* source_count, size_t* source_capacity) {
    static const char* const files[] = {
        "STRATEQU.INC", "VARS.INC", "STRUCTS.INC", "MACROS.INC"
    };
    char inc_folder[GUI_PATH_CAPACITY * 2];
    size_t length;
    char* separator;
    size_t index;
    if (!shapes_folder || !sources || !source_count || !source_capacity)
        return;
    if (snprintf(inc_folder, sizeof(inc_folder), "%s", shapes_folder) < 0)
        return;
    length = strlen(inc_folder);
    while (length && (inc_folder[length - 1] == '\\' ||
                      inc_folder[length - 1] == '/'))
        inc_folder[--length] = '\0';
    separator = strrchr(inc_folder, '\\');
    if (!separator) separator = strrchr(inc_folder, '/');
    if (separator) *separator = '\0';
    length = strlen(inc_folder);
    if (length + strlen("\\INC") + 1 >= sizeof(inc_folder)) return;
    memcpy(inc_folder + length, "\\INC", strlen("\\INC") + 1);
    for (index = 0; index < (size_t)ARRAY_COUNT(files); ++index) {
        char full_path[GUI_PATH_CAPACITY * 2];
        CadResult ignored;
        size_t before = *source_count;
        int written = snprintf(full_path, sizeof(full_path), "%s\\%s",
                               shapes_folder, files[index]);
        if (written < 0 || (size_t)written >= sizeof(full_path)) continue;
        (void)append_asm_source(sources, source_count, source_capacity,
                                files[index], full_path, &ignored);
        if (*source_count != before) continue;
        written = snprintf(full_path, sizeof(full_path), "%s\\%s",
                           inc_folder, files[index]);
        if (written < 0 || (size_t)written >= sizeof(full_path)) continue;
        (void)append_asm_source(sources, source_count, source_capacity,
                                files[index], full_path, &ignored);
    }
}

static void scan_asm_folder_for_shapes(GuiState* g,
                                       const char* folder_path) {
    CadAsmTextSource* asm_sources = NULL;
    CadAsmTextSource* constant_sources = NULL;
    size_t asm_count = 0;
    size_t asm_capacity = 0;
    size_t constant_count = 0;
    size_t constant_capacity = 0;
    CadAsmCatalog* catalog = NULL;
    char** names = NULL;
    int name_count = 0;
    CadResult result = CadResult_Ok(CAD_FORMAT_AUTO);
    size_t index;
    if (!g || !folder_path || !folder_path[0]) return;
    if (strlen(folder_path) >= sizeof(g->shape_folder_path)) {
        gui_set_status(g, "ASM folder path is too long");
        return;
    }
#ifdef _WIN32
    if (!collect_asm_folder(folder_path, &asm_sources, &asm_count,
                            &asm_capacity, &result)) {
        gui_set_status(g, "Could not read ASM folder: %s",
                       result.errorCount ? asm_result_message(&result)
                                         : "no .asm files found");
        free_asm_source_array(&asm_sources, &asm_count);
        return;
    }
#else
    (void)asm_capacity;
    gui_set_status(g, "ASM shape browsing is currently Windows-only");
    return;
#endif
    qsort(asm_sources, asm_count, sizeof(*asm_sources), compare_asm_source);
    collect_optional_constant_sources(folder_path, &constant_sources,
                                      &constant_count, &constant_capacity);
    catalog = (CadAsmCatalog*)malloc(sizeof(*catalog));
    if (!catalog) {
        gui_set_status(g, "Could not allocate the ASM shape catalog");
        goto fail;
    }
    result = CadImportAsm_BuildCatalog(asm_sources, asm_count, catalog);
    if (!CadResult_IsSuccess(&result)) {
        gui_set_status(g, "ASM catalog rejected: %s",
                       asm_result_message(&result));
        goto fail;
    }
    names = (char**)calloc(catalog->shapeCount ? catalog->shapeCount : 1,
                           sizeof(*names));
    if (!names) {
        gui_set_status(g, "Could not allocate the ASM shape list");
        goto fail;
    }
    for (index = 0; index < catalog->shapeCount; ++index) {
        if (name_count > 0 &&
            asm_ascii_compare_ci(names[name_count - 1],
                                 catalog->shapes[index].name) == 0)
            continue;
        names[name_count] = asm_copy_string(catalog->shapes[index].name);
        if (!names[name_count]) {
            gui_set_status(g, "Could not allocate an ASM shape name");
            goto fail;
        }
        ++name_count;
    }
    if (name_count == 0) {
        gui_set_status(g, "No recovered shape definitions were found");
        goto fail;
    }

    free_asm_browser_sources(g);
    free_shape_name_array(&g->shape_names, &g->shape_count);
    g->shape_asm_sources = asm_sources;
    g->shape_asm_source_count = asm_count;
    g->shape_constant_sources = constant_sources;
    g->shape_constant_source_count = constant_count;
    g->shape_catalog = catalog;
    g->shape_names = names;
    g->shape_count = name_count;
    g->shape_selected = -1;
    g->shape_scroll_offset = 0;
    g->shape_search_active = 0;
    g->shape_search[0] = '\0';
    g->shape_preview_valid = 0;
    CadCore_Clear(g->shape_preview);
    snprintf(g->shape_folder_path, sizeof(g->shape_folder_path), "%s",
             folder_path);
    {
        int width = g->layout_width > 20 && g->layout_width - 20 < 640
                        ? g->layout_width - 20 : 640;
        int height = g->layout_height > 70 && g->layout_height - 50 < 520
                         ? g->layout_height - 50 : 520;
        int x;
        int y;
        if (width < 420) width = 420;
        if (height < 360) height = 360;
        x = g->layout_width > width ? (g->layout_width - width) / 2 : 8;
        y = g->layout_height > height ? (g->layout_height - height) / 2
                                      : MenuBarHeight() + 4;
        if (y < MenuBarHeight() + 4) y = MenuBarHeight() + 4;
        g->shapeBrowserWindow.r = (Rect){ x, y, width, height };
        raise_aux(g, GUI_AUX_SHAPE_BROWSER);
    }
    gui_set_status(g, "Found %d ASM shapes%s; select one to preview",
                   name_count, result.warningCount ? " (with diagnostics)" : "");
    return;

fail:
    free_shape_name_array(&names, &name_count);
    free(catalog);
    free_asm_source_array(&constant_sources, &constant_count);
    free_asm_source_array(&asm_sources, &asm_count);
}

static int load_shape_from_asm(GuiState* g, CadCore* core,
                               const char* shape_name) {
    const CadAsmShapeInfo* shape;
    CadFileData* decoded;
    CadAsmImportInfo info;
    CadResult result;
    if (!g || !core || !shape_name || !g->shape_catalog ||
        !g->shape_asm_sources) return 0;
    shape = CadImportAsm_FindShape(g->shape_catalog, shape_name);
    if (!shape) {
        gui_set_status(g, "ASM shape '%s' is no longer in the catalog",
                       shape_name);
        return 0;
    }
    decoded = (CadFileData*)malloc(sizeof(*decoded));
    if (!decoded) {
        gui_set_status(g, "Could not allocate an ASM preview document");
        return 0;
    }
    result = CadImportAsm_DecodeCatalogShape(
        g->shape_asm_sources, g->shape_asm_source_count,
        g->shape_constant_sources, g->shape_constant_source_count,
        shape, NULL, decoded, &info);
    if (!CadResult_IsSuccess(&result)) {
        const CadDiagnostic* diagnostic = result.diagnosticCount
                                              ? &result.diagnostics[0] : NULL;
        gui_set_status(g, "%s:%d: %s", shape->sourceName,
                       diagnostic ? diagnostic->recordIndex : 0,
                       asm_result_message(&result));
        free(decoded);
        return 0;
    }
    CadCore_Clear(core);
    core->data = *decoded;
    CadCore_RebuildDerivedState(core);
    core->isDirty = 1;
    free(decoded);
    return 1;
}

typedef struct ShapeBrowserLayout {
    Rect inner;
    Rect search;
    Rect list;
    Rect preview;
    Rect replace_button;
} ShapeBrowserLayout;

static ShapeBrowserLayout shape_browser_layout(const GuiState* g) {
    Rect window = g->shapeBrowserWindow.r;
    ShapeBrowserLayout layout;
    layout.inner = (Rect){ window.x + 6, window.y + 26, window.w - 12, window.h - 32 };
    layout.search = (Rect){ layout.inner.x + 54, layout.inner.y + 48,
                            layout.inner.w - 62, 24 };
    int body_y = layout.inner.y + 82;
    int body_h = layout.inner.h - 128;
    int list_w = (layout.inner.w - 28) * 2 / 5;
    layout.list = (Rect){ layout.inner.x + 8, body_y, list_w, body_h };
    layout.preview = (Rect){ layout.list.x + layout.list.w + 12, body_y,
                             layout.inner.x + layout.inner.w - 8 -
                                 (layout.list.x + layout.list.w + 12),
                             body_h };
    layout.replace_button = (Rect){ layout.inner.x + layout.inner.w - 102,
                                    layout.inner.y + layout.inner.h - 34, 94, 26 };
    return layout;
}

static int shape_name_matches(const char* name, const char* query) {
    if (!name) return 0;
    if (!query || !query[0]) return 1;
    for (const char* start = name; *start; ++start) {
        const char* left = start;
        const char* right = query;
        while (*left && *right &&
               tolower((unsigned char)*left) == tolower((unsigned char)*right)) {
            ++left;
            ++right;
        }
        if (!*right) return 1;
    }
    return 0;
}

static int filtered_shape_count(const GuiState* g) {
    int count = 0;
    if (!g) return 0;
    for (int i = 0; i < g->shape_count; ++i) {
        count += shape_name_matches(g->shape_names[i], g->shape_search);
    }
    return count;
}

static int filtered_shape_index(const GuiState* g, int filtered_index) {
    if (!g || filtered_index < 0) return -1;
    for (int i = 0; i < g->shape_count; ++i) {
        if (!shape_name_matches(g->shape_names[i], g->shape_search)) continue;
        if (filtered_index-- == 0) return i;
    }
    return -1;
}

static void fit_shape_preview(GuiState* g) {
    if (!g || !g->shape_preview) return;
    double radius = 1.0;
    for (int i = 0; i < g->shape_preview->data.pointCount; ++i) {
        const CadPoint* point = &g->shape_preview->data.points[i];
        if (!point->flags) continue;
        double distance = sqrt(point->pointx * point->pointx +
                               point->pointy * point->pointy +
                               point->pointz * point->pointz);
        if (distance > radius) radius = distance;
    }
    CadView_Reset(&g->shape_preview_view);
    g->shape_preview_view.wireframe = 0;
    g->shape_preview_view.camera_distance = radius * 3.0 + 40.0;
}

static void select_shape_preview(GuiState* g, int index) {
    if (!g || !g->shape_preview || index < 0 || index >= g->shape_count ||
        !g->shape_names[index]) return;
    g->shape_selected = index;
    g->shape_preview_valid = load_shape_from_asm(
        g, g->shape_preview, g->shape_names[index]);
    if (!g->shape_preview_valid) {
        return;
    }
    fit_shape_preview(g);
    gui_set_status(g, "Previewing %s (%d points, %d faces); choose Replace to import",
                   g->shape_names[index], g->shape_preview->data.pointCount,
                   g->shape_preview->data.polygonCount);
}

static int shape_browser_key(GuiState* g, int key) {
    if (!g || !g->shape_search_active) return 0;
    size_t length = strlen(g->shape_search);
    if (key == GUI_KEY_ESCAPE || key == GUI_KEY_ENTER) {
        g->shape_search_active = 0;
        return 1;
    }
    if (key == GUI_KEY_BACKSPACE || key == GUI_KEY_DELETE) {
        if (length) g->shape_search[length - 1] = '\0';
    } else if (key >= 32 && key <= 126 && length + 1 < sizeof(g->shape_search)) {
        g->shape_search[length] = (char)key;
        g->shape_search[length + 1] = '\0';
    } else {
        return 1;
    }
    g->shape_scroll_offset = 0;
    g->shape_selected = -1;
    g->shape_preview_valid = 0;
    CadCore_Clear(g->shape_preview);
    return 1;
}

void gui_set_font(GuiState* g, FontWin32* font) {
    if (!g) return;
    g->font = font;
}

GuiCommand gui_take_command(GuiState* g) {
    if (!g) return GUI_COMMAND_NONE;

    const GuiCommand command = g->pending_command;
    g->pending_command = GUI_COMMAND_NONE;
    return command;
}

int gui_request_quit(GuiState* g) {
    if (!g) return 0;
    if (EditorController_IsEditing(&g->controller) || g->document.transactionBefore)
        reset_interaction(g);
    if (!confirm_replace_document(g, "quitting")) return 0;
    g->pending_command = GUI_COMMAND_QUIT;
    return 1;
}

void gui_cancel_input(GuiState* g) {
    if (!g) return;
    if (g->anim_scrubbing) CadAnimationSession_EndScrub(&g->animation);
    g->anim_scrubbing = 0;
    g->anim_copy_mode = 0;
    reset_interaction(g);
    g->menu_open = -1;
    g->menu_hover_item = -1;
    g->submenu_open = 0;
    g->submenu_hover_item = -1;
    g->shape_search_active = 0;
}

void gui_handle_key(GuiState* g, int key, unsigned modifiers, int pressed) {
    if (!g || !pressed) return;
    if (key >= 'A' && key <= 'Z') key += 'a' - 'A';

    if (g->state_visible) {
        state_panel_key(g, key, modifiers);
        return;
    }
    if (g->shapeBrowserWindow.r.w > 0 && g->shape_search_active &&
        !(modifiers & GUI_MOD_CTRL) && shape_browser_key(g, key)) {
        return;
    }

    if (key == GUI_KEY_ESCAPE) {
        const int had_operation = g->point_pending || g->area_select_armed ||
                                  g->area_select_active || g->point_move_active ||
                                  g->selected_tool == CAD_TOOL_FACE_CREATE;
        const int had_menu = g->menu_open >= 0 || g->submenu_open;
        const int had_selection = g->cad->selection.pointCount > 0 ||
                                  g->cad->selection.polygonCount > 0;
        if (g->document.transactionBefore) history_cancel(g);
        reset_interaction(g);
        g->menu_open = -1;
        g->submenu_open = 0;
        if (g->selected_tool == CAD_TOOL_FACE_CREATE) CadCore_ClearSelection(g->cad);
        if (had_operation) gui_set_status(g, "Operation cancelled");
        else if (!had_menu && had_selection) {
            CadCore_ClearSelection(g->cad);
            gui_set_status(g, "Deselected all");
        }
        return;
    }

    if ((modifiers & GUI_MOD_CTRL) != 0) {
        switch (key) {
        case 'n': execute_editor_command(g, CAD_COMMAND_FILE_NEW); return;
        case 'o': execute_editor_command(g, CAD_COMMAND_FILE_OPEN); return;
        case 's': execute_editor_command(g, (modifiers & GUI_MOD_SHIFT)
                                            ? CAD_COMMAND_FILE_SAVE_AS
                                            : CAD_COMMAND_FILE_SAVE); return;
        case 'z': execute_editor_command(g, (modifiers & GUI_MOD_SHIFT)
                                            ? CAD_COMMAND_EDIT_REDO
                                            : CAD_COMMAND_EDIT_UNDO); return;
        case 'y': execute_editor_command(g, CAD_COMMAND_EDIT_REDO); return;
        case 'a': execute_editor_command(
                      g, (modifiers & GUI_MOD_SHIFT)
                             ? CAD_COMMAND_OPTION_DESELECT_ALL
                             : CAD_COMMAND_OPTION_SELECT_ALL);
                  return;
        case 'c': execute_editor_command(g, CAD_COMMAND_EDIT_COPY); return;
        case 'v': execute_editor_command(g, CAD_COMMAND_EDIT_PASTE); return;
        case 'q': gui_request_quit(g); return;
        default: break;
        }
    }

    if (g->selected_tool == CAD_TOOL_FACE_CREATE) {
        if (key == GUI_KEY_BACKSPACE) {
            if (g->cad->selection.pointCount > 0) {
                int16_t last = g->cad->selection.selectedPoints[
                    g->cad->selection.pointCount - 1];
                CadCore_DeselectPoint(g->cad, last);
                update_face_creation_status(g, -1);
            } else {
                gui_set_status(g, "Face preview is already empty");
            }
            return;
        }
        if (key == GUI_KEY_ENTER) {
            create_face_from_selection(g);
            return;
        }
    }

    if (key == 'f' && !(modifiers & (GUI_MOD_CTRL | GUI_MOD_ALT))) {
        if (frame_document_views(g, 1)) {
            gui_set_status(g, "Framed the current selection (F)");
        } else {
            gui_set_status(g, "Select points or faces before using Frame Selection");
        }
        return;
    }

    if (key == GUI_KEY_DELETE || key == GUI_KEY_BACKSPACE) {
        activate_tool(g, g->cad->selectModeFlag ? CAD_TOOL_POINT_DELETE : CAD_TOOL_FACE_DELETE);
    }
}

const char* gui_window_title(GuiState* g) {
    if (!g) return "3DCad";
    const char* name = g->document.savePath ? g->document.savePath
                        : (g->document.sourcePath ? g->document.sourcePath : "Untitled");
    const char* slash = strrchr(name, '/');
    const char* backslash = strrchr(name, '\\');
    if (slash && slash[1]) name = slash + 1;
    if (backslash && backslash[1] && backslash + 1 > name) name = backslash + 1;
    snprintf(g->title_text, sizeof(g->title_text), "3DCad - %s%s",
             name, g->document.isDirty ? " *" : "");
    return g->title_text;
}

void gui_load_tool_icons(GuiState* g, const char* resource_path) {
    if (!g || !resource_path) return;
    
    /* Tool icon filenames in order (matching toolIcons array from bitmap.c) */
    const char* tool_names[TOOL_COUNT] = {
        "pointselect_bits_32x48.png",
        "faceselect_bits_32x48.png",
        "point_bits_32x48.png",
        "make_bits_32x48.png",
        "addpoint_bits_32x48.png",
        "color_bits_32x48.png",
        "pointmove_bits_32x48.png",
        "facemove_bits_32x48.png",
        "pointrotate_bits_32x48.png",
        "facerotate_bits_32x48.png",
        "pointscale_bits_32x48.png",
        "facescale_bits_32x48.png",
        "delpoint_bits_32x48.png",
        "delface_bits_32x48.png",
        "flip_bits_32x48.png",
        "mirror_bits_32x48.png",
        "faceflip_bits_32x48.png",
        "facecopy_bits_32x48.png",
        "facecut_bits_32x48.png",
        "faceside_bits_32x48.png",
        "state_bits_32x48.png",
        "transfer_bits_32x48.png",
        "primitive_bits_32x48.png",
        "UNDO_bits_32x48.png"
    };
    
    char path[512];
    for (int i = 0; i < TOOL_COUNT; i++) {
        rg_free_texture(g->tool_icons[i]);
        g->tool_icons[i] = NULL;
        snprintf(path, sizeof(path), "%s/%s", resource_path, tool_names[i]);
        g->tool_icons[i] = rg_load_texture(path);
        if (!g->tool_icons[i]) {
            fprintf(stderr, "Warning: Failed to load tool icon %d: %s\n", i, tool_names[i]);
        }
    }
}

void gui_load_anim_icons(GuiState* g, const char* resource_path) {
    if (!g || !resource_path) return;
    
    /* Animation icon filenames in order */
    const char* anim_names[12] = {
        "beframe_bits_24x48.png",      /* 0: 10 frames back */
        "topfram_bits_24x48.png",      /* 1: First/Last frame button */
        "beforeframe_bits_24x48.png",  /* 2: 1 frame back */
        "goframe_bits_32x48.png",      /* 3: Preview/Play animation */
        "nextframe_bits_24x48.png",    /* 4: 1 frame forward */
        "nexframe_bits_24x48.png",     /* 5: 10 frames forward */
        "kplus_bits_32x20.png",        /* 6: Add keyframe (30x20 in code) */
        "kminus_bits_32x20.png",       /* 7: Delete keyframe (30x20 in code) */
        "plus_bits_32x30.png",         /* 8: Add frame (30x30 in code) */
        "minus_bits_32x30.png",        /* 9: Delete frame (30x30 in code) */
        "copy_bits_32x30.png",         /* 10: Copy (30x30 in code) */
        "toguru_bits_48x24.png"        /* 11: Loop toggle (48x24 in code) */
    };
    
    char path[512];
    for (int i = 0; i < 12; i++) {
        rg_free_texture(g->anim_icons[i]);
        g->anim_icons[i] = NULL;
        snprintf(path, sizeof(path), "%s/%s", resource_path, anim_names[i]);
        g->anim_icons[i] = rg_load_texture(path);
        if (!g->anim_icons[i]) {
            fprintf(stderr, "Warning: Failed to load animation icon %d: %s\n", i, anim_names[i]);
        }
    }
}

enum { VIEW_SCROLLBAR_SIZE = 14 };

typedef struct ViewScrollbarGeometry {
    Rect horizontal_track;
    Rect vertical_track;
    Rect horizontal_thumb;
    Rect vertical_thumb;
    Rect corner;
} ViewScrollbarGeometry;

static Rect view_client_rect(const GuiState* g, int view_index) {
    Rect r = g->view[view_index].r;
    return (Rect){ r.x + 6, r.y + 26, r.w - 12, r.h - 32 };
}

static Rect view_content_rect(const GuiState* g, int view_index) {
    Rect r = view_client_rect(g, view_index);
    r.w -= VIEW_SCROLLBAR_SIZE;
    r.h -= VIEW_SCROLLBAR_SIZE;
    if (r.w < 1) r.w = 1;
    if (r.h < 1) r.h = 1;
    return r;
}

static void link_orthographic_pan(GuiState* g, int source_index) {
    if (!g || source_index < 0 || source_index >= 4) return;
    CadView* source = &g->views[source_index];
    if (source->type == CAD_VIEW_3D || source->zoom <= 0.0) return;
    double world_x = 0.0, world_y = 0.0, world_z = 0.0;
    int has_x = 0, has_y = 0, has_z = 0;
    switch (source->type) {
    case CAD_VIEW_TOP:
        world_x = -source->pan_x / source->zoom;
        world_z = source->pan_y / source->zoom;
        has_x = has_z = 1;
        break;
    case CAD_VIEW_FRONT:
        world_x = -source->pan_x / source->zoom;
        world_y = -source->pan_y / source->zoom;
        has_x = has_y = 1;
        break;
    case CAD_VIEW_RIGHT:
        world_z = -source->pan_x / source->zoom;
        world_y = -source->pan_y / source->zoom;
        has_y = has_z = 1;
        break;
    default:
        return;
    }
    for (int i = 0; i < 4; ++i) {
        CadView* view = &g->views[i];
        if (i == source_index || view->type == CAD_VIEW_3D) continue;
        if (has_x && (view->type == CAD_VIEW_TOP || view->type == CAD_VIEW_FRONT)) {
            view->pan_x = -world_x * view->zoom;
        }
        if (has_y && (view->type == CAD_VIEW_FRONT || view->type == CAD_VIEW_RIGHT)) {
            view->pan_y = -world_y * view->zoom;
        }
        if (has_z && view->type == CAD_VIEW_TOP) view->pan_y = world_z * view->zoom;
        if (has_z && view->type == CAD_VIEW_RIGHT) view->pan_x = -world_z * view->zoom;
    }
}

static void view_scrollbar_geometry(Rect client, const CadView* view,
                                    ViewScrollbarGeometry* geometry) {
    if (!geometry) return;
    memset(geometry, 0, sizeof(*geometry));
    if (client.w <= VIEW_SCROLLBAR_SIZE || client.h <= VIEW_SCROLLBAR_SIZE) return;
    geometry->horizontal_track = (Rect){ client.x, client.y + client.h - VIEW_SCROLLBAR_SIZE,
                                         client.w - VIEW_SCROLLBAR_SIZE, VIEW_SCROLLBAR_SIZE };
    geometry->vertical_track = (Rect){ client.x + client.w - VIEW_SCROLLBAR_SIZE, client.y,
                                       VIEW_SCROLLBAR_SIZE, client.h - VIEW_SCROLLBAR_SIZE };
    geometry->corner = (Rect){ client.x + client.w - VIEW_SCROLLBAR_SIZE,
                               client.y + client.h - VIEW_SCROLLBAR_SIZE,
                               VIEW_SCROLLBAR_SIZE, VIEW_SCROLLBAR_SIZE };

    int horizontal_rail = geometry->horizontal_track.w - 4;
    int vertical_rail = geometry->vertical_track.h - 4;
    double zoom = view && view->zoom > 1.0 ? view->zoom : 1.0;
    int horizontal_thumb = (int)lround(horizontal_rail / zoom);
    int vertical_thumb = (int)lround(vertical_rail / zoom);
    if (horizontal_thumb < 24) horizontal_thumb = 24;
    if (vertical_thumb < 24) vertical_thumb = 24;
    if (horizontal_thumb > horizontal_rail) horizontal_thumb = horizontal_rail;
    if (vertical_thumb > vertical_rail) vertical_thumb = vertical_rail;
    int horizontal_range = horizontal_rail - horizontal_thumb;
    int vertical_range = vertical_rail - vertical_thumb;
    double horizontal_position = view ? tanh(view->pan_x / 250.0) : 0.0;
    double vertical_position = view ? tanh(view->pan_y / 250.0) : 0.0;
    geometry->horizontal_thumb = (Rect){
        geometry->horizontal_track.x + 2 +
            (int)lround(horizontal_range * (horizontal_position + 1.0) * 0.5),
        geometry->horizontal_track.y + 2, horizontal_thumb,
        geometry->horizontal_track.h - 4
    };
    geometry->vertical_thumb = (Rect){
        geometry->vertical_track.x + 2,
        geometry->vertical_track.y + 2 +
            (int)lround(vertical_range * (vertical_position + 1.0) * 0.5),
        geometry->vertical_track.w - 4, vertical_thumb
    };
}

static void layout_cleanup(GuiState* g, int win_w, int win_h) {
    CadDesktopLayoutInput input;
    CadDesktopLayout layout;
    if (!g || win_w <= 0 || win_h <= 0) return;
    CadDesktopLayoutInput_Init(&input, win_w, win_h);
    for (int i = 0; i < CAD_DESKTOP_VIEW_COUNT; ++i)
        input.viewVisible[i] = (unsigned char)(g->view_visible[i] != 0);
    input.toolPaletteVisible = (unsigned char)(g->tool_palette_visible != 0);
    input.coordinatesVisible = (unsigned char)(g->coordinates_visible != 0);
    input.animationVisible = (unsigned char)(g->animation_visible != 0);
    input.animationDocked = (unsigned char)(g->animation_docked != 0);
    if (!CadDesktopLayout_Compute(&input, &layout)) return;

#define GUI_RECT_FROM_UI(rect) \
    (Rect){ (rect).x, (rect).y, (rect).width, (rect).height }
    if (g->tool_palette_visible)
        g->toolPalette.r = GUI_RECT_FROM_UI(layout.toolPalette);
    if (g->coordinates_visible)
        g->coordBox.r = GUI_RECT_FROM_UI(layout.coordinatesPanel);
    if (g->animation_visible && g->animation_docked)
        g->animationWindow.r = GUI_RECT_FROM_UI(layout.animationPanel);
    else if (g->animation_visible &&
             (g->animationWindow.r.w <= 0 || g->animationWindow.r.h <= 0))
        g->animationWindow.r = GUI_RECT_FROM_UI(layout.floatingAnimationDefault);
    for (int i = 0; i < CAD_DESKTOP_VIEW_COUNT; ++i)
        if (g->view_visible[i]) g->view[i].r = GUI_RECT_FROM_UI(layout.views[i]);
#undef GUI_RECT_FROM_UI
    g->layout_width = win_w;
    g->layout_height = win_h;
}

static void clamp_window_reachable(GuiWin* window, int win_w, int win_h) {
    CadUiRect work;
    CadUiRect current;
    CadUiRect clamped;
    int work_height;
    if (!window || window->r.w <= 0 || window->r.h <= 0 ||
        win_w <= 0 || win_h <= 0) return;
    work_height = win_h - MenuBarHeight() - 22;
    if (work_height < 1) work_height = 1;
    work = (CadUiRect){0, MenuBarHeight(), win_w, work_height};
    current = (CadUiRect){window->r.x, window->r.y,
                          window->r.w, window->r.h};
    clamped = CadUiRect_ClampReachable(current, work, 20, 80);
    window->r.x = clamped.x;
    window->r.y = clamped.y;
}

static void clamp_manual_layout(GuiState* g, int win_w, int win_h) {
    if (!g) return;
    for (int i = 0; i < 4; ++i)
        if (g->view_visible[i]) clamp_window_reachable(&g->view[i], win_w, win_h);
    if (g->tool_palette_visible)
        clamp_window_reachable(&g->toolPalette, win_w, win_h);
    if (g->coordinates_visible)
        clamp_window_reachable(&g->coordBox, win_w, win_h);
    if (g->animation_visible && !g->animation_docked)
        clamp_window_reachable(&g->animationWindow, win_w, win_h);
    if (g->shapeBrowserWindow.r.w > 0 && g->shapeBrowserWindow.r.h > 0)
        clamp_window_reachable(&g->shapeBrowserWindow, win_w, win_h);
    if (g->state_visible)
        clamp_window_reachable(&g->stateWindow, win_w, win_h);
}

typedef struct ToolMetrics {
    int button_w;
    int button_h;
    int icon_w;
    int icon_h;
    int start_x;
    int start_y;
    int col_gap;
    int row_gap;
} ToolMetrics;

static ToolMetrics tool_metrics(const GuiState* g) {
    ToolMetrics m = { 36, 52, 32, 48, 0, 0, 2, 1 };
    Rect inner = { g->toolPalette.r.x + 6, g->toolPalette.r.y + 26,
                   g->toolPalette.r.w - 12, g->toolPalette.r.h - 32 };
    const int rows = (CAD_TOOL_COUNT + 1) / 2;
    int available = inner.h - (rows - 1) * m.row_gap;
    if (available / rows < m.button_h) m.button_h = available / rows;
    if (m.button_h < 24) m.button_h = 24;
    m.icon_h = m.button_h - 4;
    if (m.icon_h > 48) m.icon_h = 48;
    m.icon_w = (m.icon_h * 2) / 3;
    if (m.icon_w > 32) m.icon_w = 32;
    m.button_w = m.icon_w + 4;
    int total_w = m.button_w * 2 + m.col_gap;
    m.start_x = inner.x + (inner.w - total_w) / 2;
    m.start_y = inner.y;
    return m;
}

static Rect tool_button_rect(const GuiState* g, int index) {
    ToolMetrics m = tool_metrics(g);
    int col = index % 2;
    int row = index / 2;
    return (Rect){ m.start_x + col * (m.button_w + m.col_gap),
                   m.start_y + row * (m.button_h + m.row_gap),
                   m.button_w, m.button_h };
}

static int polygon_point_indices(const CadCore* core, int16_t polygon_index,
                                 int16_t* result, int capacity) {
    if (!core || !result || capacity <= 0 || polygon_index < 0 ||
        polygon_index >= core->data.polygonCount) return 0;
    const CadPolygon* polygon = &core->data.polygons[polygon_index];
    if (!polygon->flags) return 0;
    int count = 0;
    int16_t current = polygon->firstPoint;
    while (current >= 0 && current < core->data.pointCount && count < capacity &&
           count < polygon->npoints) {
        int repeated = 0;
        for (int i = 0; i < count; ++i) repeated |= result[i] == current;
        if (repeated || !core->data.points[current].flags) break;
        result[count++] = current;
        current = core->data.points[current].nextPoint;
    }
    return count;
}

static int include_point_in_bounds(const GuiState* g, int16_t point_index,
                                   double* min_x, double* min_y, double* min_z,
                                   double* max_x, double* max_y, double* max_z,
                                   int* count) {
    CadPosition point;
    if (!g || !count || !gui_scene_point(g, point_index, &point)) return 0;
    if (*count == 0) {
        *min_x = *max_x = point.x;
        *min_y = *max_y = point.y;
        *min_z = *max_z = point.z;
    } else {
        if (point.x < *min_x) *min_x = point.x;
        if (point.x > *max_x) *max_x = point.x;
        if (point.y < *min_y) *min_y = point.y;
        if (point.y > *max_y) *max_y = point.y;
        if (point.z < *min_z) *min_z = point.z;
        if (point.z > *max_z) *max_z = point.z;
    }
    ++*count;
    return 1;
}

static int frame_document_views(GuiState* g, int selection_only) {
    double min_x = 0.0, min_y = 0.0, min_z = 0.0;
    double max_x = 0.0, max_y = 0.0, max_z = 0.0;
    int count = 0;
    int16_t chain[CAD_MAX_FACE_POINTS];
    if (!g || !g->cad) return 0;

    if (selection_only) {
        for (int i = 0; i < g->cad->selection.pointCount; ++i) {
            include_point_in_bounds(g, g->cad->selection.selectedPoints[i],
                                    &min_x, &min_y, &min_z, &max_x, &max_y, &max_z, &count);
        }
        for (int i = 0; i < g->cad->selection.polygonCount; ++i) {
            int chain_count = polygon_point_indices(
                g->cad, g->cad->selection.selectedPolygons[i], chain, ARRAY_COUNT(chain));
            for (int point = 0; point < chain_count; ++point) {
                include_point_in_bounds(g, chain[point],
                                        &min_x, &min_y, &min_z,
                                        &max_x, &max_y, &max_z, &count);
            }
        }
    } else {
        for (int i = 0; i < g->cad->data.pointCount; ++i) {
            include_point_in_bounds(g, (int16_t)i,
                                    &min_x, &min_y, &min_z, &max_x, &max_y, &max_z, &count);
        }
    }
    if (!count) return 0;

    for (int i = 0; i < 4; ++i) {
        Rect content = view_content_rect(g, i);
        if (selection_only) {
            CadView_FrameBoundsPreserveOrientation(
                &g->views[i], min_x, min_y, min_z,
                max_x, max_y, max_z, content.w, content.h);
        } else {
            CadView_FrameBounds(&g->views[i], min_x, min_y, min_z,
                                max_x, max_y, max_z, content.w, content.h);
        }
    }
    return 1;
}

static int point_in_other_polygon(const CadCore* core, int16_t point_index,
                                  int16_t excluded_polygon) {
    int16_t chain[CAD_MAX_FACE_POINTS];
    for (int p = 0; p < core->data.polygonCount; ++p) {
        if (p == excluded_polygon || !core->data.polygons[p].flags) continue;
        int count = polygon_point_indices(core, (int16_t)p, chain, ARRAY_COUNT(chain));
        for (int i = 0; i < count; ++i) if (chain[i] == point_index) return 1;
    }
    return 0;
}

static void attach_polygon_to_root(CadCore* core, int16_t polygon_index) {
    if (!core || polygon_index < 0) return;
    int16_t root = INVALID_INDEX;
    for (int i = 0; i < core->data.objectCount; ++i) {
        if (core->data.objects[i].flags && core->data.objects[i].parentObject == INVALID_INDEX) {
            root = (int16_t)i;
            break;
        }
    }
    if (root == INVALID_INDEX) root = CadCore_AddObject(core, INVALID_INDEX, 0.0, 0.0, 0.0);
    if (root == INVALID_INDEX) return;
    CadObject* object = &core->data.objects[root];
    if (object->firstPolygon == INVALID_INDEX) {
        object->firstPolygon = polygon_index;
        return;
    }
    int16_t current = object->firstPolygon;
    int guard = 0;
    while (current >= 0 && current < core->data.polygonCount && guard++ < CAD_MAX_POLYGONS) {
        if (core->data.polygons[current].nextPolygon == INVALID_INDEX) {
            core->data.polygons[current].nextPolygon = polygon_index;
            return;
        }
        current = core->data.polygons[current].nextPolygon;
    }
}

static int16_t create_polygon_from_coordinates(CadCore* core,
                                               const double coords[][3], int count,
                                               uint8_t color, uint8_t side,
                                               int reverse) {
    if (!core || !coords || count < CAD_MIN_FACE_POINTS || count > CAD_MAX_FACE_POINTS) {
        return INVALID_INDEX;
    }
    int16_t points[CAD_MAX_FACE_POINTS];
    int made = 0;
    for (int i = 0; i < count; ++i) {
        int source = reverse ? count - 1 - i : i;
        points[made] = CadCore_AddPoint(core, coords[source][0], coords[source][1], coords[source][2]);
        if (points[made] == INVALID_INDEX) break;
        ++made;
    }
    if (made != count) {
        for (int i = 0; i < made; ++i) CadCore_DeletePoint(core, points[i]);
        return INVALID_INDEX;
    }
    for (int i = 0; i < count; ++i) {
        core->data.points[points[i]].nextPoint = i + 1 < count ? points[i + 1] : INVALID_INDEX;
    }
    int16_t polygon = CadCore_AddPolygon(core, points[0], color, (uint8_t)count);
    if (polygon == INVALID_INDEX) {
        for (int i = 0; i < count; ++i) CadCore_DeletePoint(core, points[i]);
        return INVALID_INDEX;
    }
    core->data.polygons[polygon].animation = INVALID_INDEX;
    core->data.polygons[polygon].side = side;
    attach_polygon_to_root(core, polygon);
    return polygon;
}

static int16_t duplicate_polygon_from_core(CadCore* destination, const CadCore* source,
                                           int16_t polygon_index, double offset_x,
                                           int mirror, double mirror_center,
                                           int reverse) {
    int16_t chain[CAD_MAX_FACE_POINTS];
    int count = polygon_point_indices(source, polygon_index, chain, ARRAY_COUNT(chain));
    if (count < CAD_MIN_FACE_POINTS) return INVALID_INDEX;
    double coords[CAD_MAX_FACE_POINTS][3];
    for (int i = 0; i < count; ++i) {
        const CadPoint* point = &source->data.points[chain[i]];
        coords[i][0] = mirror ? (2.0 * mirror_center - point->pointx) : (point->pointx + offset_x);
        coords[i][1] = point->pointy;
        coords[i][2] = point->pointz;
    }
    const CadPolygon* polygon = &source->data.polygons[polygon_index];
    return create_polygon_from_coordinates(destination, coords, count,
                                           polygon->color, polygon->side, reverse);
}

static void delete_polygon_geometry(CadCore* core, int16_t polygon_index) {
    int16_t chain[CAD_MAX_FACE_POINTS];
    int count = polygon_point_indices(core, polygon_index, chain, ARRAY_COUNT(chain));
    int16_t successor = core->data.polygons[polygon_index].nextPolygon;
    for (int i = 0; i < core->data.objectCount; ++i) {
        CadObject* object = &core->data.objects[i];
        if (object->flags && object->firstPolygon == polygon_index) object->firstPolygon = successor;
    }
    for (int i = 0; i < core->data.polygonCount; ++i) {
        CadPolygon* polygon = &core->data.polygons[i];
        if (!polygon->flags || i == polygon_index) continue;
        if (polygon->nextPolygon == polygon_index) polygon->nextPolygon = successor;
        if (polygon->both == polygon_index) polygon->both = INVALID_INDEX;
    }
    if (!CadCore_DeletePolygon(core, polygon_index)) return;
    for (int i = 0; i < count; ++i) {
        if (!point_in_other_polygon(core, chain[i], polygon_index)) CadCore_DeletePoint(core, chain[i]);
    }
}

static int collect_transform_points(GuiState* g, int face_tool,
                                    int16_t* result, int capacity) {
    int count = 0;
    if (!g || !g->cad || !result) return 0;
    if (!face_tool) {
        for (int i = 0; i < g->cad->selection.pointCount && count < capacity; ++i) {
            int16_t point = g->cad->selection.selectedPoints[i];
            if (CadCore_IsPointValid(g->cad, point)) result[count++] = point;
        }
        return count;
    }
    int16_t chain[CAD_MAX_FACE_POINTS];
    for (int i = 0; i < g->cad->selection.polygonCount; ++i) {
        int16_t polygon = g->cad->selection.selectedPolygons[i];
        int chain_count = polygon_point_indices(g->cad, polygon, chain, ARRAY_COUNT(chain));
        for (int p = 0; p < chain_count && count < capacity; ++p) {
            int duplicate = 0;
            for (int j = 0; j < count; ++j) duplicate |= result[j] == chain[p];
            if (!duplicate) result[count++] = chain[p];
        }
    }
    return count;
}

static int ensure_static_topology(GuiState* g, const char* action) {
    if (!g || !CadDocument_HasAnimation(&g->document)) return 1;
    gui_set_status(g,
                   "%s disabled while animation exists; use Make Static Copy from the timeline",
                   action ? action : "Topology change");
    return 0;
}

static void selection_center(const GuiState* g, const int16_t* points, int count,
                             double* x, double* y, double* z) {
    int valid = 0;
    *x = *y = *z = 0.0;
    if (!g || !points || count <= 0) return;
    for (int i = 0; i < count; ++i) {
        CadPosition point;
        if (!gui_scene_point(g, points[i], &point)) continue;
        *x += point.x;
        *y += point.y;
        *z += point.z;
        ++valid;
    }
    if (valid > 0) {
        *x /= valid; *y /= valid; *z /= valid;
    }
}

static const char* const state_field_labels[15] = {
    "Move X", "Move Y", "Move Z",
    "Rotate CX", "Rotate CY", "Rotate CZ",
    "Angle X", "Angle Y", "Angle Z",
    "Scale CX", "Scale CY", "Scale CZ",
    "Factor X", "Factor Y", "Factor Z"
};

static Rect state_field_rect(const GuiState* g, int field) {
    Rect inner = { g->stateWindow.r.x + 6, g->stateWindow.r.y + 26,
                   g->stateWindow.r.w - 12, g->stateWindow.r.h - 32 };
    int row = field / 3, col = field % 3;
    int block_width = (inner.w - 116) / 3;
    return (Rect){ inner.x + 110 + col * block_width,
                   inner.y + 40 + row * 45, block_width - 48, 24 };
}

static Rect state_minus_rect(const GuiState* g, int field) {
    Rect value = state_field_rect(g, field);
    return (Rect){ value.x + value.w + 3, value.y, 20, value.h };
}

static Rect state_plus_rect(const GuiState* g, int field) {
    Rect minus = state_minus_rect(g, field);
    return (Rect){ minus.x + minus.w + 2, minus.y, 20, minus.h };
}

static Rect state_apply_rect(const GuiState* g) {
    return (Rect){ g->stateWindow.r.x + g->stateWindow.r.w - 176,
                   g->stateWindow.r.y + g->stateWindow.r.h - 38, 78, 26 };
}

static Rect state_cancel_rect(const GuiState* g) {
    Rect apply = state_apply_rect(g);
    return (Rect){ apply.x + 86, apply.y, 78, apply.h };
}

static int state_parse_values(GuiState* g, double values[15]) {
    for (int i = 0; i < 15; ++i) {
        char* end = NULL;
        values[i] = strtod(g->state_values[i], &end);
        while (end && isspace((unsigned char)*end)) ++end;
        if (!g->state_values[i][0] || !end || *end || !isfinite(values[i])) {
            g->state_active_field = i;
            gui_set_status(g, "Invalid numeric value for %s", state_field_labels[i]);
            return 0;
        }
    }
    return 1;
}

static void state_set_value(GuiState* g, int field, double value) {
    if (!g || field < 0 || field >= 15) return;
    snprintf(g->state_values[field], sizeof(g->state_values[field]), "%.8g", value);
}

static void state_panel_defaults(GuiState* g) {
    int16_t points[CAD_MAX_POINTS];
    int count = collect_transform_points(g, g->state_face_target,
                                         points, ARRAY_COUNT(points));
    double x = 0.0, y = 0.0, z = 0.0;
    selection_center(g, points, count, &x, &y, &z);
    state_set_value(g, 0, 0.0); state_set_value(g, 1, 0.0); state_set_value(g, 2, 0.0);
    state_set_value(g, 3, x); state_set_value(g, 4, y); state_set_value(g, 5, z);
    state_set_value(g, 6, 0.0); state_set_value(g, 7, 0.0); state_set_value(g, 8, 0.0);
    state_set_value(g, 9, x); state_set_value(g, 10, y); state_set_value(g, 11, z);
    state_set_value(g, 12, 1.0); state_set_value(g, 13, 1.0); state_set_value(g, 14, 1.0);
}

static void state_panel_open(GuiState* g) {
    if (!g) return;
    /* Numeric transform defaults and pivots must come from a stored pose.
       Opening the modal panel is the beginning of that editing workflow, so
       pause playback and snap a fractional preview before sampling it. */
    CadAnimationSession_BeginEdit(&g->animation, g->animation_now);
    animation_update_scene(g, g->animation_now);
    g->state_face_target = g->cad->selection.polygonCount > 0 &&
                           (!g->cad->selectModeFlag || !g->cad->selection.pointCount);
    int selected = g->state_face_target ? g->cad->selection.polygonCount
                                        : g->cad->selection.pointCount;
    if (!selected) {
        gui_set_status(g, "Select points or faces before opening STATE / TenKey");
        return;
    }
    int available_width = g->layout_width > 20 ? g->layout_width - 20 : g->layout_width;
    int width = available_width < 620 ? available_width : 620;
    if (width < 430 && available_width >= 430) width = 430;
    int height = 350;
    int x = g->layout_width > width ? (g->layout_width - width) / 2 : 10;
    int y = g->layout_height > height ? (g->layout_height - height) / 2 : 30;
    if (y < MenuBarHeight() + 4) y = MenuBarHeight() + 4;
    g->stateWindow.r = (Rect){ x, y, width, height };
    g->state_visible = 1;
    g->state_active_field = 0;
    g->state_replace_on_input = 1;
    state_panel_defaults(g);
    gui_set_status(g, "STATE: edit values, then Apply; Enter applies and Escape cancels");
}

static void state_panel_close(GuiState* g) {
    if (!g) return;
    g->state_visible = 0;
    g->state_active_field = -1;
    if (g->drag_win == &g->stateWindow) g->drag_win = NULL;
    gui_set_status(g, "Numeric transform panel closed");
}

static void state_transform_point(CadPoint* point, const double v[15]) {
    double x, y, z, s, c, next;
    if (!point) return;
    point->pointx = v[9] + (point->pointx - v[9]) * v[12];
    point->pointy = v[10] + (point->pointy - v[10]) * v[13];
    point->pointz = v[11] + (point->pointz - v[11]) * v[14];

    x = point->pointx - v[3]; y = point->pointy - v[4]; z = point->pointz - v[5];
    s = sin(v[6] * M_PI / 180.0); c = cos(v[6] * M_PI / 180.0);
    next = y * c - z * s; z = y * s + z * c; y = next;
    s = sin(v[7] * M_PI / 180.0); c = cos(v[7] * M_PI / 180.0);
    next = x * c + z * s; z = -x * s + z * c; x = next;
    s = sin(v[8] * M_PI / 180.0); c = cos(v[8] * M_PI / 180.0);
    next = x * c - y * s; y = x * s + y * c; x = next;

    point->pointx = v[3] + x + v[0];
    point->pointy = v[4] + y + v[1];
    point->pointz = v[5] + z + v[2];
}

static int state_panel_apply(GuiState* g) {
    double values[15];
    int16_t points[CAD_MAX_POINTS];
    CadPoint samples[4];
    CadAffineTransform transform;
    CadResult result;
    if (!g || !state_parse_values(g, values)) return 0;
    int point_count = collect_transform_points(g, g->state_face_target,
                                               points, ARRAY_COUNT(points));
    if (!point_count) {
        gui_set_status(g, "The STATE target selection is no longer available");
        return 0;
    }
    memset(samples, 0, sizeof(samples));
    samples[1].pointx = 1.0;
    samples[2].pointy = 1.0;
    samples[3].pointz = 1.0;
    for (int i = 0; i < 4; ++i) state_transform_point(&samples[i], values);
    memset(&transform, 0, sizeof(transform));
    transform.translation.x = samples[0].pointx;
    transform.translation.y = samples[0].pointy;
    transform.translation.z = samples[0].pointz;
    transform.matrix[0][0] = samples[1].pointx - samples[0].pointx;
    transform.matrix[1][0] = samples[1].pointy - samples[0].pointy;
    transform.matrix[2][0] = samples[1].pointz - samples[0].pointz;
    transform.matrix[0][1] = samples[2].pointx - samples[0].pointx;
    transform.matrix[1][1] = samples[2].pointy - samples[0].pointy;
    transform.matrix[2][1] = samples[2].pointz - samples[0].pointz;
    transform.matrix[0][2] = samples[3].pointx - samples[0].pointx;
    transform.matrix[1][2] = samples[3].pointy - samples[0].pointy;
    transform.matrix[2][2] = samples[3].pointz - samples[0].pointz;
    CadAnimationSession_BeginEdit(&g->animation, g->animation_now);
    if (!history_push_named(g, "State Transform")) return 0;
    result = CadAnimation_Transform(
        &g->cad->data, g->animation.currentFrame,
        g->animation.allFrames ? CAD_ANIMATION_ALL_FRAMES
                               : CAD_ANIMATION_CURRENT_FRAME,
        points, (size_t)point_count, &transform);
    if (!CadResult_IsSuccess(&result)) {
        history_cancel(g);
        gui_set_status(g, "STATE transform failed: %s",
                       cad_result_message(&result));
        return 0;
    }
    if (!history_commit(g)) return 0;
    gui_set_status(g, "Applied numeric transform to %d point(s) in %s",
                   point_count, g->animation.allFrames ? "all frames"
                                                       : "the current frame");
    state_panel_defaults(g);
    g->state_active_field = 0;
    g->state_replace_on_input = 1;
    return 1;
}

static void state_adjust_field(GuiState* g, int field, int direction) {
    if (!g || field < 0 || field >= 15) return;
    char* end = NULL;
    double value = strtod(g->state_values[field], &end);
    if (!end || *end || !isfinite(value)) value = (field >= 12 ? 1.0 : 0.0);
    double step = field >= 12 ? 0.1 : (field >= 6 && field <= 8 ? 1.0 : 1.0);
    state_set_value(g, field, value + direction * step);
    g->state_active_field = field;
    g->state_replace_on_input = 0;
}

static void state_panel_key(GuiState* g, int key, unsigned modifiers) {
    (void)modifiers;
    if (!g || !g->state_visible) return;
    if (key == GUI_KEY_ESCAPE) { state_panel_close(g); return; }
    if (key == GUI_KEY_ENTER) { state_panel_apply(g); return; }
    if (key == '\t') {
        g->state_active_field = (g->state_active_field + 1) % 15;
        g->state_replace_on_input = 1;
        return;
    }
    if (g->state_active_field < 0) g->state_active_field = 0;
    char* value = g->state_values[g->state_active_field];
    size_t length = strlen(value);
    if (key == GUI_KEY_BACKSPACE || key == GUI_KEY_DELETE) {
        if (g->state_replace_on_input) value[0] = '\0';
        else if (length) value[length - 1] = '\0';
        g->state_replace_on_input = 0;
        return;
    }
    if ((key >= '0' && key <= '9') || key == '.' || key == '-' || key == '+' ||
        key == 'e') {
        if (g->state_replace_on_input) {
            value[0] = '\0';
            length = 0;
            g->state_replace_on_input = 0;
        }
        if (length + 1 < sizeof(g->state_values[0])) {
            value[length] = (char)key;
            value[length + 1] = '\0';
        }
    }
}

static void rebuild_polygon_selection(CadCore* core) {
    if (!core) return;
    core->selection.polygonCount = 0;
    for (int i = 0; i < CAD_MAX_POLYGONS; ++i) core->selection.selectedPolygons[i] = INVALID_INDEX;
    for (int i = 0; i < core->data.polygonCount; ++i) {
        if (core->data.polygons[i].flags && core->data.polygons[i].selectFlag) {
            core->selection.selectedPolygons[core->selection.polygonCount++] = (int16_t)i;
        }
    }
}

static void editor_copy(GuiState* g) {
    if (!g || !g->clipboard) return;
    if (g->cad->selection.pointCount == 0 && g->cad->selection.polygonCount == 0) {
        gui_set_status(g, "Nothing selected to copy");
        return;
    }
    *g->clipboard = *g->cad;
    g->clipboard_has_data = 1;
    gui_set_status(g, "Copied %d point(s), %d face(s)",
                   g->cad->selection.pointCount, g->cad->selection.polygonCount);
}

static void editor_paste(GuiState* g) {
    if (!g || !g->clipboard || !g->clipboard_has_data) {
        gui_set_status(g, "Clipboard is empty");
        return;
    }
    if (!ensure_static_topology(g, "Paste")) return;
    if (!history_push_named(g, "Paste")) return;
    CadCore_ClearSelection(g->cad);
    int made = 0;
    int failed = 0;
    if (g->clipboard->selection.polygonCount > 0) {
        for (int i = 0; i < g->clipboard->selection.polygonCount; ++i) {
            int16_t created = duplicate_polygon_from_core(g->cad, g->clipboard,
                g->clipboard->selection.selectedPolygons[i], 10.0, 0, 0.0, 0);
            if (created == INVALID_INDEX) { failed = 1; break; }
            CadCore_SelectPolygon(g->cad, created);
            ++made;
        }
    } else {
        for (int i = 0; i < g->clipboard->selection.pointCount; ++i) {
            int16_t source = g->clipboard->selection.selectedPoints[i];
            if (!CadCore_IsPointValid(g->clipboard, source)) continue;
            CadPoint* p = &g->clipboard->data.points[source];
            int16_t created = CadCore_AddPoint(g->cad, p->pointx + 10.0, p->pointy, p->pointz);
            if (created == INVALID_INDEX) { failed = 1; break; }
            CadCore_SelectPoint(g->cad, created);
            ++made;
        }
    }
    if (failed) {
        history_cancel(g);
        gui_set_status(g, "Paste cancelled: model capacity reached; document unchanged");
        return;
    }
    if (!history_commit(g)) return;
    gui_set_status(g, "Pasted %d item(s), offset +10 on X", made);
}

static void editor_change_first_point(GuiState* g) {
    if (!g || g->cad->selection.polygonCount == 0) {
        gui_set_status(g, "Select one or more faces first");
        return;
    }
    if (!ensure_static_topology(g, "Change First Point")) return;
    if (!history_push_named(g, "Change First Point")) return;
    int changed = 0;
    int16_t chain[CAD_MAX_FACE_POINTS];
    for (int i = 0; i < g->cad->selection.polygonCount; ++i) {
        int16_t polygon_index = g->cad->selection.selectedPolygons[i];
        int count = polygon_point_indices(g->cad, polygon_index, chain, ARRAY_COUNT(chain));
        if (count < 2) continue;
        g->cad->data.polygons[polygon_index].firstPoint = chain[1];
        for (int p = 1; p < count - 1; ++p) g->cad->data.points[chain[p]].nextPoint = chain[p + 1];
        g->cad->data.points[chain[count - 1]].nextPoint = chain[0];
        g->cad->data.points[chain[0]].nextPoint = INVALID_INDEX;
        ++changed;
    }
    if (changed) g->cad->isDirty = 1;
    if (!history_commit(g)) return;
    gui_set_status(g, "Changed first point on %d face(s)", changed);
}

static int polygon_is_flat(const CadCore* core, int16_t polygon_index) {
    int16_t chain[CAD_MAX_FACE_POINTS];
    double coordinates[CAD_MAX_FACE_POINTS][3];
    int count = polygon_point_indices(core, polygon_index, chain, ARRAY_COUNT(chain));
    if (count < 2) return 0;
    for (int i = 0; i < count; ++i) {
        const CadPoint* p = &core->data.points[chain[i]];
        coordinates[i][0] = p->pointx;
        coordinates[i][1] = p->pointy;
        coordinates[i][2] = p->pointz;
    }
    return CadGeometry_ClassifyPointChain(coordinates, count, 0.01) ==
           CAD_POINT_CHAIN_COPLANAR;
}

static void editor_flat_check(GuiState* g) {
    int checked = 0, nonflat = 0;
    for (int i = 0; i < g->cad->data.polygonCount; ++i) {
        if (!CadCore_IsPolygonValid(g->cad, (int16_t)i)) continue;
        if (g->cad->selection.polygonCount && !CadCore_IsPolygonSelected(g->cad, (int16_t)i)) continue;
        ++checked;
        nonflat += !polygon_is_flat(g->cad, (int16_t)i);
    }
    gui_set_status(g, "Flat Check: %d checked, %d non-coplanar", checked, nonflat);
}

static void reverse_polygon(CadCore* core, int16_t polygon_index) {
    int16_t chain[CAD_MAX_FACE_POINTS];
    int count = polygon_point_indices(core, polygon_index, chain, ARRAY_COUNT(chain));
    if (count < 2) return;
    for (int i = count - 1; i > 0; --i) core->data.points[chain[i]].nextPoint = chain[i - 1];
    core->data.points[chain[0]].nextPoint = INVALID_INDEX;
    core->data.polygons[polygon_index].firstPoint = chain[count - 1];
    core->data.polygons[polygon_index].side ^= 1;
    core->isDirty = 1;
}

static void editor_face_support(GuiState* g) {
    if (!g || g->cad->selection.polygonCount == 0) {
        gui_set_status(g, "Select faces before creating reverse sides");
        return;
    }
    activate_tool(g, CAD_TOOL_FACE_SIDE);
}

static void editor_face_information(GuiState* g) {
    if (!g || g->cad->selection.polygonCount == 0) {
        gui_set_status(g, "No faces selected");
        return;
    }
    int first = g->cad->selection.selectedPolygons[0];
    CadPolygon* polygon = CadCore_GetPolygon(g->cad, (int16_t)first);
    if (!polygon) return;
    gui_set_status(g, "%d face(s); first #%d: %u points, color %u, side %u, pair %d",
                   g->cad->selection.polygonCount, first, polygon->npoints,
                   polygon->color, polygon->side, polygon->both);
}

static void editor_grid_merge(GuiState* g) {
    if (!g) return;
    if (!ensure_static_topology(g, "Grid Merge")) return;
    int changed = 0;
    for (int i = 0; i < g->cad->data.pointCount; ++i) {
        CadPoint* p = &g->cad->data.points[i];
        if (!p->flags) continue;
        double x = (double)CadCore_ConvertCoordinate(p->pointx);
        double y = (double)CadCore_ConvertCoordinate(p->pointy);
        double z = (double)CadCore_ConvertCoordinate(p->pointz);
        changed += x != p->pointx || y != p->pointy || z != p->pointz;
    }
    if (!changed) { gui_set_status(g, "Grid Merge: coordinates already integral"); return; }
    if (!history_push_named(g, "Grid Merge")) return;
    for (int i = 0; i < g->cad->data.pointCount; ++i) {
        CadPoint* p = &g->cad->data.points[i];
        if (!p->flags) continue;
        p->pointx = (double)CadCore_ConvertCoordinate(p->pointx);
        p->pointy = (double)CadCore_ConvertCoordinate(p->pointy);
        p->pointz = (double)CadCore_ConvertCoordinate(p->pointz);
    }
    g->cad->isDirty = 1;
    if (!history_commit(g)) return;
    gui_set_status(g, "Grid Merge: rounded %d point(s)", changed);
}

static void editor_point_merge(GuiState* g) {
    if (!ensure_static_topology(g, "Point Merge")) return;
    if (CadCore_ArePointsMerged(g->cad)) {
        gui_set_status(g, "Point Merge: polygon chains are already merged");
        return;
    }
    if (!history_push_named(g, "Point Merge")) return;
    int merged = CadCore_MergePolygonPoints(g->cad);
    if (!merged) {
        history_cancel(g);
        gui_set_status(g, "Point Merge: no mergeable linked-chain vertices");
        return;
    }
    if (!history_commit(g)) return;
    gui_set_status(g, "Point Merge: removed %d redundant chain point(s)", merged);
}

static int polygons_equal(const CadCore* core, int16_t left, int16_t right) {
    int16_t a[CAD_MAX_FACE_POINTS], b[CAD_MAX_FACE_POINTS];
    /* Reciprocal faces intentionally describe the same boundary in opposite
       directions so each side can carry its own color.  They are a valid
       pair, not duplicate geometry for Polygon/All Merge to discard. */
    if (core->data.polygons[left].both == right &&
        core->data.polygons[right].both == left) {
        return 0;
    }
    int ac = polygon_point_indices(core, left, a, ARRAY_COUNT(a));
    int bc = polygon_point_indices(core, right, b, ARRAY_COUNT(b));
    if (ac != bc || ac < 2) return 0;
    for (int direction = 0; direction < 2; ++direction) {
        for (int offset = 0; offset < ac; ++offset) {
            int match = 1;
            for (int i = 0; i < ac; ++i) {
                int bi = direction ? (offset - i + ac) % ac : (offset + i) % ac;
                const CadPoint* pa = &core->data.points[a[i]];
                const CadPoint* pb = &core->data.points[b[bi]];
                if (fabs(pa->pointx - pb->pointx) > 0.0001 ||
                    fabs(pa->pointy - pb->pointy) > 0.0001 ||
                    fabs(pa->pointz - pb->pointz) > 0.0001) { match = 0; break; }
            }
            if (match) return 1;
        }
    }
    return 0;
}

static void editor_polygon_merge(GuiState* g) {
    if (!ensure_static_topology(g, "Polygon Merge")) return;
    int duplicates = 0;
    for (int i = 0; i < g->cad->data.polygonCount; ++i) {
        if (!CadCore_IsPolygonValid(g->cad, (int16_t)i)) continue;
        for (int j = i + 1; j < g->cad->data.polygonCount; ++j) {
            if (CadCore_IsPolygonValid(g->cad, (int16_t)j) && polygons_equal(g->cad, (int16_t)i, (int16_t)j)) {
                ++duplicates;
            }
        }
    }
    if (!duplicates) { gui_set_status(g, "Polygon Merge: no duplicate faces"); return; }
    if (!history_push_named(g, "Polygon Merge")) return;
    int removed = 0;
    for (int i = 0; i < g->cad->data.polygonCount; ++i) {
        if (!CadCore_IsPolygonValid(g->cad, (int16_t)i)) continue;
        for (int j = i + 1; j < g->cad->data.polygonCount; ++j) {
            if (CadCore_IsPolygonValid(g->cad, (int16_t)j) && polygons_equal(g->cad, (int16_t)i, (int16_t)j)) {
                delete_polygon_geometry(g->cad, (int16_t)j);
                ++removed;
            }
        }
    }
    if (!history_commit(g)) return;
    gui_set_status(g, "Polygon Merge: removed %d duplicate face(s)", removed);
}

static void editor_merge_all(GuiState* g) {
    int rounded = 0;
    int merged = 0;
    int removed = 0;
    if (!g || !ensure_static_topology(g, "All Merge")) return;
    if (!history_push_named(g, "All Merge")) return;

    for (int i = 0; i < g->cad->data.pointCount; ++i) {
        CadPoint* point = &g->cad->data.points[i];
        double x, y, z;
        if (!point->flags) continue;
        x = (double)CadCore_ConvertCoordinate(point->pointx);
        y = (double)CadCore_ConvertCoordinate(point->pointy);
        z = (double)CadCore_ConvertCoordinate(point->pointz);
        if (x != point->pointx || y != point->pointy || z != point->pointz)
            ++rounded;
        point->pointx = x; point->pointy = y; point->pointz = z;
    }
    merged = CadCore_MergePolygonPoints(g->cad);
    for (int i = 0; i < g->cad->data.polygonCount; ++i) {
        if (!CadCore_IsPolygonValid(g->cad, (int16_t)i)) continue;
        for (int j = i + 1; j < g->cad->data.polygonCount; ++j) {
            if (CadCore_IsPolygonValid(g->cad, (int16_t)j) &&
                polygons_equal(g->cad, (int16_t)i, (int16_t)j)) {
                delete_polygon_geometry(g->cad, (int16_t)j);
                ++removed;
            }
        }
    }
    if (!rounded && !merged && !removed) {
        history_cancel(g);
        gui_set_status(g, "All Merge: model is already fully merged");
        return;
    }
    g->cad->isDirty = 1;
    if (!history_commit(g)) return;
    gui_set_status(g,
                   "All Merge: rounded %d point(s), merged %d chain point(s), removed %d face(s)",
                   rounded, merged, removed);
}

static void editor_polygon_sort(GuiState* g) {
    if (!ensure_static_topology(g, "Polygon Sort")) return;
    int ids[CAD_MAX_POLYGONS], count = 0;
    for (int i = 0; i < g->cad->data.polygonCount; ++i) {
        if (g->cad->data.polygons[i].flags) ids[count++] = i;
    }
    if (count < 2) { gui_set_status(g, "Polygon Sort: fewer than two faces"); return; }
    for (int i = 1; i < count; ++i) {
        int value = ids[i], j = i - 1;
        while (j >= 0 && (g->cad->data.polygons[ids[j]].color > g->cad->data.polygons[value].color ||
               (g->cad->data.polygons[ids[j]].color == g->cad->data.polygons[value].color && ids[j] > value))) {
            ids[j + 1] = ids[j]; --j;
        }
        ids[j + 1] = value;
    }
    if (!history_push_named(g, "Polygon Sort")) return;
    CadPolygon sorted[CAD_MAX_POLYGONS];
    int16_t map[CAD_MAX_POLYGONS];
    memset(sorted, 0, sizeof(sorted));
    for (int i = 0; i < CAD_MAX_POLYGONS; ++i) map[i] = INVALID_INDEX;
    for (int i = 0; i < count; ++i) { sorted[i] = g->cad->data.polygons[ids[i]]; map[ids[i]] = (int16_t)i; }
    for (int i = 0; i < count; ++i) {
        if (sorted[i].nextPolygon >= 0) sorted[i].nextPolygon = map[sorted[i].nextPolygon];
        if (sorted[i].both >= 0) sorted[i].both = map[sorted[i].both];
    }
    memcpy(g->cad->data.polygons, sorted, sizeof(sorted));
    g->cad->data.polygonCount = count;
    for (int i = 0; i < g->cad->data.objectCount; ++i) {
        CadObject* object = &g->cad->data.objects[i];
        if (object->flags && object->firstPolygon >= 0) object->firstPolygon = map[object->firstPolygon];
    }
    rebuild_polygon_selection(g->cad);
    g->cad->isDirty = 1;
    if (!history_commit(g)) return;
    gui_set_status(g, "Polygon Sort: ordered %d faces by color", count);
}

static double point_segment_distance(double px, double py, double ax, double ay,
                                     double bx, double by) {
    double dx = bx - ax, dy = by - ay;
    double length2 = dx * dx + dy * dy;
    double t = length2 > 0.0 ? ((px - ax) * dx + (py - ay) * dy) / length2 : 0.0;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    dx = px - (ax + t * dx);
    dy = py - (ay + t * dy);
    return sqrt(dx * dx + dy * dy);
}

static int16_t find_polygon_at(GuiState* g, int view_index, int screen_x, int screen_y,
                               int* nearest_edge) {
    Rect content = view_content_rect(g, view_index);
    int16_t polygon = gui_find_nearest_polygon(g, view_index, screen_x,
                                                screen_y, content, 8);
    if (nearest_edge) *nearest_edge = -1;
    if (polygon == INVALID_INDEX || !nearest_edge) return polygon;

    int16_t chain[CAD_MAX_FACE_POINTS];
    int count = polygon_point_indices(g->cad, polygon, chain, ARRAY_COUNT(chain));
    int px[CAD_MAX_FACE_POINTS], py[CAD_MAX_FACE_POINTS];
    double best_edge = HUGE_VAL;
    if (count < 2) return INVALID_INDEX;
    for (int i = 0; i < count; ++i) {
        CadPosition point;
        if (!gui_scene_point(g, chain[i], &point)) return INVALID_INDEX;
        CadView_ProjectPoint(&g->views[view_index], point.x, point.y, point.z,
                             &px[i], &py[i], content.w, content.h);
        px[i] += content.x;
        py[i] += content.y;
    }
    for (int i = 0; i < (count == 2 ? 1 : count); ++i) {
        int next = (i + 1) % count;
        double distance = point_segment_distance(screen_x, screen_y,
                                                 px[i], py[i], px[next], py[next]);
        if (distance < best_edge) {
            best_edge = distance;
            *nearest_edge = i;
        }
    }
    return polygon;
}

static void update_face_creation_status(GuiState* g, int view_index) {
    double coords[CAD_MAX_FACE_POINTS][3];
    int count;
    (void)view_index;
    if (!g || !g->cad) return;
    count = g->cad->selection.pointCount;
    if (count <= 0) {
        gui_set_status(g, "Face: choose 2-%d ordered points; Enter finishes, Escape cancels",
                       CAD_MAX_FACE_POINTS);
        return;
    }
    if (count > CAD_MAX_FACE_POINTS) count = CAD_MAX_FACE_POINTS;
    for (int i = 0; i < count; ++i) {
        int16_t index = g->cad->selection.selectedPoints[i];
        const CadPoint* point = CadCore_IsPointValid(g->cad, index)
                                ? &g->cad->data.points[index] : NULL;
        if (!point) return;
        coords[i][0] = point->pointx;
        coords[i][1] = point->pointy;
        coords[i][2] = point->pointz;
    }
    if (count == 1) {
        gui_set_status(g, "Face: vertex 1 set; choose the next point (Backspace removes it)");
    } else if (count == 2) {
        CadPointChainPlanarity planarity =
            CadGeometry_ClassifyPointChain(coords, count, 0.01);
        if (planarity == CAD_POINT_CHAIN_COPLANAR)
            gui_set_status(g, "Face: 2 vertices form a colored line; Enter/right-click finishes");
        else
            gui_set_status(g, "Face: degenerate line (coincident endpoints); choose a different point");
    } else {
        double nx = 0.0, ny = 0.0, nz = 0.0;
        CadPointChainPlanarity planarity =
            CadGeometry_ClassifyPointChain(coords, count, 0.01);
        const char* normal_axis;
        for (int i = 0; i < count; ++i) {
            int next = (i + 1) % count;
            nx += (coords[i][1] - coords[next][1]) * (coords[i][2] + coords[next][2]);
            ny += (coords[i][2] - coords[next][2]) * (coords[i][0] + coords[next][0]);
            nz += (coords[i][0] - coords[next][0]) * (coords[i][1] + coords[next][1]);
        }
        if (planarity == CAD_POINT_CHAIN_DEGENERATE) {
            gui_set_status(g, "Face: %d/%d vertices, degenerate (coincident or collinear); Enter will reject",
                           count, CAD_MAX_FACE_POINTS);
            return;
        }
        if (fabs(nx) >= fabs(ny) && fabs(nx) >= fabs(nz)) normal_axis = nx < 0.0 ? "-X" : "+X";
        else if (fabs(ny) >= fabs(nz)) normal_axis = ny < 0.0 ? "-Y" : "+Y";
        else normal_axis = nz < 0.0 ? "-Z" : "+Z";
        gui_set_status(g, "Face: %d/%d vertices, %s, winding normal %s; Enter finishes",
                       count, CAD_MAX_FACE_POINTS,
                       planarity == CAD_POINT_CHAIN_COPLANAR
                           ? "coplanar" : "NOT coplanar",
                       normal_axis);
    }
}

static int create_face_from_selection(GuiState* g) {
    if (!ensure_static_topology(g, "Make Face")) return 0;
    int count = g->cad->selection.pointCount;
    if (count < CAD_MIN_FACE_POINTS || count > CAD_MAX_FACE_POINTS) {
        gui_set_status(g, "A face needs 2-%d ordered points", CAD_MAX_FACE_POINTS);
        return 0;
    }
    double coords[CAD_MAX_FACE_POINTS][3];
    for (int i = 0; i < count; ++i) {
        int16_t index = g->cad->selection.selectedPoints[i];
        if (!CadCore_IsPointValid(g->cad, index)) return 0;
        CadPoint* point = &g->cad->data.points[index];
        coords[i][0] = point->pointx; coords[i][1] = point->pointy; coords[i][2] = point->pointz;
    }
    CadPointChainPlanarity planarity =
        CadGeometry_ClassifyPointChain(coords, count, 0.01);
    if (planarity == CAD_POINT_CHAIN_DEGENERATE) {
        gui_set_status(g, count == 2
                           ? "Face rejected: colored-line endpoints coincide"
                           : "Face rejected: selected points are coincident or collinear");
        return 0;
    }
    if (planarity != CAD_POINT_CHAIN_COPLANAR) {
        gui_set_status(g, "Face rejected: selected points are not coplanar");
        return 0;
    }
    if (!history_push(g)) return 0;
    int16_t polygon = create_polygon_from_coordinates(g->cad, coords, count, 0, 0, 0);
    CadCore_ClearSelection(g->cad);
    if (polygon == INVALID_INDEX) {
        history_cancel(g);
        gui_set_status(g, "Face creation failed: model capacity reached");
        return 0;
    }
    CadCore_SelectPolygon(g->cad, polygon);
    if (!history_commit(g)) return 0;
    gui_set_status(g, count == 2 ? "Created colored line #%d" : "Created face #%d with %d points",
                   polygon, count);
    return 1;
}

static void insert_point_on_edge(GuiState* g, int16_t polygon_index, int edge_index) {
    if (!ensure_static_topology(g, "Insert Point")) return;
    int16_t chain[CAD_MAX_FACE_POINTS];
    int count = polygon_point_indices(g->cad, polygon_index, chain, ARRAY_COUNT(chain));
    if (count < 2 || count >= CAD_MAX_FACE_POINTS || edge_index < 0) {
        gui_set_status(g, count >= CAD_MAX_FACE_POINTS ? "Face already has the maximum point count" : "No edge found");
        return;
    }
    int next_index = count == 2 ? 1 : (edge_index + 1) % count;
    int current_index = edge_index % count;
    CadPoint* a = &g->cad->data.points[chain[current_index]];
    CadPoint* b = &g->cad->data.points[chain[next_index]];
    if (!history_push(g)) return;
    int16_t inserted = CadCore_AddPoint(g->cad,
        (a->pointx + b->pointx) * 0.5, (a->pointy + b->pointy) * 0.5,
        (a->pointz + b->pointz) * 0.5);
    if (inserted == INVALID_INDEX) { history_cancel(g); gui_set_status(g, "No point capacity left"); return; }
    g->cad->data.points[inserted].nextPoint = a->nextPoint;
    a->nextPoint = inserted;
    g->cad->data.polygons[polygon_index].npoints++;
    g->cad->isDirty = 1;
    if (!history_commit(g)) return;
    gui_set_status(g, "Inserted point #%d on face #%d", inserted, polygon_index);
}

static void remove_selected_points(GuiState* g) {
    if (g->cad->selection.pointCount == 0) { gui_set_status(g, "Select points to delete"); return; }
    if (!ensure_static_topology(g, "Delete Points")) return;
    int16_t selected[CAD_MAX_POINTS];
    int selected_count = g->cad->selection.pointCount;
    memcpy(selected, g->cad->selection.selectedPoints, sizeof(int16_t) * selected_count);
    if (!history_push(g)) return;
    int affected_faces = 0;
    for (int p = 0; p < g->cad->data.polygonCount; ++p) {
        if (!CadCore_IsPolygonValid(g->cad, (int16_t)p)) continue;
        int16_t chain[CAD_MAX_FACE_POINTS], kept[CAD_MAX_FACE_POINTS];
        int count = polygon_point_indices(g->cad, (int16_t)p, chain, ARRAY_COUNT(chain));
        int kept_count = 0;
        for (int i = 0; i < count; ++i) {
            int remove = 0;
            for (int s = 0; s < selected_count; ++s) remove |= chain[i] == selected[s];
            if (!remove) kept[kept_count++] = chain[i];
        }
        if (kept_count == count) continue;
        ++affected_faces;
        if (kept_count < CAD_MIN_FACE_POINTS) {
            delete_polygon_geometry(g->cad, (int16_t)p);
        } else {
            g->cad->data.polygons[p].firstPoint = kept[0];
            g->cad->data.polygons[p].npoints = (uint8_t)kept_count;
            for (int i = 0; i < kept_count; ++i) {
                g->cad->data.points[kept[i]].nextPoint = i + 1 < kept_count ? kept[i + 1] : INVALID_INDEX;
            }
        }
    }
    int deleted = 0;
    for (int i = 0; i < selected_count; ++i) deleted += CadCore_DeletePoint(g->cad, selected[i]);
    CadCore_ClearSelection(g->cad);
    if (!history_commit(g)) return;
    gui_set_status(g, "Deleted %d point(s); updated %d face(s)", deleted, affected_faces);
}

static void remove_selected_faces(GuiState* g) {
    if (g->cad->selection.polygonCount == 0) { gui_set_status(g, "Select faces to delete"); return; }
    if (!ensure_static_topology(g, "Delete Faces")) return;
    int16_t selected[CAD_MAX_POLYGONS];
    int count = g->cad->selection.polygonCount;
    memcpy(selected, g->cad->selection.selectedPolygons, sizeof(int16_t) * count);
    if (!history_push(g)) return;
    for (int i = 0; i < count; ++i) delete_polygon_geometry(g->cad, selected[i]);
    CadCore_ClearSelection(g->cad);
    if (!history_commit(g)) return;
    gui_set_status(g, "Deleted %d face(s)", count);
}

static void flip_selected_points(GuiState* g) {
    int16_t points[CAD_MAX_POINTS];
    CadAffineTransform transform;
    CadResult result;
    int count = collect_transform_points(g, 0, points, ARRAY_COUNT(points));
    if (!count) { gui_set_status(g, "Select points to flip"); return; }
    CadAnimationSession_BeginEdit(&g->animation, g->animation_now);
    animation_update_scene(g, g->animation_now);
    double cx, cy, cz;
    selection_center(g, points, count, &cx, &cy, &cz);
    (void)cy; (void)cz;
    memset(&transform, 0, sizeof(transform));
    transform.matrix[0][0] = -1.0;
    transform.matrix[1][1] = 1.0;
    transform.matrix[2][2] = 1.0;
    transform.pivot.x = cx;
    transform.pivot.y = cy;
    transform.pivot.z = cz;
    if (!history_push(g)) return;
    result = CadAnimation_Transform(
        &g->cad->data, g->animation.currentFrame,
        g->animation.allFrames ? CAD_ANIMATION_ALL_FRAMES
                               : CAD_ANIMATION_CURRENT_FRAME,
        points, (size_t)count, &transform);
    if (!CadResult_IsSuccess(&result)) {
        history_cancel(g);
        gui_set_status(g, "Flip failed: %s", cad_result_message(&result));
        return;
    }
    if (!history_commit(g)) return;
    gui_set_status(g, "Flipped %d point(s) across X=%.2f in %s", count, cx,
                   g->animation.allFrames ? "all frames" : "the current frame");
}

static void copy_selected_faces(GuiState* g, int mirror) {
    int count = g->cad->selection.polygonCount;
    if (!count) { gui_set_status(g, "Select faces first"); return; }
    if (!ensure_static_topology(g, mirror ? "Mirror Faces" : "Copy Faces")) return;
    int16_t selected[CAD_MAX_POLYGONS];
    memcpy(selected, g->cad->selection.selectedPolygons, sizeof(int16_t) * count);
    double mirror_center = 0.0, cy = 0.0, cz = 0.0;
    int16_t points[CAD_MAX_POINTS];
    int point_count = collect_transform_points(g, 1, points, ARRAY_COUNT(points));
    selection_center(g, points, point_count, &mirror_center, &cy, &cz);
    (void)cy; (void)cz;
    if (!history_push(g)) return;
    CadCore_ClearSelection(g->cad);
    int made = 0;
    for (int i = 0; i < count; ++i) {
        int16_t result = duplicate_polygon_from_core(g->cad, g->cad, selected[i],
                                                     mirror ? 0.0 : 10.0,
                                                     mirror, mirror_center, mirror);
        if (result == INVALID_INDEX) {
            history_cancel(g);
            gui_set_status(g, "%s cancelled: model capacity reached; document unchanged",
                           mirror ? "Mirror" : "Copy");
            return;
        }
        CadCore_SelectPolygon(g->cad, result);
        ++made;
    }
    if (!history_commit(g)) return;
    gui_set_status(g, "%s %d face(s)%s", mirror ? "Mirrored" : "Copied", made,
                   mirror ? "" : " with +10 X offset");
}

static void reverse_selected_faces(GuiState* g) {
    if (!g->cad->selection.polygonCount) { gui_set_status(g, "Select faces to reverse"); return; }
    if (!ensure_static_topology(g, "Reverse Face")) return;
    if (!history_push(g)) return;
    for (int i = 0; i < g->cad->selection.polygonCount; ++i) {
        reverse_polygon(g->cad, g->cad->selection.selectedPolygons[i]);
    }
    if (!history_commit(g)) return;
    gui_set_status(g, "Reversed winding on %d face(s)", g->cad->selection.polygonCount);
}

static void cut_selected_faces(GuiState* g) {
    int selected_count = g->cad->selection.polygonCount;
    if (!selected_count) { gui_set_status(g, "Select polygon faces to cut"); return; }
    if (!ensure_static_topology(g, "Cut Faces")) return;
    int16_t selected[CAD_MAX_POLYGONS];
    memcpy(selected, g->cad->selection.selectedPolygons, sizeof(int16_t) * selected_count);
    if (!history_push(g)) return;
    int cut = 0, triangles = 0;
    int failed = 0;
    CadCore_ClearSelection(g->cad);
    for (int s = 0; s < selected_count; ++s) {
        int16_t chain[CAD_MAX_FACE_POINTS];
        int count = polygon_point_indices(g->cad, selected[s], chain, ARRAY_COUNT(chain));
        if (count <= 3) continue;
        double source[CAD_MAX_FACE_POINTS][3];
        CadPolygon original = g->cad->data.polygons[selected[s]];
        for (int i = 0; i < count; ++i) {
            CadPoint* p = &g->cad->data.points[chain[i]];
            source[i][0] = p->pointx; source[i][1] = p->pointy; source[i][2] = p->pointz;
        }
        int made = 0;
        for (int i = 1; i < count - 1; ++i) {
            double triangle[3][3];
            memcpy(triangle[0], source[0], sizeof(triangle[0]));
            memcpy(triangle[1], source[i], sizeof(triangle[1]));
            memcpy(triangle[2], source[i + 1], sizeof(triangle[2]));
            int16_t result = create_polygon_from_coordinates(g->cad, triangle, 3,
                                                              original.color, original.side, 0);
            if (result == INVALID_INDEX) { failed = 1; break; }
            CadCore_SelectPolygon(g->cad, result);
            ++made;
        }
        if (failed) break;
        if (made == count - 2) {
            delete_polygon_geometry(g->cad, selected[s]);
            ++cut; triangles += made;
        }
    }
    if (failed) {
        history_cancel(g);
        gui_set_status(g, "Cut cancelled: model capacity reached; document unchanged");
        return;
    }
    if (!history_commit(g)) return;
    gui_set_status(g, "Cut %d face(s) into %d triangle(s)", cut, triangles);
}

static void side_selected_faces(GuiState* g) {
    int count = g->cad->selection.polygonCount;
    if (!count) { gui_set_status(g, "Select faces before creating reverse sides"); return; }
    if (!ensure_static_topology(g, "Face Side")) return;
    int16_t selected[CAD_MAX_POLYGONS];
    memcpy(selected, g->cad->selection.selectedPolygons, sizeof(int16_t) * count);
    if (!history_push(g)) return;
    int made = 0;
    for (int i = 0; i < count; ++i) {
        if (!CadCore_IsPolygonValid(g->cad, selected[i])) continue;
        int16_t reverse = duplicate_polygon_from_core(g->cad, g->cad, selected[i], 0.0, 0, 0.0, 1);
        if (reverse == INVALID_INDEX) {
            history_cancel(g);
            gui_set_status(g, "Face Side cancelled: model capacity reached; document unchanged");
            return;
        }
        g->cad->data.polygons[selected[i]].both = reverse;
        g->cad->data.polygons[reverse].both = selected[i];
        g->cad->data.polygons[reverse].side = g->cad->data.polygons[selected[i]].side ^ 1;
        ++made;
    }
    if (!history_commit(g)) return;
    gui_set_status(g, "Created %d paired reverse side(s)", made);
}

static int tool_needs_points(CadToolId tool) {
    return tool == CAD_TOOL_POINT_MOVE || tool == CAD_TOOL_POINT_ROTATE ||
           tool == CAD_TOOL_POINT_SCALE;
}

static int tool_needs_faces(CadToolId tool) {
    return tool == CAD_TOOL_FACE_MOVE || tool == CAD_TOOL_FACE_ROTATE ||
           tool == CAD_TOOL_FACE_SCALE;
}

static int tool_is_transform(CadToolId tool) {
    return tool_needs_points(tool) || tool_needs_faces(tool);
}

static void activate_tool(GuiState* g, CadToolId tool) {
    if (!g || tool < 0 || tool >= CAD_TOOL_COUNT) return;
    const CadToolDescriptor* descriptor = &toolDescriptors[tool];
    EditorCommandContext context = editor_command_context(g);
    CadCommandState state = EditorController_GetToolState(
        &g->controller, tool, &context);
    reset_interaction(g);
    if (!state.enabled) {
        g->selected_tool = CAD_TOOL_NONE;
        gui_set_status(g, "%s unavailable: %s", descriptor->name,
                       state.disabledReason[0] ? state.disabledReason : descriptor->help);
        return;
    }
    if (descriptor->flags & CAD_TOOL_FLAG_IMMEDIATE) {
        g->selected_tool = CAD_TOOL_NONE;
        switch (tool) {
        case CAD_TOOL_POINT_DELETE: remove_selected_points(g); break;
        case CAD_TOOL_FACE_DELETE: remove_selected_faces(g); break;
        case CAD_TOOL_POINT_FLIP: flip_selected_points(g); break;
        case CAD_TOOL_MIRROR: copy_selected_faces(g, 1); break;
        case CAD_TOOL_FACE_REVERSE: reverse_selected_faces(g); break;
        case CAD_TOOL_FACE_COPY: copy_selected_faces(g, 0); break;
        case CAD_TOOL_FACE_CUT: cut_selected_faces(g); break;
        case CAD_TOOL_FACE_SIDE: side_selected_faces(g); break;
        case CAD_TOOL_STATE:
            state_panel_open(g);
            break;
        case CAD_TOOL_UNDO: history_undo(g); break;
        default: break;
        }
        return;
    }
    if (tool_needs_points(tool) && !g->cad->selection.pointCount) {
        g->selected_tool = CAD_TOOL_NONE;
        gui_set_status(g, "Select points before using %s", descriptor->name);
        return;
    }
    if (tool_needs_faces(tool) && !g->cad->selection.polygonCount) {
        g->selected_tool = CAD_TOOL_NONE;
        gui_set_status(g, "Select faces before using %s", descriptor->name);
        return;
    }
    g->selected_tool = tool;
    if (tool == CAD_TOOL_POINT_SELECT || tool == CAD_TOOL_POINT_CREATE || tool_needs_points(tool)) {
        CadCore_SetEditMode(g->cad, tool == CAD_TOOL_POINT_SELECT ? CAD_MODE_SELECT_POINT : CAD_MODE_EDIT_POINT);
    } else {
        CadCore_SetEditMode(g->cad, tool == CAD_TOOL_FACE_SELECT ? CAD_MODE_SELECT_POLYGON : CAD_MODE_EDIT_POLYGON);
    }
    if (tool == CAD_TOOL_FACE_CREATE) {
        CadCore_ClearSelection(g->cad);
        CadCore_SetEditMode(g->cad, CAD_MODE_SELECT_POINT);
    }
    gui_set_status(g, "%s: %s", descriptor->name, descriptor->help);
}

static int apply_transform_drag(GuiState* g, int dx, int dy) {
    const int face_tool = tool_needs_faces(g->selected_tool);
    int16_t points[CAD_MAX_POINTS];
    int count = collect_transform_points(g, face_tool, points, ARRAY_COUNT(points));
    CadAffineTransform transform;
    CadAnimationScope scope;
    CadResult result;
    if (!count || (dx == 0 && dy == 0)) return 1;
    if (!g->transform_history_pushed) {
        CadAnimationSession_BeginEdit(&g->animation, g->animation_now);
        if (!history_push(g)) return 0;
        g->transform_history_pushed = 1;
    }
    memset(&transform, 0, sizeof(transform));
    transform.matrix[0][0] = 1.0;
    transform.matrix[1][1] = 1.0;
    transform.matrix[2][2] = 1.0;
    selection_center(g, points, count,
                     &transform.pivot.x, &transform.pivot.y,
                     &transform.pivot.z);
    scope = g->animation.allFrames ? CAD_ANIMATION_ALL_FRAMES
                                   : CAD_ANIMATION_CURRENT_FRAME;
    CadView* view = &g->views[g->point_move_view];
    if (g->selected_tool == CAD_TOOL_POINT_MOVE || g->selected_tool == CAD_TOOL_FACE_MOVE) {
        double tx, ty, tz;
        Rect content = view_content_rect(g, g->point_move_view);
        CadView_UnprojectDelta(view, dx, dy, content.w, content.h, &tx, &ty, &tz);
        transform.translation.x = tx;
        transform.translation.y = ty;
        transform.translation.z = tz;
    } else {
        if (g->selected_tool == CAD_TOOL_POINT_SCALE || g->selected_tool == CAD_TOOL_FACE_SCALE) {
            double factor = exp((double)(dx - dy) * 0.01);
            if (factor < 0.25) factor = 0.25;
            if (factor > 4.0) factor = 4.0;
            transform.matrix[0][0] = factor;
            transform.matrix[1][1] = factor;
            transform.matrix[2][2] = factor;
        } else {
            double angle = (double)(dx - dy) * 0.01;
            double s = sin(angle), c = cos(angle);
            if (view->type == CAD_VIEW_TOP || view->type == CAD_VIEW_3D) {
                transform.matrix[0][0] = c;
                transform.matrix[0][2] = -s;
                transform.matrix[2][0] = s;
                transform.matrix[2][2] = c;
            } else if (view->type == CAD_VIEW_FRONT) {
                transform.matrix[0][0] = c;
                transform.matrix[0][1] = -s;
                transform.matrix[1][0] = s;
                transform.matrix[1][1] = c;
            } else {
                transform.matrix[1][1] = c;
                transform.matrix[1][2] = -s;
                transform.matrix[2][1] = s;
                transform.matrix[2][2] = c;
            }
        }
    }
    result = CadAnimation_Transform(&g->cad->data,
                                    g->animation.currentFrame, scope,
                                    points, (size_t)count, &transform);
    if (!CadResult_IsSuccess(&result)) {
        history_cancel(g);
        g->transform_history_pushed = 0;
        gui_set_status(g, "Transform failed: %s", cad_result_message(&result));
        return 0;
    }
    return 1;
}

static unsigned axes_for_view(CadViewType type) {
    if (type == CAD_VIEW_TOP) return 1u | 4u;
    if (type == CAD_VIEW_FRONT) return 1u | 2u;
    if (type == CAD_VIEW_RIGHT) return 2u | 4u;
    return 0;
}

static void guided_point_click(GuiState* g, int view_index, int mouse_x, int mouse_y) {
    if (!ensure_static_topology(g, "Create Point")) return;
    Rect content = view_content_rect(g, view_index);
    double x, y, z;
    CadView_UnprojectPoint(&g->views[view_index], mouse_x - content.x, mouse_y - content.y,
                           content.w, content.h, &x, &y, &z);
    unsigned axes = axes_for_view(g->views[view_index].type);
    if (!axes) { gui_set_status(g, "Place points in Top, Front, or Right view"); return; }
    if (!g->point_pending) {
        g->point_pending = 1; g->point_pending_view = view_index; g->point_known_axes = axes;
        if (axes & 1u) g->point_pending_x = x;
        if (axes & 2u) g->point_pending_y = y;
        if (axes & 4u) g->point_pending_z = z;
        if (!(axes & 1u)) {
            gui_set_status(g, "Point preview: X is unknown; choose Top or Front (Escape cancels)");
        } else if (!(axes & 2u)) {
            gui_set_status(g, "Point preview: Y is unknown; choose Front or Right (Escape cancels)");
        } else {
            gui_set_status(g, "Point preview: Z is unknown; choose Top or Right (Escape cancels)");
        }
        return;
    }
    unsigned missing = axes & ~g->point_known_axes;
    if (!missing) { gui_set_status(g, "Choose a different orthographic view; Escape cancels"); return; }
    if (missing & 1u) g->point_pending_x = x;
    if (missing & 2u) g->point_pending_y = y;
    if (missing & 4u) g->point_pending_z = z;
    g->point_known_axes |= axes;
    if ((g->point_known_axes & 7u) == 7u) {
        if (!history_push(g)) {
            g->point_pending = 0;
            g->point_known_axes = 0;
            g->point_pending_view = -1;
            return;
        }
        int16_t created = CadCore_AddPoint(g->cad, g->point_pending_x,
                                           g->point_pending_y, g->point_pending_z);
        if (created != INVALID_INDEX) {
            CadCore_ClearSelection(g->cad);
            CadCore_SelectPoint(g->cad, created);
            gui_set_status(g, "Created point #%d at %.2f, %.2f, %.2f", created,
                           g->point_pending_x, g->point_pending_y, g->point_pending_z);
        } else {
            history_cancel(g);
            gui_set_status(g, "Point capacity reached");
        }
        if (created != INVALID_INDEX && !history_commit(g)) {
            g->point_pending = 0;
            g->point_known_axes = 0;
            g->point_pending_view = -1;
            return;
        }
        g->point_pending = 0; g->point_known_axes = 0; g->point_pending_view = -1;
    }
}

static void complete_area_selection(GuiState* g) {
    if (!g || g->area_select_view < 0) return;
    int left = g->area_start_x < g->area_end_x ? g->area_start_x : g->area_end_x;
    int right = g->area_start_x > g->area_end_x ? g->area_start_x : g->area_end_x;
    int top = g->area_start_y < g->area_end_y ? g->area_start_y : g->area_end_y;
    int bottom = g->area_start_y > g->area_end_y ? g->area_start_y : g->area_end_y;
    Rect content = view_content_rect(g, g->area_select_view);
    CadCore_ClearSelection(g->cad);
    if (g->cad->selectModeFlag) {
        for (int i = 0; i < g->cad->data.pointCount; ++i) {
            CadPosition point;
            if (!gui_scene_point(g, (int16_t)i, &point)) continue;
            int x, y;
            CadView_ProjectPoint(&g->views[g->area_select_view],
                                 point.x, point.y, point.z,
                                 &x, &y, content.w, content.h);
            x += content.x; y += content.y;
            if (x >= left && x <= right && y >= top && y <= bottom) CadCore_SelectPoint(g->cad, (int16_t)i);
        }
        gui_set_status(g, "Area selected %d point(s)", g->cad->selection.pointCount);
    } else {
        int16_t chain[CAD_MAX_FACE_POINTS];
        for (int p = 0; p < g->cad->data.polygonCount; ++p) {
            int count = polygon_point_indices(g->cad, (int16_t)p, chain, ARRAY_COUNT(chain));
            int hit = 0;
            for (int i = 0; i < count && !hit; ++i) {
                CadPosition point;
                int x, y;
                if (!gui_scene_point(g, chain[i], &point)) continue;
                CadView_ProjectPoint(&g->views[g->area_select_view],
                                     point.x, point.y, point.z,
                                     &x, &y, content.w, content.h);
                x += content.x; y += content.y;
                hit = x >= left && x <= right && y >= top && y <= bottom;
            }
            if (hit) CadCore_SelectPolygon(g->cad, (int16_t)p);
        }
        gui_set_status(g, "Area selected %d face(s)", g->cad->selection.polygonCount);
    }
    g->area_select_active = 0;
    g->area_select_armed = 0;
    g->area_select_view = -1;
}

static void draw_window_chrome(GuiState* g, GuiWin* w, int win_h, float scale_x, float scale_y) {
    (void)win_h; (void)scale_x; (void)scale_y; /* Scale handled by projection matrix */
    Rect r = w->r;
    RG_Color border = { 0,0,0,255 };
    RG_Color face = { 230,230,230,255 };
    RG_Color title = { 210,210,210,255 };

    rg_fill_rect(r.x, r.y, r.w, r.h, face);
    rg_stroke_rect(r.x, r.y, r.w, r.h, border);
    rg_fill_rect(r.x + 1, r.y + 1, r.w - 2, 18, title);
    rg_line(r.x + 1, r.y + 19, r.x + r.w - 2, r.y + 19, border);

    if (g->font && w->title) {
        /* Text drawn in the current projection: we will set viewport to full window before calling */
        font_draw(g->font, r.x + 6, r.y + 2, w->title, 0);
    }
}

static void draw_view_scrollbars(Rect inner, const CadView* view) {
    RG_Color sb = { 200,200,200,255 };
    RG_Color edge = { 120,120,120,255 };
    RG_Color thumb = { 145,145,145,255 };
    ViewScrollbarGeometry geometry;
    view_scrollbar_geometry(inner, view, &geometry);
    if (geometry.horizontal_track.w <= 0 || geometry.vertical_track.h <= 0) return;
    rg_fill_rect(geometry.horizontal_track.x, geometry.horizontal_track.y,
                 geometry.horizontal_track.w, geometry.horizontal_track.h, sb);
    rg_stroke_rect(geometry.horizontal_track.x, geometry.horizontal_track.y,
                   geometry.horizontal_track.w, geometry.horizontal_track.h, edge);
    rg_fill_rect(geometry.vertical_track.x, geometry.vertical_track.y,
                 geometry.vertical_track.w, geometry.vertical_track.h, sb);
    rg_stroke_rect(geometry.vertical_track.x, geometry.vertical_track.y,
                   geometry.vertical_track.w, geometry.vertical_track.h, edge);
    rg_fill_rect(geometry.corner.x, geometry.corner.y,
                 geometry.corner.w, geometry.corner.h, sb);
    rg_stroke_rect(geometry.corner.x, geometry.corner.y,
                   geometry.corner.w, geometry.corner.h, edge);
    rg_fill_rect(geometry.horizontal_thumb.x, geometry.horizontal_thumb.y,
                 geometry.horizontal_thumb.w, geometry.horizontal_thumb.h, thumb);
    rg_stroke_rect(geometry.horizontal_thumb.x, geometry.horizontal_thumb.y,
                   geometry.horizontal_thumb.w, geometry.horizontal_thumb.h, edge);
    rg_fill_rect(geometry.vertical_thumb.x, geometry.vertical_thumb.y,
                 geometry.vertical_thumb.w, geometry.vertical_thumb.h, thumb);
    rg_stroke_rect(geometry.vertical_thumb.x, geometry.vertical_thumb.y,
                   geometry.vertical_thumb.w, geometry.vertical_thumb.h, edge);
}

static void update_scrollbar_pan(GuiState* g, int view_index, int axis,
                                 int pointer_position, int drag_offset) {
    if (!g || view_index < 0 || view_index >= 4 || (axis != 1 && axis != 2)) return;
    ViewScrollbarGeometry geometry;
    view_scrollbar_geometry(view_client_rect(g, view_index), &g->views[view_index], &geometry);
    Rect track = axis == 1 ? geometry.horizontal_track : geometry.vertical_track;
    Rect thumb = axis == 1 ? geometry.horizontal_thumb : geometry.vertical_thumb;
    int rail_start = (axis == 1 ? track.x : track.y) + 2;
    int rail_length = (axis == 1 ? track.w : track.h) - 4;
    int thumb_length = axis == 1 ? thumb.w : thumb.h;
    int range = rail_length - thumb_length;
    double normalized = 0.0;
    if (range > 0) {
        int position = pointer_position - rail_start - drag_offset;
        if (position < 0) position = 0;
        if (position > range) position = range;
        normalized = (double)position * 2.0 / (double)range - 1.0;
        if (normalized < -0.98) normalized = -0.98;
        if (normalized > 0.98) normalized = 0.98;
    }
    if (axis == 1) g->views[view_index].pan_x = atanh(normalized) * 250.0;
    else g->views[view_index].pan_y = atanh(normalized) * 250.0;
    link_orthographic_pan(g, view_index);
}

static const GuiWin* aux_window(const GuiState* g, int aux_id) {
    if (!g) return NULL;
    switch ((GuiAuxWindowId)aux_id) {
    case GUI_AUX_TOOL_PALETTE: return &g->toolPalette;
    case GUI_AUX_COORDINATES: return &g->coordBox;
    case GUI_AUX_ANIMATION: return &g->animationWindow;
    case GUI_AUX_SHAPE_BROWSER: return &g->shapeBrowserWindow;
    default: return NULL;
    }
}

static int aux_window_visible(const GuiState* g, int aux_id) {
    if (!g) return 0;
    switch ((GuiAuxWindowId)aux_id) {
    case GUI_AUX_TOOL_PALETTE:
        return g->tool_palette_visible && g->toolPalette.r.w > 0 &&
               g->toolPalette.r.h > 0;
    case GUI_AUX_COORDINATES:
        return g->coordinates_visible && g->coordBox.r.w > 0 &&
               g->coordBox.r.h > 0;
    case GUI_AUX_ANIMATION:
        return g->animation_visible && g->animationWindow.r.w > 0 &&
               g->animationWindow.r.h > 0;
    case GUI_AUX_SHAPE_BROWSER:
        return g->shapeBrowserWindow.r.w > 0 &&
               g->shapeBrowserWindow.r.h > 0;
    default:
        return 0;
    }
}

static int topmost_aux_at(const GuiState* g, int x, int y) {
    CadUiRect rectangles[GUI_AUX_COUNT];
    unsigned char visible[GUI_AUX_COUNT];
    if (!g) return -1;
    for (int id = 0; id < GUI_AUX_COUNT; ++id) {
        const GuiWin* window = aux_window(g, id);
        rectangles[id] = window
            ? (CadUiRect){window->r.x, window->r.y, window->r.w, window->r.h}
            : (CadUiRect){0, 0, 0, 0};
        visible[id] = (unsigned char)aux_window_visible(g, id);
    }
    return CadUiZOrder_TopmostAt(g->aux_z_order, GUI_AUX_COUNT,
                                 rectangles, visible, x, y);
}

static void raise_aux(GuiState* g, int aux_id) {
    if (!g || aux_id < 0 || aux_id >= GUI_AUX_COUNT) return;
    CadUiZOrder_Raise(g->aux_z_order, GUI_AUX_COUNT, aux_id);
}

static void raise_view(GuiState* g, int view_index) {
    int position = -1;
    if (!g || view_index < 0 || view_index >= 4) return;
    for (int i = 0; i < 4; ++i) {
        if (g->view_z_order[i] == view_index) { position = i; break; }
    }
    if (position < 0 || position == 3) return;
    for (int i = position; i < 3; ++i) g->view_z_order[i] = g->view_z_order[i + 1];
    g->view_z_order[3] = view_index;
}

static int topmost_view_at(const GuiState* g, int x, int y) {
    if (!g) return -1;
    for (int slot = 3; slot >= 0; --slot) {
        int i = g->view_z_order[slot];
        if (i >= 0 && i < 4 && g->view_visible[i] &&
            pt_in_rect(x, y, g->view[i].r)) return i;
    }
    return -1;
}

static void zoom_view_at(GuiState* g, int view_index, int screen_x, int screen_y,
                         double factor) {
    Rect content;
    CadView* view;
    double anchor_x, anchor_y, anchor_z;
    double projected_x, projected_y, depth;
    if (!g || view_index < 0 || view_index >= 4 || !isfinite(factor) || factor <= 0.0) return;
    content = view_content_rect(g, view_index);
    view = &g->views[view_index];
    CadView_UnprojectPoint(view, screen_x - content.x, screen_y - content.y,
                           content.w, content.h, &anchor_x, &anchor_y, &anchor_z);
    CadView_SetZoom(view, view->zoom * factor);
    if (CadView_ProjectPointDepth(view, anchor_x, anchor_y, anchor_z,
                                  &projected_x, &projected_y, &depth,
                                  content.w, content.h)) {
        view->pan_x += (screen_x - content.x) - projected_x;
        view->pan_y -= (screen_y - content.y) - projected_y;
        link_orthographic_pan(g, view_index);
    }
}

static int handle_view_scrollbar_input(GuiState* g, const GuiInput* in, int view_index) {
    if (!g || !in || view_index < 0 || view_index >= 4) return 0;
    ViewScrollbarGeometry geometry;
    view_scrollbar_geometry(view_client_rect(g, view_index), &g->views[view_index], &geometry);
    int axis = pt_in_rect(in->mouse_x, in->mouse_y, geometry.horizontal_track) ? 1 :
               pt_in_rect(in->mouse_x, in->mouse_y, geometry.vertical_track) ? 2 : 0;
    if (!axis && !pt_in_rect(in->mouse_x, in->mouse_y, geometry.corner)) return 0;
    if (axis && in->mouse_pressed) {
        Rect thumb = axis == 1 ? geometry.horizontal_thumb : geometry.vertical_thumb;
        int coordinate = axis == 1 ? in->mouse_x : in->mouse_y;
        int thumb_start = axis == 1 ? thumb.x : thumb.y;
        int thumb_length = axis == 1 ? thumb.w : thumb.h;
        int offset = pt_in_rect(in->mouse_x, in->mouse_y, thumb)
                     ? coordinate - thumb_start : thumb_length / 2;
        update_scrollbar_pan(g, view_index, axis, coordinate, offset);
        if (in->mouse_down && !in->mouse_released) {
            g->scrollbar_view = view_index;
            g->scrollbar_axis = axis;
            g->scrollbar_drag_offset = offset;
            g->pointer_owner = GUI_POINTER_SCROLLBAR;
        }
    }
    return 1;
}

static void draw_grid(Rect inner) {
    RG_Color grid = { 220,220,220,255 };
    RG_Color axis = { 120,120,255,255 };
    for (int x = inner.x; x < inner.x + inner.w; x += 20) {
        rg_line(x, inner.y, x, inner.y + inner.h, grid);
    }
    for (int y = inner.y; y < inner.y + inner.h; y += 20) {
        rg_line(inner.x, y, inner.x + inner.w, y, grid);
    }
    rg_line(inner.x + inner.w / 2, inner.y, inner.x + inner.w / 2, inner.y + inner.h, axis);
    rg_line(inner.x, inner.y + inner.h / 2, inner.x + inner.w, inner.y + inner.h / 2, axis);
}


static int menu_item_enabled(const GuiState* g, const CadMenuItemDescriptor* item) {
    if (!g || !item || (item->flags & (CAD_MENU_ITEM_SEPARATOR | CAD_MENU_ITEM_DISABLED))) return 0;
    EditorCommandContext context = editor_command_context(g);
    return EditorController_GetCommandState(
        &g->controller, item->command, &context).enabled;
}

static Rect menu_popup_rect(const GuiState* g, int menu_index) {
    const MenuDescriptor* menu = menu_for_index(menu_index);
    int x = 8, max_width = 0;
    for (int i = 0; i < menu_index; ++i) {
        x += g->font ? font_measure(g->font, g->menus[i]) + 16
                     : (int)strlen(g->menus[i]) * 8 + 16;
    }
    if (menu) {
        for (int i = 0; i < menu->count; ++i) {
            char label[160];
            const char* text = menu_item_display_text(
                g, &menu->items[i], label, sizeof(label));
            int width = g->font ? font_measure(g->font, text)
                                : (int)strlen(text) * 8;
            if (width > max_width) max_width = width;
        }
    }
    return (Rect){ x, MenuBarHeight(), max_width + 28, menu ? menu->count * 20 : 0 };
}

static void update_submenu_rect(GuiState* g, Rect popup, int row) {
    const CadMenuItemDescriptor* submenu = row == 4 ? importSubMenuItems : exportSubMenuItems;
    int submenu_count = row == 4 ? ARRAY_COUNT(importSubMenuItems)
                                  : ARRAY_COUNT(exportSubMenuItems);
    int max_width = 0;
    for (int i = 0; i < submenu_count; ++i) {
        int width = g->font ? font_measure(g->font, submenu[i].label)
                            : (int)strlen(submenu[i].label) * 8;
        if (width > max_width) max_width = width;
    }
    g->submenu_rect = (Rect){ popup.x + popup.w - 2, popup.y + row * 20,
                              max_width + 24, submenu_count * 20 };
}

static int update_menu_input(GuiState* g, const GuiInput* in) {
    if (in->mouse_pressed && in->mouse_y < MenuBarHeight()) {
        int x = 8;
        for (int i = 0; i < g->menu_count; ++i) {
            int width = g->font ? font_measure(g->font, g->menus[i]) + 16
                                : (int)strlen(g->menus[i]) * 8 + 16;
            if (pt_in_rect(in->mouse_x, in->mouse_y, (Rect){x, 0, width, MenuBarHeight()})) {
                g->menu_open = g->menu_open == i ? -1 : i;
                g->menu_hover_item = -1;
                g->submenu_open = 0;
                g->pointer_owner = GUI_POINTER_MENU;
                return 1;
            }
            x += width;
        }
        g->menu_open = -1;
        return 1;
    }
    if (g->menu_open < 0) return 0;

    const MenuDescriptor* menu = menu_for_index(g->menu_open);
    Rect popup = menu_popup_rect(g, g->menu_open);
    int in_popup = pt_in_rect(in->mouse_x, in->mouse_y, popup);
    int in_submenu = g->submenu_open && pt_in_rect(in->mouse_x, in->mouse_y, g->submenu_rect);
    if (in_popup) {
        int row = (in->mouse_y - popup.y) / 20;
        g->menu_hover_item = row >= 0 && row < menu->count ? row : -1;
        if (g->menu_open == 0 && (row == 4 || row == 5)) {
            g->submenu_open = row + 1;
            update_submenu_rect(g, popup, row);
        } else {
            g->submenu_open = 0;
            g->submenu_hover_item = -1;
        }
        if (in->mouse_pressed && g->menu_hover_item >= 0) {
            const CadMenuItemDescriptor* item = &menu->items[g->menu_hover_item];
            if (!(item->flags & CAD_MENU_ITEM_SUBMENU) && menu_item_enabled(g, item)) {
                CadCommandId command = item->command;
                g->menu_open = -1; g->submenu_open = 0; g->menu_hover_item = -1;
                execute_editor_command(g, command);
            }
        }
    } else if (in_submenu) {
        int row = (in->mouse_y - g->submenu_rect.y) / 20;
        int submenu_count = g->submenu_open == 5
                                ? ARRAY_COUNT(importSubMenuItems)
                                : ARRAY_COUNT(exportSubMenuItems);
        g->submenu_hover_item = row >= 0 && row < submenu_count ? row : -1;
        if (in->mouse_pressed && g->submenu_hover_item >= 0) {
            const CadMenuItemDescriptor* submenu = g->submenu_open == 5 ? importSubMenuItems : exportSubMenuItems;
            if (menu_item_enabled(g, &submenu[g->submenu_hover_item])) {
                CadCommandId command = submenu[g->submenu_hover_item].command;
                g->menu_open = -1; g->submenu_open = 0; g->submenu_hover_item = -1;
                execute_editor_command(g, command);
            }
        }
    } else {
        g->menu_hover_item = -1;
        g->submenu_hover_item = -1;
        if (in->mouse_pressed || in->mouse_right_pressed || in->mouse_middle_pressed) {
            g->menu_open = -1;
            g->submenu_open = 0;
        }
    }
    return 1; /* An open menu owns the pointer even over transparent desktop. */
}

static int begin_title_drag(GuiState* g, GuiWin* window, const GuiInput* in) {
    if (!window || !window->draggable || window->r.w <= 0 || window->r.h <= 0) return 0;
    if (!in->mouse_down || in->mouse_released) return 0;
    Rect title = { window->r.x, window->r.y, window->r.w, 20 };
    if (!pt_in_rect(in->mouse_x, in->mouse_y, title)) return 0;
    g->drag_win = window;
    g->drag_off_x = in->mouse_x - window->r.x;
    g->drag_off_y = in->mouse_y - window->r.y;
    g->pointer_owner = GUI_POINTER_WINDOW;
    g->auto_layout = 0;
    return 1;
}

static void select_point_at(GuiState* g, int view_index, int mouse_x, int mouse_y) {
    Rect content = view_content_rect(g, view_index);
    int16_t points[64];
    int count = gui_find_points_at_location(g, view_index, mouse_x, mouse_y,
                                            content, 10, 0.01, points,
                                            ARRAY_COUNT(points));
    if (!count) { gui_set_status(g, "No point under cursor"); return; }
    int all_selected = 1;
    for (int i = 0; i < count; ++i) all_selected &= CadCore_IsPointSelected(g->cad, points[i]);
    for (int i = 0; i < count; ++i) {
        if (all_selected) CadCore_DeselectPoint(g->cad, points[i]);
        else CadCore_SelectPoint(g->cad, points[i]);
    }
    gui_set_status(g, "%s %d coincident point(s)", all_selected ? "Deselected" : "Selected", count);
}

static void select_face_at(GuiState* g, int view_index, int mouse_x, int mouse_y) {
    int16_t polygon = find_polygon_at(g, view_index, mouse_x, mouse_y, NULL);
    if (polygon == INVALID_INDEX) { gui_set_status(g, "No face under cursor"); return; }
    if (CadCore_IsPolygonSelected(g->cad, polygon)) {
        CadCore_DeselectPolygon(g->cad, polygon);
        gui_set_status(g, "Deselected face #%d", polygon);
    } else {
        CadCore_SelectPolygon(g->cad, polygon);
        gui_set_status(g, "Selected face #%d", polygon);
    }
}

static void handle_shape_browser_click(GuiState* g, const GuiInput* in) {
    ShapeBrowserLayout layout = shape_browser_layout(g);
    int filtered_count = filtered_shape_count(g);
    int visible = layout.list.h / 20;
    int maximum = filtered_count > visible ? filtered_count - visible : 0;
    if (pt_in_rect(in->mouse_x, in->mouse_y, layout.search) && in->mouse_pressed) {
        g->shape_search_active = 1;
        gui_set_status(g, "Shape search active; type to filter, Enter finishes");
        return;
    }
    if (in->mouse_pressed) g->shape_search_active = 0;
    if (pt_in_rect(in->mouse_x, in->mouse_y, layout.replace_button) && in->mouse_pressed) {
        const CadAsmShapeInfo* shape;
        const char* name;
        const char* separator;
        const char* source_name;
        char* import_path_copy;
        char import_path[GUI_PATH_CAPACITY * 2];
        size_t folder_length;
        int written;
        if (!g->shape_preview_valid || g->shape_selected < 0) {
            gui_set_status(g, "Select a valid ASM shape preview before Replace");
            return;
        }
        name = g->shape_names[g->shape_selected];
        shape = CadImportAsm_FindShape(g->shape_catalog, name);
        if (!shape || shape->sourceIndex >= g->shape_asm_source_count ||
            !g->shape_asm_sources[shape->sourceIndex].name ||
            !g->shape_asm_sources[shape->sourceIndex].name[0]) {
            gui_set_status(g, "The previewed ASM source is no longer available");
            return;
        }
        source_name = g->shape_asm_sources[shape->sourceIndex].name;
        folder_length = strlen(g->shape_folder_path);
        separator = folder_length &&
                    (g->shape_folder_path[folder_length - 1] == '\\' ||
                     g->shape_folder_path[folder_length - 1] == '/')
                        ? "" : "\\";
        written = snprintf(import_path, sizeof(import_path), "%s%s%s",
                           g->shape_folder_path, separator,
                           source_name);
        if (written < 0 || (size_t)written >= sizeof(import_path)) {
            gui_set_status(g, "The previewed ASM source path is too long");
            return;
        }
        import_path_copy = asm_copy_string(import_path);
        if (!import_path_copy) {
            gui_set_status(g,
                           "Not enough memory to preserve the ASM source path; current document unchanged");
            return;
        }
        if (confirm_replace_document(g, "replacing the document with the previewed ASM shape")) {
            replace_document(g, g->shape_preview, NULL, 1);
            /* CadDocument_New cleared the previous origin; transfer the path
               allocation so source-overwrite protection cannot fail after
               the old document has been replaced. */
            g->document.lastImportPath = import_path_copy;
            import_path_copy = NULL;
            gui_set_status(g,
                           "Replaced document with %s from %s; use Save As for native CAD",
                           name, source_name);
        }
        free(import_path_copy);
        return;
    }
    if (!pt_in_rect(in->mouse_x, in->mouse_y, layout.list)) return;
    if (in->wheel_delta) {
        g->shape_scroll_offset -= in->wheel_delta;
        if (g->shape_scroll_offset < 0) g->shape_scroll_offset = 0;
        if (g->shape_scroll_offset > maximum) g->shape_scroll_offset = maximum;
    }
    if (!in->mouse_pressed) return;
    if (in->mouse_x >= layout.list.x + layout.list.w - 14 && maximum > 0) {
        int track = layout.list.h - 8;
        int position = in->mouse_y - layout.list.y - 4;
        if (position < 0) position = 0;
        if (position > track) position = track;
        g->shape_scroll_offset = track > 0 ? position * maximum / track : 0;
        return;
    }
    int filtered_index = (in->mouse_y - layout.list.y) / 20 + g->shape_scroll_offset;
    int index = filtered_shape_index(g, filtered_index);
    select_shape_preview(g, index);
}

typedef CadAnimationPanelLayout AnimationPanelLayout;

static AnimationPanelLayout animation_panel_layout(const GuiState* g) {
    AnimationPanelLayout layout;
    CadAnimationPanel_ComputeLayout(
        g ? g->animationWindow.r : (Rect){0, 0, 0, 0}, &layout);
    return layout;
}

static void animation_set_frame(GuiState* g, int frame) {
    if (!g) return;
    g->anim_copy_mode = 0;
    CadAnimationSession_SetFrame(&g->animation, frame);
}

static int animation_finish_edit(GuiState* g, const char* label,
                                 CadResult result, int displayed_frame) {
    if (!CadResult_IsSuccess(&result)) {
        history_cancel(g);
        gui_set_status(g, "%s failed: %s", label, cad_result_message(&result));
        return 0;
    }
    if (!history_commit(g)) return 0;
    if (displayed_frame >= 0) {
        g->animation.currentFrame = displayed_frame;
        g->animation.previewFrame = (double)displayed_frame;
    }
    gui_set_status(g, "%s", label);
    return 1;
}

static int animation_selected_static_faces(GuiState* g,
                                           int16_t output[CAD_MAX_POLYGONS]) {
    int count = 0;
    if (!g) return 0;
    for (int i = 0; i < g->cad->selection.polygonCount; ++i) {
        int16_t polygon = g->cad->selection.selectedPolygons[i];
        if (polygon < 0 || polygon >= CAD_MAX_POLYGONS ||
            !g->cad->data.polygons[polygon].flags ||
            g->cad->data.polygons[polygon].animation != -1) continue;
        if (output) output[count] = polygon;
        ++count;
    }
    return count;
}

static CadAnimationPanelState animation_panel_state(GuiState* g) {
    CadAnimationPanelState state;
    memset(&state, 0, sizeof(state));
    if (!g) return state;
    state.informationValid = g->animation_info_valid;
    state.editable = g->animation_info_valid && g->animation_info.editable;
    state.frameCount = g->animation_info.frameCount;
    state.maximumFrameCount = g->animation_info.maximumFrameCount;
    state.staticFaceCount = g->animation_info.staticFaceCount;
    state.selectedStaticFaceCount = animation_selected_static_faces(g, NULL);
    state.selectedPointCount = g->cad ? g->cad->selection.pointCount : 0;
    state.hasAnimation = g->cad && CadAnimation_HasAny(&g->cad->data);
    return state;
}

static void animation_create(GuiState* g, int selected_only) {
    const int16_t* faces = NULL;
    size_t face_count = 0;
    int16_t selected[CAD_MAX_POLYGONS];
    CadResult result;
    if (!g) return;
    if (g->animation_info.editable) {
        gui_set_status(g, "Animation already exists; use Add Faces for static faces");
        return;
    }
    CadAnimationSession_BeginEdit(&g->animation, g->animation_now);
    if (selected_only) {
        face_count = (size_t)animation_selected_static_faces(g, selected);
        faces = selected;
        if (!face_count) {
            gui_set_status(g, "Select one or more static faces to animate");
            return;
        }
    }
    if (!history_push_named(g, selected_only ? "Create Selected Animation"
                                             : "Create Animation")) return;
    result = CadAnimation_Create(&g->cad->data, faces, face_count, 0);
    animation_finish_edit(g, selected_only ? "Created animation for selected faces"
                                           : "Created animation for all faces",
                          result, 0);
}

static void animation_set_count_delta(GuiState* g, int delta) {
    int target;
    CadResult result;
    if (!g || !g->animation_info.editable) {
        gui_set_status(g, "Create an animation before changing frame count");
        return;
    }
    target = g->animation_info.frameCount + delta;
    if (target < 1 || target > g->animation_info.maximumFrameCount) {
        gui_set_status(g, "Frame count must stay within 1..%d",
                       g->animation_info.maximumFrameCount);
        return;
    }
    CadAnimationSession_BeginEdit(&g->animation, g->animation_now);
    if (!history_push_named(g, "Set Frame Count")) return;
    result = CadAnimation_SetFrameCount(&g->cad->data, target);
    animation_finish_edit(g, "Set animation frame count", result,
                          g->animation.currentFrame < target
                              ? g->animation.currentFrame : target - 1);
}

static void animation_insert(GuiState* g, int duplicate) {
    int current, inserted;
    CadResult result;
    if (!g || !g->animation_info.editable) {
        gui_set_status(g, "Create an animation before inserting frames");
        return;
    }
    if (g->animation_info.frameCount >= g->animation_info.maximumFrameCount) {
        gui_set_status(g, "Animation capacity is full at %d frame(s)",
                       g->animation_info.frameCount);
        return;
    }
    CadAnimationSession_BeginEdit(&g->animation, g->animation_now);
    current = g->animation.currentFrame;
    inserted = current + 1;
    if (!history_push_named(g, duplicate ? "Duplicate Frame" : "Insert Frame")) return;
    result = duplicate
        ? CadAnimation_DuplicateFrame(&g->cad->data, current, inserted)
        : CadAnimation_InsertFrame(&g->cad->data, inserted, -1);
    animation_finish_edit(g, duplicate ? "Duplicated frame" : "Inserted frame",
                          result, inserted);
}

static void animation_delete_current(GuiState* g) {
    int current, next;
    CadResult result;
    if (!g || !g->animation_info.editable) {
        gui_set_status(g, "There is no editable animation frame to delete");
        return;
    }
    if (g->animation_info.frameCount <= 1) {
        gui_set_status(g, "An animation must retain at least one frame");
        return;
    }
    CadAnimationSession_BeginEdit(&g->animation, g->animation_now);
    current = g->animation.currentFrame;
    next = current < g->animation_info.frameCount - 1 ? current
                                                       : current - 1;
    if (!history_push_named(g, "Delete Frame")) return;
    result = CadAnimation_DeleteFrame(&g->cad->data, current);
    animation_finish_edit(g, "Deleted frame", result, next < 0 ? 0 : next);
}

static void animation_arm_copy(GuiState* g, int selected_only) {
    if (!g || !g->animation_info.editable) {
        gui_set_status(g, "There is no editable animation pose to copy");
        return;
    }
    if (selected_only && !g->cad->selection.pointCount) {
        gui_set_status(g, "Select points before using Copy Selected");
        return;
    }
    CadAnimationSession_BeginEdit(&g->animation, g->animation_now);
    g->anim_copy_mode = selected_only ? 2 : 1;
    g->anim_copy_source = g->animation.currentFrame;
    gui_set_status(g, "Copy %s armed from frame %d; click the target frame",
                   selected_only ? "selected points" : "complete pose",
                   g->anim_copy_source);
}

static void animation_copy_to(GuiState* g, int target) {
    const int16_t* points = NULL;
    size_t point_count = 0;
    CadResult result;
    const int mode = g ? g->anim_copy_mode : 0;
    if (!g || !mode) return;
    if (mode == 2) {
        points = g->cad->selection.selectedPoints;
        point_count = (size_t)g->cad->selection.pointCount;
    }
    if (!history_push_named(g, mode == 2 ? "Copy Selected Pose"
                                         : "Copy Pose")) return;
    result = CadAnimation_CopyFrame(&g->cad->data, g->anim_copy_source,
                                    target, points, point_count);
    g->anim_copy_mode = 0;
    animation_finish_edit(g, mode == 2 ? "Copied selected points to frame"
                                       : "Copied complete pose to frame",
                          result, target);
}

static void animation_add_selected_faces(GuiState* g) {
    int16_t selected[CAD_MAX_POLYGONS];
    int count;
    CadResult result;
    if (!g || !g->animation_info.editable) {
        gui_set_status(g, "Create an animation before adding faces");
        return;
    }
    count = animation_selected_static_faces(g, selected);
    if (!count) {
        gui_set_status(g, "Select one or more previously static faces");
        return;
    }
    CadAnimationSession_BeginEdit(&g->animation, g->animation_now);
    if (!history_push_named(g, "Add Faces to Animation")) return;
    result = CadAnimation_AddFaces(&g->cad->data, selected, (size_t)count);
    animation_finish_edit(g, "Added selected faces to animation", result,
                          g->animation.currentFrame);
}

static void animation_make_static_copy(GuiState* g) {
    CadFileData* baked;
    CadResult result;
    if (!g || !g->scene_valid || !CadAnimation_HasAny(&g->cad->data)) {
        gui_set_status(g, "There is no displayed animation pose to make static");
        return;
    }
    CadAnimationSession_BeginEdit(&g->animation, g->animation_now);
    baked = (CadFileData*)malloc(sizeof(*baked));
    if (!baked) {
        gui_set_status(g, "Not enough memory to create a static pose copy");
        return;
    }
    result = CadAnimation_MakeStaticCopy(&g->cad->data, g->scene.pose, baked);
    if (!CadResult_IsSuccess(&result)) {
        gui_set_status(g, "Static copy failed: %s", cad_result_message(&result));
        free(baked);
        return;
    }
    if (!history_push_named(g, "Make Static Copy")) {
        free(baked);
        return;
    }
    g->cad->data = *baked;
    free(baked);
    CadCore_RebuildDerivedState(g->cad);
    if (!history_commit(g)) return;
    CadDocument_MakeUnnamed(&g->document);
    g->animation.currentFrame = 0;
    g->animation.previewFrame = 0.0;
    gui_set_status(g, "Created unnamed static copy from the exact displayed pose");
}

static int animation_frame_from_pointer(const AnimationPanelLayout* layout,
                                        const GuiState* g, int mouse_x,
                                        int fractional, double* position) {
    int frame = -1;
    int count = g && g->animation_info.frameCount > 0
                    ? g->animation_info.frameCount : 1;
    return CadAnimationPanel_MapFrame(layout, mouse_x, count, fractional,
                                      &frame, position) ? frame : -1;
}

static void handle_animation_panel(GuiState* g, const GuiInput* in) {
    AnimationPanelLayout layout;
    CadAnimationPanelState state;
    CadAnimationPanelHit hit;
    int count;
    if (!g || !in) return;
    layout = animation_panel_layout(g);
    state = animation_panel_state(g);
    count = g->animation_info.frameCount > 0 ? g->animation_info.frameCount : 1;
    if (in->mouse_pressed && begin_title_drag(g, &g->animationWindow, in)) {
        if (g->animation_docked) {
            g->animation_docked = 0;
            /* Reclaim dock space once, then preserve the newly manual floating
               placement for the rest of this drag. */
            layout_cleanup(g, g->layout_width, g->layout_height);
            g->auto_layout = 0;
        }
        return;
    }
    if (!layout.usable) {
        if (in->mouse_pressed)
            gui_set_status(g, "Enlarge or float the animation timeline to use its controls");
        return;
    }
    if (!in->mouse_pressed) return;
    hit = CadAnimationPanel_HitTest(&layout, in->mouse_x, in->mouse_y,
                                    count, g->anim_copy_mode == 0);
    if (!CadAnimationPanel_IsActionEnabled(hit.action, &state)) {
        if (hit.action != CAD_ANIMATION_PANEL_NONE)
            gui_set_status(g, "That animation action is unavailable in the current document state");
        return;
    }
    if (hit.action == CAD_ANIMATION_PANEL_STRIP) {
        if (g->anim_copy_mode) {
            animation_copy_to(g, hit.frameIndex);
        } else {
            CadAnimationSession_Seek(&g->animation, hit.framePosition);
            g->anim_scrubbing = in->mouse_down && !in->mouse_released;
            g->pointer_owner = g->anim_scrubbing ? GUI_POINTER_ANIMATION
                                                 : GUI_POINTER_NONE;
            if (!g->anim_scrubbing) CadAnimationSession_EndScrub(&g->animation);
        }
        return;
    }
    switch (hit.action) {
    case CAD_ANIMATION_PANEL_FIRST:
        animation_set_frame(g, 0);
        break;
    case CAD_ANIMATION_PANEL_PREVIOUS:
        animation_set_frame(g, g->animation.currentFrame - 1);
        break;
    case CAD_ANIMATION_PANEL_PLAY_PAUSE:
        if (g->animation.playing)
            CadAnimationSession_Pause(&g->animation, g->animation_now);
        else
            CadAnimationSession_Play(&g->animation, g->animation_now);
        break;
    case CAD_ANIMATION_PANEL_STOP:
        CadAnimationSession_Stop(&g->animation);
        break;
    case CAD_ANIMATION_PANEL_NEXT:
        animation_set_frame(g, g->animation.currentFrame + 1);
        break;
    case CAD_ANIMATION_PANEL_LAST:
        animation_set_frame(g, count - 1);
        break;
    case CAD_ANIMATION_PANEL_TOGGLE_LOOP:
        g->animation.loop = !g->animation.loop;
        break;
    case CAD_ANIMATION_PANEL_TOGGLE_INTERPOLATION:
        g->animation.interpolation = !g->animation.interpolation;
        break;
    case CAD_ANIMATION_PANEL_FPS_DOWN:
        g->animation.fps -= 1.0;
        if (g->animation.fps < 1.0) g->animation.fps = 1.0;
        break;
    case CAD_ANIMATION_PANEL_FPS_UP:
        g->animation.fps += 1.0;
        if (g->animation.fps > 60.0) g->animation.fps = 60.0;
        break;
    case CAD_ANIMATION_PANEL_TOGGLE_ALL_FRAMES:
        g->animation.allFrames = !g->animation.allFrames;
        break;
    case CAD_ANIMATION_PANEL_TOGGLE_DOCK:
        g->animation_docked = !g->animation_docked;
        g->auto_layout = 1;
        break;
    case CAD_ANIMATION_PANEL_CREATE_ALL:
        animation_create(g, 0);
        break;
    case CAD_ANIMATION_PANEL_CREATE_SELECTED:
        animation_create(g, 1);
        break;
    case CAD_ANIMATION_PANEL_COUNT_DOWN:
        animation_set_count_delta(g, -1);
        break;
    case CAD_ANIMATION_PANEL_COUNT_UP:
        animation_set_count_delta(g, 1);
        break;
    case CAD_ANIMATION_PANEL_INSERT:
        animation_insert(g, 0);
        break;
    case CAD_ANIMATION_PANEL_DUPLICATE:
        animation_insert(g, 1);
        break;
    case CAD_ANIMATION_PANEL_DELETE:
        animation_delete_current(g);
        break;
    case CAD_ANIMATION_PANEL_COPY_ALL:
        animation_arm_copy(g, 0);
        break;
    case CAD_ANIMATION_PANEL_COPY_SELECTED:
        animation_arm_copy(g, 1);
        break;
    case CAD_ANIMATION_PANEL_ADD_FACES:
        animation_add_selected_faces(g);
        break;
    case CAD_ANIMATION_PANEL_MAKE_STATIC_COPY:
        animation_make_static_copy(g);
        break;
    default:
        break;
    }
}

void gui_update(GuiState* g, const GuiInput* in, int win_w, int win_h) {
    if (!g || !in) return;
    if (g->auto_layout) {
        layout_cleanup(g, win_w, win_h);
    } else {
        g->layout_width = win_w;
        g->layout_height = win_h;
        clamp_manual_layout(g, win_w, win_h);
    }

    g->animation_now = (double)SDL_GetTicksNS() / 1000000000.0;
    if (in->mouse_pressed && g->shape_search_active &&
        !pt_in_rect(in->mouse_x, in->mouse_y, g->shapeBrowserWindow.r)) {
        g->shape_search_active = 0;
    }
    if (g->anim_scrubbing) {
        AnimationPanelLayout layout = animation_panel_layout(g);
        double position = 0.0;
        animation_frame_from_pointer(&layout, g, in->mouse_x, 1, &position);
        CadAnimationSession_Seek(&g->animation, position);
        if (in->mouse_released || !in->mouse_down) {
            CadAnimationSession_EndScrub(&g->animation);
            g->anim_scrubbing = 0;
            g->pointer_owner = GUI_POINTER_NONE;
        }
        animation_update_scene(g, g->animation_now);
        return;
    }
    animation_update_scene(g, g->animation_now);

    if (update_menu_input(g, in)) return;

    /* Complete or update an existing pointer capture before hit-testing any
       other layer.  This prevents drags crossing overlapping windows from
       leaking edits into the model beneath them. */
    if (g->scrollbar_view >= 0) {
        if (in->mouse_down) {
            update_scrollbar_pan(g, g->scrollbar_view, g->scrollbar_axis,
                                 g->scrollbar_axis == 1 ? in->mouse_x : in->mouse_y,
                                 g->scrollbar_drag_offset);
        }
        if (in->mouse_released || !in->mouse_down) {
            g->scrollbar_view = -1;
            g->scrollbar_axis = 0;
            g->pointer_owner = GUI_POINTER_NONE;
        }
        return;
    }
    if (g->area_select_active) {
        g->area_end_x = in->mouse_x; g->area_end_y = in->mouse_y;
        if (in->mouse_released || !in->mouse_down) {
            complete_area_selection(g);
            g->pointer_owner = GUI_POINTER_NONE;
        }
        return;
    }
    if (g->point_move_active) {
        if (in->mouse_down) {
            int dx = in->mouse_x - g->last_mouse_x, dy = in->mouse_y - g->last_mouse_y;
            if (!apply_transform_drag(g, dx, dy)) {
                g->point_move_active = 0;
                g->point_move_view = -1;
                g->pointer_owner = GUI_POINTER_NONE;
                return;
            }
            g->last_mouse_x = in->mouse_x; g->last_mouse_y = in->mouse_y;
        }
        if (in->mouse_released || !in->mouse_down) {
            int committed = !g->transform_history_pushed || history_commit(g);
            g->point_move_active = 0; g->point_move_view = -1;
            g->pointer_owner = GUI_POINTER_NONE;
            if (committed) gui_set_status(g, "%s complete", toolDescriptors[g->selected_tool].name);
        }
        return;
    }
    if (g->resize_win) {
        if (in->mouse_down) {
            int dx = in->mouse_x - g->resize_start_x, dy = in->mouse_y - g->resize_start_y;
            Rect* r = &g->resize_win->r;
            int right = g->resize_window_x + g->resize_start_w;
            int bottom = g->resize_window_y + g->resize_start_h;
            int minimum_width = g->resize_win == &g->animationWindow ? 380 : 120;
            int minimum_height = g->resize_win == &g->animationWindow ? 124 : 90;
            r->x = g->resize_window_x;
            r->y = g->resize_window_y;
            r->w = g->resize_start_w;
            r->h = g->resize_start_h;
            if (g->resize_edge & 1) { r->x = g->resize_window_x + dx; r->w = right - r->x; }
            if (g->resize_edge & 2) r->w = g->resize_start_w + dx;
            if (g->resize_edge & 4) { r->y = g->resize_window_y + dy; r->h = bottom - r->y; }
            if (g->resize_edge & 8) r->h = g->resize_start_h + dy;
            if (r->w < minimum_width) {
                r->w = minimum_width;
                if (g->resize_edge & 1) r->x = right - minimum_width;
            }
            if (r->h < minimum_height) {
                r->h = minimum_height;
                if (g->resize_edge & 4) r->y = bottom - minimum_height;
            }
            clamp_window_reachable(g->resize_win, win_w, win_h);
        }
        if (in->mouse_released || !in->mouse_down) { g->resize_win = NULL; g->resize_edge = 0; g->pointer_owner = GUI_POINTER_NONE; }
        return;
    }
    if (g->drag_win) {
        if (in->mouse_down) {
            g->drag_win->r.x = in->mouse_x - g->drag_off_x;
            g->drag_win->r.y = in->mouse_y - g->drag_off_y;
            clamp_window_reachable(g->drag_win, win_w, win_h);
        }
        if (in->mouse_released || !in->mouse_down) { g->drag_win = NULL; g->pointer_owner = GUI_POINTER_NONE; }
        return;
    }
    if (g->view_interacting >= 0) {
        if (in->mouse_down) {
            int dx = in->mouse_x - g->last_mouse_x, dy = in->mouse_y - g->last_mouse_y;
            CadView* view = &g->views[g->view_interacting];
            if (view->type == CAD_VIEW_3D) {
                if (in->modifiers & GUI_MOD_SHIFT) {
                    CadView_RotateRoll(view, dx * 0.5);
                } else {
                    /* Orbit opposite the pointer delta so the model/grid follows
                       the drag instead of feeling inverted on pitch and yaw. */
                    CadView_Rotate(view, -dx * 0.5, -dy * 0.5);
                }
            } else {
                CadView_Pan(view, dx, -dy);
                link_orthographic_pan(g, g->view_interacting);
            }
            g->last_mouse_x = in->mouse_x; g->last_mouse_y = in->mouse_y;
        }
        if (in->mouse_released || !in->mouse_down) { g->view_interacting = -1; g->pointer_owner = GUI_POINTER_NONE; }
        return;
    }
    if (g->view_right_interacting >= 0) {
        if (in->mouse_right_down) {
            int dx = in->mouse_x - g->last_mouse_x, dy = in->mouse_y - g->last_mouse_y;
            CadView* view = &g->views[g->view_right_interacting];
            if (view->type == CAD_VIEW_3D) {
                CadView_Pan3DVertical(view, -dy);
                view->pan_x += dx;
            } else {
                CadView_Pan(view, dx, -dy);
                link_orthographic_pan(g, g->view_right_interacting);
            }
            g->last_mouse_x = in->mouse_x; g->last_mouse_y = in->mouse_y;
        }
        if (in->mouse_right_released || !in->mouse_right_down) { g->view_right_interacting = -1; g->pointer_owner = GUI_POINTER_NONE; }
        return;
    }
    if (g->view_middle_interacting >= 0) {
        if (in->mouse_middle_down) {
            int dy = in->mouse_y - g->last_mouse_y;
            zoom_view_at(g, g->view_middle_interacting, in->mouse_x, in->mouse_y,
                         exp((double)-dy * 0.015));
            g->last_mouse_y = in->mouse_y;
        }
        if (in->mouse_middle_released || !in->mouse_middle_down) { g->view_middle_interacting = -1; g->pointer_owner = GUI_POINTER_NONE; }
        return;
    }

    /* STATE / TenKey is modal over the editor desktop.  Menus are processed
       above it, but every other pointer event is consumed here so a click can
       never edit geometry through the panel. */
    if (g->state_visible) {
        if (in->mouse_pressed && pt_in_rect(in->mouse_x, in->mouse_y, g->stateWindow.r)) {
            if (begin_title_drag(g, &g->stateWindow, in)) return;
            for (int field = 0; field < 15; ++field) {
                if (pt_in_rect(in->mouse_x, in->mouse_y, state_field_rect(g, field))) {
                    g->state_active_field = field;
                    g->state_replace_on_input = 1;
                    return;
                }
                if (pt_in_rect(in->mouse_x, in->mouse_y, state_minus_rect(g, field))) {
                    state_adjust_field(g, field, -1);
                    return;
                }
                if (pt_in_rect(in->mouse_x, in->mouse_y, state_plus_rect(g, field))) {
                    state_adjust_field(g, field, 1);
                    return;
                }
            }
            if (pt_in_rect(in->mouse_x, in->mouse_y, state_apply_rect(g))) {
                state_panel_apply(g);
                return;
            }
            if (pt_in_rect(in->mouse_x, in->mouse_y, state_cancel_rect(g))) {
                state_panel_close(g);
                return;
            }
        }
        return;
    }

    /* The persistent status band is a real top-level UI surface. */
    if (in->mouse_y >= win_h - 22) return;

    /* Auxiliary windows form a single desktop stack above every modeling
       view.  Resolve exactly one owner before looking at any child controls;
       this keeps hit-testing and painting consistent when panels overlap. */
    int topmost_aux = topmost_aux_at(g, in->mouse_x, in->mouse_y);
    if (topmost_aux >= 0) {
        if (in->mouse_pressed || in->mouse_right_pressed ||
            in->mouse_middle_pressed) {
            raise_aux(g, topmost_aux);
        }
        switch ((GuiAuxWindowId)topmost_aux) {
        case GUI_AUX_SHAPE_BROWSER:
            if (in->mouse_pressed &&
                begin_title_drag(g, &g->shapeBrowserWindow, in)) return;
            handle_shape_browser_click(g, in);
            if (in->mouse_pressed && in->mouse_down && !in->mouse_released) {
                g->pointer_owner = GUI_POINTER_SHAPE_BROWSER;
            } else if (in->mouse_released || !in->mouse_down) {
                g->pointer_owner = GUI_POINTER_NONE;
            }
            return;
        case GUI_AUX_ANIMATION:
            if (!g->animation_docked && in->mouse_pressed && in->mouse_down &&
                !in->mouse_released) {
                int edge = get_resize_edge(in->mouse_x, in->mouse_y,
                                           g->animationWindow.r, 6);
                if (edge) {
                    g->resize_win = &g->animationWindow;
                    g->resize_edge = edge;
                    g->resize_start_x = in->mouse_x;
                    g->resize_start_y = in->mouse_y;
                    g->resize_window_x = g->animationWindow.r.x;
                    g->resize_window_y = g->animationWindow.r.y;
                    g->resize_start_w = g->animationWindow.r.w;
                    g->resize_start_h = g->animationWindow.r.h;
                    g->pointer_owner = GUI_POINTER_WINDOW;
                    g->auto_layout = 0;
                    return;
                }
            }
            handle_animation_panel(g, in);
            return;
        case GUI_AUX_COORDINATES:
            if (in->mouse_pressed) begin_title_drag(g, &g->coordBox, in);
            return;
        case GUI_AUX_TOOL_PALETTE:
            if (in->mouse_pressed &&
                begin_title_drag(g, &g->toolPalette, in)) return;
            for (int slot = 0; slot < CAD_TOOL_COUNT; ++slot) {
                if (!pt_in_rect(in->mouse_x, in->mouse_y,
                                tool_button_rect(g, slot))) continue;
                CadToolId tool = toolPaletteOrder[slot];
                EditorCommandContext context = editor_command_context(g);
                CadCommandState state = EditorController_GetToolState(
                    &g->controller, tool, &context);
                if (!in->mouse_pressed) {
                    snprintf(g->status_text, sizeof(g->status_text),
                             "%s - %s%s%s%s", toolDescriptors[tool].name,
                             toolDescriptors[tool].help,
                             state.enabled ? "" : " (unavailable: ",
                             state.enabled ? "" : state.disabledReason,
                             state.enabled ? "" : ")");
                } else if (g->selected_tool == tool &&
                           !(toolDescriptors[tool].flags &
                             CAD_TOOL_FLAG_IMMEDIATE)) {
                    g->selected_tool = CAD_TOOL_NONE;
                    reset_interaction(g);
                    gui_set_status(g, "Tool cancelled");
                } else {
                    activate_tool(g, tool);
                }
                break;
            }
            return;
        default:
            return;
        }
    }

    /* Resolve one complete desktop view before considering any of its child
       regions.  This preserves z-order when movable windows overlap. */
    int topmost_view = topmost_view_at(g, in->mouse_x, in->mouse_y);
    if ((in->mouse_pressed || in->mouse_right_pressed || in->mouse_middle_pressed) &&
        topmost_view >= 0) raise_view(g, topmost_view);
    if (handle_view_scrollbar_input(g, in, topmost_view)) return;

    /* View chrome sits above its content. */
    if (in->mouse_pressed && topmost_view >= 0) {
        Rect content = view_content_rect(g, topmost_view);
        if (!pt_in_rect(in->mouse_x, in->mouse_y, content)) {
            int edge = get_resize_edge(in->mouse_x, in->mouse_y,
                                       g->view[topmost_view].r, 6);
            if (edge && in->mouse_down && !in->mouse_released) {
                g->resize_win = &g->view[topmost_view]; g->resize_edge = edge;
                g->resize_start_x = in->mouse_x; g->resize_start_y = in->mouse_y;
                g->resize_window_x = g->view[topmost_view].r.x;
                g->resize_window_y = g->view[topmost_view].r.y;
                g->resize_start_w = g->view[topmost_view].r.w;
                g->resize_start_h = g->view[topmost_view].r.h;
                g->pointer_owner = GUI_POINTER_WINDOW; g->auto_layout = 0;
            } else {
                begin_title_drag(g, &g->view[topmost_view], in);
            }
            return;
        }
    }

    int hovered_view = topmost_view >= 0 &&
                       pt_in_rect(in->mouse_x, in->mouse_y,
                                  view_content_rect(g, topmost_view))
                       ? topmost_view : -1;
    if (hovered_view >= 0 && in->wheel_delta) {
        zoom_view_at(g, hovered_view, in->mouse_x, in->mouse_y,
                     exp((double)in->wheel_delta * 0.10));
        return;
    }
    if (hovered_view >= 0 && in->mouse_middle_pressed) {
        if (in->mouse_middle_down && !in->mouse_middle_released) {
            g->view_middle_interacting = hovered_view;
            g->last_mouse_x = in->mouse_x;
            g->last_mouse_y = in->mouse_y;
            g->pointer_owner = GUI_POINTER_VIEW;
        }
        return;
    }
    if (hovered_view >= 0 && in->mouse_right_pressed) {
        if (g->selected_tool == CAD_TOOL_FACE_CREATE) {
            Rect content = view_content_rect(g, hovered_view);
            int16_t point = gui_find_nearest_point(g, hovered_view,
                                                   in->mouse_x, in->mouse_y,
                                                   content, 10);
            if (point != INVALID_INDEX && !CadCore_IsPointSelected(g->cad, point) &&
                g->cad->selection.pointCount < CAD_MAX_FACE_POINTS) CadCore_SelectPoint(g->cad, point);
            create_face_from_selection(g);
        } else {
            if (in->mouse_right_down && !in->mouse_right_released) {
                g->view_right_interacting = hovered_view;
                g->last_mouse_x = in->mouse_x; g->last_mouse_y = in->mouse_y;
                g->pointer_owner = GUI_POINTER_VIEW;
            }
        }
        return;
    }
    if (hovered_view < 0 || !in->mouse_pressed) return;

    if (g->area_select_armed) {
        g->area_select_active = 1; g->area_select_view = hovered_view;
        g->area_start_x = g->area_end_x = in->mouse_x;
        g->area_start_y = g->area_end_y = in->mouse_y;
        g->pointer_owner = GUI_POINTER_AREA_SELECT;
        if (in->mouse_released || !in->mouse_down) {
            complete_area_selection(g);
            g->pointer_owner = GUI_POINTER_NONE;
        }
        return;
    }

    switch (g->selected_tool) {
    case CAD_TOOL_POINT_SELECT: select_point_at(g, hovered_view, in->mouse_x, in->mouse_y); break;
    case CAD_TOOL_FACE_SELECT: select_face_at(g, hovered_view, in->mouse_x, in->mouse_y); break;
    case CAD_TOOL_POINT_CREATE: guided_point_click(g, hovered_view, in->mouse_x, in->mouse_y); break;
    case CAD_TOOL_FACE_CREATE: {
        Rect content = view_content_rect(g, hovered_view);
        int16_t point = gui_find_nearest_point(g, hovered_view,
                                               in->mouse_x, in->mouse_y,
                                               content, 10);
        if (point != INVALID_INDEX && !CadCore_IsPointSelected(g->cad, point) &&
            g->cad->selection.pointCount < CAD_MAX_FACE_POINTS) {
            CadCore_SelectPoint(g->cad, point);
            update_face_creation_status(g, hovered_view);
        }
        break;
    }
    case CAD_TOOL_FACE_INSERT_POINT: {
        int edge = -1;
        int16_t polygon = find_polygon_at(g, hovered_view, in->mouse_x, in->mouse_y, &edge);
        if (polygon != INVALID_INDEX) insert_point_on_edge(g, polygon, edge);
        else gui_set_status(g, "Click a face edge to insert a point");
        break;
    }
    case CAD_TOOL_FACE_COLOR: {
        int16_t polygon = find_polygon_at(g, hovered_view, in->mouse_x, in->mouse_y, NULL);
        if (polygon != INVALID_INDEX) {
            if (!history_push(g)) break;
            g->cad->data.polygons[polygon].color++;
            g->cad->isDirty = 1;
            if (!history_commit(g)) break;
            gui_set_status(g, "Face #%d color %u", polygon, g->cad->data.polygons[polygon].color);
        }
        break;
    }
    default:
        if (tool_is_transform(g->selected_tool)) {
            if (g->views[hovered_view].type == CAD_VIEW_3D) {
                gui_set_status(g, "Use Top, Front, or Right for geometry transforms");
            } else if (in->mouse_down && !in->mouse_released) {
                CadAnimationSession_BeginEdit(&g->animation, g->animation_now);
                g->point_move_active = 1; g->point_move_view = hovered_view;
                g->transform_history_pushed = 0;
                g->last_mouse_x = in->mouse_x; g->last_mouse_y = in->mouse_y;
                g->pointer_owner = GUI_POINTER_TRANSFORM;
            }
        } else {
            if (in->mouse_down && !in->mouse_released) {
                g->view_interacting = hovered_view;
                g->last_mouse_x = in->mouse_x; g->last_mouse_y = in->mouse_y;
                g->pointer_owner = GUI_POINTER_VIEW;
            }
        }
        break;
    }
}

/* ============================================================================
   GUI ELEMENTS RENDERING (2D only - menu bar, tool palette, windows, etc.)
   ============================================================================ */

static void draw_menu_bar(GuiState* g, int win_w) {
    if (!g) return;
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    rg_fill_rect(0, 0, win_w, MenuBarHeight(), (RG_Color){230,230,230,255});
    rg_line(0, MenuBarHeight(), win_w, MenuBarHeight(), (RG_Color){0,0,0,255});
    if (g->font) {
        int x = 8;
        for (int i = 0; i < g->menu_count; i++) {
            font_draw(g->font, x, 3, g->menus[i], 0);
            x += font_measure(g->font, g->menus[i]) + 16;
        }
    }
}

/* ============================================================================
   DROPDOWN MENU RENDERING (must be drawn last, on top of everything)
   ============================================================================ */


static void draw_tool_palette_overlay(GuiState* g, int win_h) {
    if (!g || !g->tool_palette_visible) return;
    draw_window_chrome(g, &g->toolPalette, win_h, 1.0f, 1.0f);
    ToolMetrics metrics = tool_metrics(g);
    for (int slot = 0; slot < CAD_TOOL_COUNT; ++slot) {
        CadToolId tool = toolPaletteOrder[slot];
        Rect button = tool_button_rect(g, slot);
        EditorCommandContext context = editor_command_context(g);
        int disabled = !EditorController_GetToolState(
            &g->controller, tool, &context).enabled;
        rg_fill_rect(button.x, button.y, button.w, button.h,
                     disabled ? (RG_Color){220,220,220,255} : (RG_Color){245,245,245,255});
        rg_stroke_rect(button.x, button.y, button.w, button.h, (RG_Color){120,120,120,255});
        if (g->selected_tool == tool) {
            rg_fill_rect(button.x + 1, button.y + 1, button.w - 2, button.h - 2,
                         (RG_Color){190,210,235,255});
        }
        if (g->tool_icons[tool]) {
            int x = button.x + (button.w - metrics.icon_w) / 2;
            int y = button.y + (button.h - metrics.icon_h) / 2;
            if (g->selected_tool == tool) rg_draw_texture(g->tool_icons[tool], x, y, metrics.icon_w, metrics.icon_h);
            else rg_draw_texture_inverted(g->tool_icons[tool], x, y, metrics.icon_w, metrics.icon_h);
        }
        if (disabled) rg_line(button.x + 3, button.y + 3, button.x + button.w - 3,
                              button.y + button.h - 3, (RG_Color){130,130,130,255});
    }
}

static void draw_coordinates_window(GuiState* g, int win_h) {
    Rect inner;
    char coordinates[128];
    char panel_text[256];
    if (!g || !g->coordinates_visible) return;
    draw_window_chrome(g, &g->coordBox, win_h, 1.0f, 1.0f);
    inner = (Rect){g->coordBox.r.x + 6, g->coordBox.r.y + 26,
                   g->coordBox.r.w - 12, g->coordBox.r.h - 32};
    rg_fill_rect(inner.x, inner.y, inner.w, inner.h,
                 (RG_Color){250,250,250,255});
    rg_stroke_rect(inner.x, inner.y, inner.w, inner.h,
                   (RG_Color){120,120,120,255});
    if (!g->font || !g->cad) return;

    if (g->cad->selection.pointCount > 0) {
        double average_x = 0.0;
        double average_y = 0.0;
        double average_z = 0.0;
        int valid_count = 0;
        int all_same_location = 1;
        const double location_threshold = 0.01;
        for (int index = 0; index < g->cad->selection.pointCount; ++index) {
            int16_t point_index = g->cad->selection.selectedPoints[index];
            CadPosition point;
            if (point_index < 0 || !gui_scene_point(g, point_index, &point))
                continue;
            average_x += point.x;
            average_y += point.y;
            average_z += point.z;
            ++valid_count;
        }
        if (valid_count > 0) {
            average_x /= valid_count;
            average_y /= valid_count;
            average_z /= valid_count;
            if (valid_count > 1) {
                for (int index = 0;
                     index < g->cad->selection.pointCount; ++index) {
                    int16_t point_index =
                        g->cad->selection.selectedPoints[index];
                    CadPosition point;
                    double dx;
                    double dy;
                    double dz;
                    if (point_index < 0 ||
                        !gui_scene_point(g, point_index, &point)) continue;
                    dx = point.x - average_x;
                    dy = point.y - average_y;
                    dz = point.z - average_z;
                    if (dx * dx + dy * dy + dz * dz >
                        location_threshold * location_threshold) {
                        all_same_location = 0;
                        break;
                    }
                }
            }
            if (valid_count == 1 || all_same_location) {
                snprintf(coordinates, sizeof(coordinates),
                         "X=%.2f   Y=%.2f   Z=%.2f", average_x, average_y,
                         average_z);
            } else {
                snprintf(coordinates, sizeof(coordinates),
                         "X=%.2f   Y=%.2f   Z=%.2f  (avg of %d)", average_x,
                         average_y, average_z, valid_count);
            }
        } else {
            snprintf(coordinates, sizeof(coordinates),
                     "No valid points selected");
        }
    } else {
        snprintf(coordinates, sizeof(coordinates), "No points selected");
    }
    snprintf(panel_text, sizeof(panel_text),
             "%s   |   3D RX=%.1f RY=%.1f RZ=%.1f  Zoom=%.2fx",
             coordinates, g->views[1].rot_x, g->views[1].rot_y,
             g->views[1].rot_z, g->views[1].zoom);
    font_draw(g->font, inner.x + 8, inner.y + 6, panel_text, 0);
}

static int command_checked(const GuiState* g, CadCommandId command) {
    EditorCommandContext context = editor_command_context(g);
    return EditorController_GetCommandState(
        &g->controller, command, &context).checked;
}

static void draw_state_panel(GuiState* g, int win_h) {
    static const char* const group_labels[5] = {
        "Translation", "Rotation center", "Rotation degrees",
        "Scale center", "Scale factors"
    };
    static const char* const axis_labels[3] = { "X", "Y", "Z" };
    if (!g || !g->state_visible) return;
    draw_window_chrome(g, &g->stateWindow, win_h, 1.0f, 1.0f);
    Rect inner = { g->stateWindow.r.x + 6, g->stateWindow.r.y + 26,
                   g->stateWindow.r.w - 12, g->stateWindow.r.h - 32 };
    rg_fill_rect(inner.x, inner.y, inner.w, inner.h, (RG_Color){246,246,246,255});
    rg_stroke_rect(inner.x, inner.y, inner.w, inner.h, (RG_Color){120,120,120,255});

    if (g->font) {
        char summary[192];
        int selected = g->state_face_target ? g->cad->selection.polygonCount
                                            : g->cad->selection.pointCount;
        snprintf(summary, sizeof(summary), "%s target: %d selected%s",
                 g->state_face_target ? "Face" : "Point", selected,
                 CadDocument_HasAnimation(&g->document)
                     ? (g->animation.allFrames ? " (all corresponding frames)"
                                               : " (current frame)")
                     : "");
        font_draw(g->font, inner.x + 8, inner.y + 7, summary, 0);
    }

    for (int row = 0; row < 5; ++row) {
        Rect first = state_field_rect(g, row * 3);
        if (g->font) font_draw(g->font, inner.x + 8, first.y + 5, group_labels[row], 0);
        for (int axis = 0; axis < 3; ++axis) {
            int field = row * 3 + axis;
            Rect value = state_field_rect(g, field);
            Rect minus = state_minus_rect(g, field);
            Rect plus = state_plus_rect(g, field);
            RG_Color value_fill = field == g->state_active_field
                                  ? (RG_Color){210,224,248,255}
                                  : (RG_Color){255,255,255,255};
            rg_fill_rect(value.x, value.y, value.w, value.h, value_fill);
            rg_stroke_rect(value.x, value.y, value.w, value.h,
                           field == g->state_active_field
                               ? (RG_Color){45,90,170,255}
                               : (RG_Color){110,110,110,255});
            rg_fill_rect(minus.x, minus.y, minus.w, minus.h, (RG_Color){226,226,226,255});
            rg_stroke_rect(minus.x, minus.y, minus.w, minus.h, (RG_Color){110,110,110,255});
            rg_fill_rect(plus.x, plus.y, plus.w, plus.h, (RG_Color){226,226,226,255});
            rg_stroke_rect(plus.x, plus.y, plus.w, plus.h, (RG_Color){110,110,110,255});
            if (g->font) {
                font_draw(g->font, value.x - 13, value.y + 5, axis_labels[axis], 0);
                font_draw(g->font, value.x + 5, value.y + 5, g->state_values[field], 0);
                font_draw(g->font, minus.x + 7, minus.y + 4, "-", 0);
                font_draw(g->font, plus.x + 6, plus.y + 4, "+", 0);
            }
        }
    }

    Rect apply = state_apply_rect(g);
    Rect cancel = state_cancel_rect(g);
    rg_fill_rect(apply.x, apply.y, apply.w, apply.h, (RG_Color){195,218,198,255});
    rg_stroke_rect(apply.x, apply.y, apply.w, apply.h, (RG_Color){70,110,75,255});
    rg_fill_rect(cancel.x, cancel.y, cancel.w, cancel.h, (RG_Color){226,226,226,255});
    rg_stroke_rect(cancel.x, cancel.y, cancel.w, cancel.h, (RG_Color){110,110,110,255});
    if (g->font) {
        font_draw(g->font, apply.x + 19, apply.y + 5, "Apply", 0);
        font_draw(g->font, cancel.x + 16, cancel.y + 5, "Close", 0);
        font_draw(g->font, inner.x + 8, inner.y + inner.h - 48,
                  "Tab changes field; Enter applies; Escape closes.", 0);
    }
}

static void draw_menu_item(GuiState* g, const CadMenuItemDescriptor* item,
                           Rect row, int hovered) {
    char dynamic_label[160];
    const char* label;
    if (item->flags & CAD_MENU_ITEM_SEPARATOR) {
        rg_line(row.x + 6, row.y + row.h / 2, row.x + row.w - 6,
                row.y + row.h / 2, (RG_Color){120,120,120,255});
        return;
    }
    int enabled = menu_item_enabled(g, item);
    if (hovered && enabled) rg_fill_rect(row.x + 1, row.y, row.w - 2, row.h,
                                         (RG_Color){205,215,235,255});
    if (!enabled) {
        rg_fill_rect(row.x + 2, row.y + 1, row.w - 4, row.h - 2,
                     (RG_Color){232,232,232,255});
    }
    if (command_checked(g, item->command)) {
        rg_fill_rect(row.x + 7, row.y + 7, 6, 6, (RG_Color){30,30,30,255});
    }
    label = menu_item_display_text(g, item, dynamic_label,
                                   sizeof(dynamic_label));
    if (g->font) font_draw(g->font, row.x + 18, row.y + 3, label, 0);
    if (item->flags & CAD_MENU_ITEM_SUBMENU) {
        rg_line(row.x + row.w - 10, row.y + 6, row.x + row.w - 5, row.y + 10,
                (RG_Color){0,0,0,255});
        rg_line(row.x + row.w - 5, row.y + 10, row.x + row.w - 10, row.y + 14,
                (RG_Color){0,0,0,255});
    }
}

static void gui_draw_dropdown(GuiState* g) {
    if (!g || g->menu_open < 0) return;
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    const MenuDescriptor* menu = menu_for_index(g->menu_open);
    if (!menu) return;
    Rect popup = menu_popup_rect(g, g->menu_open);
    rg_fill_rect(popup.x, popup.y, popup.w, popup.h, (RG_Color){245,245,245,255});
    rg_stroke_rect(popup.x, popup.y, popup.w, popup.h, (RG_Color){0,0,0,255});
    for (int i = 0; i < menu->count; ++i) {
        draw_menu_item(g, &menu->items[i],
                       (Rect){popup.x, popup.y + i * 20, popup.w, 20},
                       i == g->menu_hover_item);
    }
    if (g->submenu_open == 5 || g->submenu_open == 6) {
        const CadMenuItemDescriptor* submenu = g->submenu_open == 5
                                                ? importSubMenuItems : exportSubMenuItems;
        int submenu_count = g->submenu_open == 5
                                ? ARRAY_COUNT(importSubMenuItems)
                                : ARRAY_COUNT(exportSubMenuItems);
        Rect r = g->submenu_rect;
        rg_fill_rect(r.x, r.y, r.w, r.h, (RG_Color){245,245,245,255});
        rg_stroke_rect(r.x, r.y, r.w, r.h, (RG_Color){0,0,0,255});
        for (int i = 0; i < submenu_count; ++i) {
            draw_menu_item(g, &submenu[i], (Rect){r.x, r.y + i * 20, r.w, 20},
                           i == g->submenu_hover_item);
        }
    }
}

/* ============================================================================
   CAD MODEL RENDERING (3D with depth testing)
   ============================================================================ */

static void gui_draw_view_info_bar(GuiState* g, int view_idx, const GuiInput* in) {
    if (!g || !g->font || !in || view_idx < 0 || view_idx >= 4) return;
    
    Rect content = view_content_rect(g, view_idx);
    Rect window = g->view[view_idx].r;
    
    /* Check if mouse is over this view's content area */
    if (!pt_in_rect(in->mouse_x, in->mouse_y, content)) {
        return; /* Don't show info if mouse not over view */
    }
    
    /* Calculate mouse position relative to viewport (content area) */
    int vp_x = in->mouse_x - content.x;
    int vp_y = in->mouse_y - content.y;
    
    /* Unproject mouse coordinates to world coordinates */
    double world_x, world_y, world_z;
    CadView_UnprojectPoint(&g->views[view_idx], vp_x, vp_y, content.w, content.h,
                          &world_x, &world_y, &world_z);
    
    /* Format coordinate string */
    char coord_str[128];
    snprintf(coord_str, sizeof(coord_str), "X:%.2f  Y:%.2f  Z:%.2f", world_x, world_y, world_z);

    /* Live coordinates belong to the title chrome, which already owns input,
       rather than obscuring an editable strip of the model viewport. */
    int text_width = font_measure(g->font, coord_str);
    int info_x = window.x + window.w - text_width - 7;
    if (info_x < window.x + 70) info_x = window.x + 70;
    rg_fill_rect(info_x - 3, window.y + 1, window.x + window.w - info_x + 1, 18,
                 (RG_Color){210,210,210,255});
    font_draw(g->font, info_x, window.y + 2, coord_str, 0);
}

static void gui_draw_cad_views(GuiState* g, int win_w, int win_h, int fb_w, int fb_h, const GuiInput* in) {
    if (!g || !g->cad) return;
    
    /* Calculate scale factors for coordinate conversion */
    float scale_x = (fb_w > 0 && win_w > 0) ? (float)fb_w / (float)win_w : 1.0f;
    float scale_y = (fb_h > 0 && win_h > 0) ? (float)fb_h / (float)win_h : 1.0f;
    
    /* Render CAD data in each view */
    for (int slot = 0; slot < 4; ++slot) {
        int i = g->view_z_order[slot];
        if (i < 0 || i >= 4) continue;
        if (!g->view_visible[i]) continue;
        /* Each view is painted as a complete desktop window in input z-order.
           Later views therefore cover earlier views without their model
           content punching through another window's chrome. */
        draw_window_chrome(g, &g->view[i], win_h, 1.0f, 1.0f);
        Rect content = view_content_rect(g, i);
        if (content.w <= 1 || content.h <= 1) continue;
        
        /* OpenGL clips in framebuffer pixels, while model projection stays in
           the same logical coordinate system used by hit testing and dragging. */
        int scaled_x = (int)lroundf((float)content.x * scale_x);
        int scaled_y = (int)lroundf((float)content.y * scale_y);
        int scaled_right = (int)lroundf((float)(content.x + content.w) * scale_x);
        int scaled_bottom = (int)lroundf((float)(content.y + content.h) * scale_y);
        int scaled_w = scaled_right - scaled_x;
        int scaled_h = scaled_bottom - scaled_y;
        
        /* Enable depth testing for this viewport */
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_TRUE);
        
        /* Clear depth buffer for this viewport only */
        glEnable(GL_SCISSOR_TEST);
        int gl_y = fb_h - (scaled_y + scaled_h);
        glScissor(scaled_x, gl_y, scaled_w, scaled_h);
        glClearDepth(1.0);
        glClear(GL_DEPTH_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
        
        if (g->scene_valid) {
            CadView_RenderScene(&g->views[i], &g->scene,
                                scaled_x, scaled_y, scaled_w, scaled_h, fb_h,
                                content.w, content.h);
        } else {
            CadView_Render(&g->views[i], g->cad,
                           scaled_x, scaled_y, scaled_w, scaled_h, fb_h,
                           content.w, content.h);
        }
        
        /* Reset to 2D after CAD rendering - restore main viewport/projection */
        rg_reset_viewport(win_w, win_h, fb_w, fb_h);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        gui_draw_interaction_overlays(g, i);
        
        /* Draw info bar for this view */
        if (in) {
            gui_draw_view_info_bar(g, i, in);
        }
        draw_view_scrollbars(view_client_rect(g, i), &g->views[i]);
    }
}

static void gui_draw_interaction_overlays(GuiState* g, int view_index) {
    if (!g || view_index < 0 || view_index >= 4) return;
    Rect content = view_content_rect(g, view_index);
    if (g->area_select_active && g->area_select_view == view_index) {
        int left = g->area_start_x < g->area_end_x ? g->area_start_x : g->area_end_x;
        int right = g->area_start_x > g->area_end_x ? g->area_start_x : g->area_end_x;
        int top = g->area_start_y < g->area_end_y ? g->area_start_y : g->area_end_y;
        int bottom = g->area_start_y > g->area_end_y ? g->area_start_y : g->area_end_y;
        if (left < content.x) left = content.x;
        if (right >= content.x + content.w) right = content.x + content.w - 1;
        if (top < content.y) top = content.y;
        if (bottom >= content.y + content.h) bottom = content.y + content.h - 1;
        if (right >= left && bottom >= top) {
            rg_stroke_rect(left, top, right - left, bottom - top,
                           (RG_Color){35,85,210,255});
        }
    }
    if (g->point_pending) {
        unsigned axes = axes_for_view(g->views[view_index].type);
        unsigned horizontal_axis = 0;
        unsigned vertical_axis = 0;
        int x, y;
        if (g->views[view_index].type == CAD_VIEW_TOP) {
            horizontal_axis = 1u; vertical_axis = 4u;
        } else if (g->views[view_index].type == CAD_VIEW_FRONT) {
            horizontal_axis = 1u; vertical_axis = 2u;
        } else if (g->views[view_index].type == CAD_VIEW_RIGHT) {
            horizontal_axis = 4u; vertical_axis = 2u;
        }
        if (axes && horizontal_axis && vertical_axis) {
            CadView_ProjectPoint(&g->views[view_index], g->point_pending_x,
                                 g->point_pending_y, g->point_pending_z,
                                 &x, &y, content.w, content.h);
            x += content.x;
            y += content.y;
            if ((g->point_known_axes & horizontal_axis) &&
                x >= content.x && x < content.x + content.w) {
                rg_line(x, content.y, x, content.y + content.h - 1,
                        (RG_Color){218,92,44,255});
            }
            if ((g->point_known_axes & vertical_axis) &&
                y >= content.y && y < content.y + content.h) {
                rg_line(content.x, y, content.x + content.w - 1, y,
                        (RG_Color){218,92,44,255});
            }
            if ((g->point_known_axes & axes) == axes &&
                x >= content.x && x < content.x + content.w &&
                y >= content.y && y < content.y + content.h) {
                rg_stroke_rect(x - 4, y - 4, 8, 8, (RG_Color){220,45,45,255});
            }
        }
    }

    if (g->selected_tool == CAD_TOOL_FACE_CREATE &&
        g->cad->selection.pointCount > 0) {
        int count = g->cad->selection.pointCount;
        int px[CAD_MAX_FACE_POINTS];
        int py[CAD_MAX_FACE_POINTS];
        double coords[CAD_MAX_FACE_POINTS][3];
        int valid = count <= CAD_MAX_FACE_POINTS;
        for (int i = 0; valid && i < count; ++i) {
            int16_t point_index = g->cad->selection.selectedPoints[i];
            CadPosition point;
            if (!gui_scene_point(g, point_index, &point)) {
                valid = 0;
                break;
            }
            coords[i][0] = point.x;
            coords[i][1] = point.y;
            coords[i][2] = point.z;
            CadView_ProjectPoint(&g->views[view_index], point.x,
                                 point.y, point.z,
                                 &px[i], &py[i], content.w, content.h);
            px[i] += content.x;
            py[i] += content.y;
        }
        if (valid) {
            const RG_Color chain_color = { 125, 50, 190, 255 };
            CadPointChainPlanarity planarity =
                CadGeometry_ClassifyPointChain(coords, count, 0.01);
            const RG_Color close_color =
                planarity == CAD_POINT_CHAIN_COPLANAR
                    ? (RG_Color){ 30, 150, 85, 255 }
                    : planarity == CAD_POINT_CHAIN_DEGENERATE
                        ? (RG_Color){ 215, 132, 35, 255 }
                        : (RG_Color){ 210, 55, 45, 255 };
            for (int i = 1; i < count; ++i) {
                if (pt_in_rect(px[i - 1], py[i - 1], content) &&
                    pt_in_rect(px[i], py[i], content)) {
                    rg_line(px[i - 1], py[i - 1], px[i], py[i], chain_color);
                }
            }
            if (count >= 3 && pt_in_rect(px[count - 1], py[count - 1], content) &&
                pt_in_rect(px[0], py[0], content)) {
                rg_line(px[count - 1], py[count - 1], px[0], py[0], close_color);
            }
            for (int i = 0; i < count; ++i) {
                char number[8];
                Rect label;
                if (!pt_in_rect(px[i], py[i], content)) continue;
                snprintf(number, sizeof(number), "%d", i + 1);
                label = (Rect){px[i] + 5, py[i] - 11, 18, 15};
                if (label.w > content.w || label.h > content.h) continue;
                if (label.x < content.x) label.x = content.x;
                if (label.y < content.y) label.y = content.y;
                if (label.x + label.w > content.x + content.w)
                    label.x = content.x + content.w - label.w;
                if (label.y + label.h > content.y + content.h)
                    label.y = content.y + content.h - label.h;
                rg_fill_rect(label.x, label.y, label.w, label.h,
                             (RG_Color){ 250, 247, 230, 255 });
                rg_stroke_rect(label.x, label.y, label.w, label.h, chain_color);
                if (g->font)
                    font_draw(g->font, label.x + 3, label.y + 1, number, 0);
            }
        }
    }
}

static void draw_animation_button(GuiState* g, Rect rect, const char* text,
                                  int checked, int enabled) {
    RG_Color fill = !enabled ? (RG_Color){218,218,218,255}
                    : checked ? (RG_Color){166,202,238,255}
                              : (RG_Color){244,244,244,255};
    RG_Color edge = enabled ? (RG_Color){82,82,82,255}
                            : (RG_Color){160,160,160,255};
    if (rect.w <= 0 || rect.h <= 0) return;
    rg_fill_rect(rect.x, rect.y, rect.w, rect.h, fill);
    rg_stroke_rect(rect.x, rect.y, rect.w, rect.h, edge);
    if (g->font && text) {
        int width = font_measure(g->font, text);
        int x = rect.x + (rect.w - width) / 2;
        if (x < rect.x + 2) x = rect.x + 2;
        font_draw(g->font, x, rect.y + 3, text, enabled ? 0 : 1);
    }
}

static void draw_animation_panel(GuiState* g) {
    AnimationPanelLayout layout;
    CadAnimationPanelState state;
    int frames;
    if (!g || !g->animation_visible || g->animationWindow.r.w <= 0 ||
        g->animationWindow.r.h <= 0) return;
    layout = animation_panel_layout(g);
    state = animation_panel_state(g);
    frames = g->animation_info_valid && g->animation_info.frameCount > 0
                 ? g->animation_info.frameCount : 0;
    rg_fill_rect(layout.inner.x, layout.inner.y, layout.inner.w, layout.inner.h,
                 (RG_Color){242,242,242,255});
    rg_stroke_rect(layout.inner.x, layout.inner.y, layout.inner.w, layout.inner.h,
                   (RG_Color){120,120,120,255});
    if (!layout.usable) {
        if (g->font)
            font_draw(g->font, layout.inner.x + 6, layout.inner.y + 4,
                      "Animation timeline: enlarge or float this panel", 0);
        return;
    }

    draw_animation_button(g, layout.first, "|<", 0,
                          CadAnimationPanel_IsActionEnabled(
                              CAD_ANIMATION_PANEL_FIRST, &state));
    draw_animation_button(g, layout.previous, "<", 0,
                          CadAnimationPanel_IsActionEnabled(
                              CAD_ANIMATION_PANEL_PREVIOUS, &state));
    draw_animation_button(g, layout.play,
                          g->animation.playing ? "Pause" : "Play",
                          g->animation.playing,
                          CadAnimationPanel_IsActionEnabled(
                              CAD_ANIMATION_PANEL_PLAY_PAUSE, &state));
    draw_animation_button(g, layout.stop, "Stop", 0,
                          CadAnimationPanel_IsActionEnabled(
                              CAD_ANIMATION_PANEL_STOP, &state));
    draw_animation_button(g, layout.next, ">", 0,
                          CadAnimationPanel_IsActionEnabled(
                              CAD_ANIMATION_PANEL_NEXT, &state));
    draw_animation_button(g, layout.last, ">|", 0,
                          CadAnimationPanel_IsActionEnabled(
                              CAD_ANIMATION_PANEL_LAST, &state));
    draw_animation_button(g, layout.loop, "Loop", g->animation.loop,
                          CadAnimationPanel_IsActionEnabled(
                              CAD_ANIMATION_PANEL_TOGGLE_LOOP, &state));
    draw_animation_button(g, layout.interpolation, "Interp",
                          g->animation.interpolation,
                          CadAnimationPanel_IsActionEnabled(
                              CAD_ANIMATION_PANEL_TOGGLE_INTERPOLATION,
                              &state));
    draw_animation_button(g, layout.fps_down, "FPS-", 0,
                          CadAnimationPanel_IsActionEnabled(
                              CAD_ANIMATION_PANEL_FPS_DOWN, &state));
    draw_animation_button(g, layout.fps_up, "FPS+", 0,
                          CadAnimationPanel_IsActionEnabled(
                              CAD_ANIMATION_PANEL_FPS_UP, &state));
    draw_animation_button(g, layout.all_frames, "All Frames",
                          g->animation.allFrames,
                          CadAnimationPanel_IsActionEnabled(
                              CAD_ANIMATION_PANEL_TOGGLE_ALL_FRAMES, &state));
    if (layout.dock.w)
        draw_animation_button(g, layout.dock,
                              g->animation_docked ? "Float" : "Dock", 0,
                              CadAnimationPanel_IsActionEnabled(
                                  CAD_ANIMATION_PANEL_TOGGLE_DOCK, &state));

    if (g->font) {
        char readout[96];
        snprintf(readout, sizeof(readout), "F %.2f / %d  %.0f fps",
                 g->animation.previewFrame, frames,
                 g->animation.fps);
        font_draw(g->font, layout.last.x + layout.last.w + 7,
                  layout.last.y + 3, readout, 0);
    }

    for (int frame = 0; frame < CAD_ANIMATION_FRAMES; ++frame) {
        int left = layout.strip.x + frame * layout.strip.w / CAD_ANIMATION_FRAMES;
        int right = layout.strip.x + (frame + 1) * layout.strip.w / CAD_ANIMATION_FRAMES;
        Rect cell = {left, layout.strip.y, right - left, layout.strip.h};
        int active = frame < frames;
        int current = frame == g->animation.currentFrame && active;
        int sampled = g->scene_valid && active &&
                      (frame == g->scene.pose->sample.frameA ||
                       frame == g->scene.pose->sample.frameB);
        RG_Color fill = !active ? (RG_Color){222,222,222,255}
                        : current ? (RG_Color){56,116,188,255}
                        : sampled ? (RG_Color){137,190,223,255}
                                  : (RG_Color){250,250,250,255};
        rg_fill_rect(cell.x, cell.y, cell.w, cell.h, fill);
        rg_stroke_rect(cell.x, cell.y, cell.w, cell.h,
                       (RG_Color){135,135,135,255});
        if (g->font && active && (frame % 8 == 0 || current) && cell.w >= 8) {
            char number[8];
            snprintf(number, sizeof(number), "%d", frame);
            font_draw(g->font, cell.x + 1, cell.y + 2, number, current ? 1 : 0);
        }
    }
    if (g->scene_valid && g->scene.pose->sample.interpolated && frames > 1) {
        double frame_width = (double)layout.strip.w / CAD_ANIMATION_FRAMES;
        int marker = layout.strip.x +
            (int)lround((g->animation.previewFrame + 0.5) * frame_width);
        rg_line(marker, layout.strip.y, marker,
                layout.strip.y + layout.strip.h - 1,
                (RG_Color){220,54,43,255});
    }

    draw_animation_button(g, layout.create_all, "Create All", 0,
                          CadAnimationPanel_IsActionEnabled(
                              CAD_ANIMATION_PANEL_CREATE_ALL, &state));
    draw_animation_button(g, layout.create_selected, "Create Sel", 0,
                          CadAnimationPanel_IsActionEnabled(
                              CAD_ANIMATION_PANEL_CREATE_SELECTED, &state));
    draw_animation_button(g, layout.count_down, "Count -", 0,
                          CadAnimationPanel_IsActionEnabled(
                              CAD_ANIMATION_PANEL_COUNT_DOWN, &state));
    draw_animation_button(g, layout.count_up, "Count +", 0,
                          CadAnimationPanel_IsActionEnabled(
                              CAD_ANIMATION_PANEL_COUNT_UP, &state));
    draw_animation_button(g, layout.insert, "Insert", 0,
                          CadAnimationPanel_IsActionEnabled(
                              CAD_ANIMATION_PANEL_INSERT, &state));
    draw_animation_button(g, layout.duplicate, "Duplicate", 0,
                          CadAnimationPanel_IsActionEnabled(
                              CAD_ANIMATION_PANEL_DUPLICATE, &state));
    draw_animation_button(g, layout.delete_frame, "Delete", 0,
                          CadAnimationPanel_IsActionEnabled(
                              CAD_ANIMATION_PANEL_DELETE, &state));
    draw_animation_button(g, layout.copy_all, "Copy All",
                          g->anim_copy_mode == 1,
                          CadAnimationPanel_IsActionEnabled(
                              CAD_ANIMATION_PANEL_COPY_ALL, &state));
    draw_animation_button(g, layout.copy_selected, "Copy Sel",
                          g->anim_copy_mode == 2,
                          CadAnimationPanel_IsActionEnabled(
                              CAD_ANIMATION_PANEL_COPY_SELECTED, &state));
    draw_animation_button(g, layout.add_faces, "Add Faces", 0,
                          CadAnimationPanel_IsActionEnabled(
                              CAD_ANIMATION_PANEL_ADD_FACES, &state));
    draw_animation_button(g, layout.static_copy, "Make Static Copy", 0,
                          CadAnimationPanel_IsActionEnabled(
                              CAD_ANIMATION_PANEL_MAKE_STATIC_COPY, &state));
    if (g->font) {
        char summary[144];
        int x = layout.static_copy.x + layout.static_copy.w + 8;
        if (state.editable) {
            snprintf(summary, sizeof(summary), "%d animated / %d static face%s%s",
                     g->animation_info.animatedFaceCount,
                     g->animation_info.staticFaceCount,
                     (g->animation_info.unattachedIndexCount ||
                      g->animation_info.unattachedPointCount)
                         ? "; unattached records preserved" : "",
                     g->anim_copy_mode ? "; choose target frame" : "");
        } else if (CadAnimation_HasAny(&g->cad->data)) {
            snprintf(summary, sizeof(summary), "Unattached animation records preserved");
        } else {
            snprintf(summary, sizeof(summary), "Static document");
        }
        if (layout.static_copy.w > 0 &&
            x < layout.inner.x + layout.inner.w - 20)
            font_draw(g->font, x, layout.static_copy.y + 3, summary, 0);
    }
}

static void draw_shape_browser_window(GuiState* g, int win_w, int win_h,
                                      int fb_w, int fb_h) {
    ShapeBrowserLayout layout;
    int result_count;
    int visible_items;
    int max_scroll;
    Rect preview_view;
    RG_Color replace_fill;
    if (!g || !aux_window_visible(g, GUI_AUX_SHAPE_BROWSER)) return;
    draw_window_chrome(g, &g->shapeBrowserWindow, win_h, 1.0f, 1.0f);
    layout = shape_browser_layout(g);
    rg_fill_rect(layout.inner.x, layout.inner.y, layout.inner.w, layout.inner.h,
                 (RG_Color){250,250,250,255});
    rg_stroke_rect(layout.inner.x, layout.inner.y, layout.inner.w,
                   layout.inner.h, (RG_Color){120,120,120,255});
    result_count = filtered_shape_count(g);
    visible_items = layout.list.h / 20;
    max_scroll = result_count > visible_items
                     ? result_count - visible_items : 0;
    if (g->shape_scroll_offset > max_scroll)
        g->shape_scroll_offset = max_scroll;
    if (g->shape_scroll_offset < 0) g->shape_scroll_offset = 0;

    if (g->font) {
        char title[160];
        snprintf(title, sizeof(title),
                 "Recovered ASM shapes: %d total, %d matching",
                 g->shape_count, result_count);
        font_draw(g->font, layout.inner.x + 8, layout.inner.y + 7, title, 0);
        font_draw(g->font, layout.inner.x + 8, layout.inner.y + 27,
                  g->shape_folder_path, 0);
        font_draw(g->font, layout.inner.x + 8, layout.search.y + 5,
                  "Find:", 0);
    }

    rg_fill_rect(layout.search.x, layout.search.y, layout.search.w,
                 layout.search.h,
                 g->shape_search_active ? (RG_Color){210,224,248,255}
                                        : (RG_Color){255,255,255,255});
    rg_stroke_rect(layout.search.x, layout.search.y, layout.search.w,
                   layout.search.h,
                   g->shape_search_active ? (RG_Color){45,90,170,255}
                                          : (RG_Color){110,110,110,255});
    if (g->font) {
        font_draw(g->font, layout.search.x + 5, layout.search.y + 5,
                  g->shape_search[0] ? g->shape_search : "type to filter...",
                  0);
    }

    rg_fill_rect(layout.list.x, layout.list.y, layout.list.w, layout.list.h,
                 (RG_Color){255,255,255,255});
    rg_stroke_rect(layout.list.x, layout.list.y, layout.list.w, layout.list.h,
                   (RG_Color){80,80,80,255});
    for (int row = 0; row < visible_items; ++row) {
        int source_index = filtered_shape_index(
            g, g->shape_scroll_offset + row);
        Rect item;
        if (source_index < 0) break;
        item = (Rect){layout.list.x + 2, layout.list.y + row * 20 + 1,
                      layout.list.w - (max_scroll > 0 ? 16 : 4), 18};
        if (source_index == g->shape_selected) {
            rg_fill_rect(item.x, item.y, item.w, item.h,
                         (RG_Color){190,210,240,255});
        }
        if (g->font) {
            font_draw(g->font, item.x + 3, item.y + 3,
                      g->shape_names[source_index], 0);
        }
    }

    if (max_scroll > 0) {
        Rect track = {layout.list.x + layout.list.w - 14, layout.list.y,
                      14, layout.list.h};
        int thumb_h = visible_items * track.h / result_count;
        int thumb_y;
        if (thumb_h < 24) thumb_h = 24;
        thumb_y = track.y + g->shape_scroll_offset *
                  (track.h - thumb_h) / max_scroll;
        rg_fill_rect(track.x, track.y, track.w, track.h,
                     (RG_Color){220,220,220,255});
        rg_stroke_rect(track.x, track.y, track.w, track.h,
                       (RG_Color){100,100,100,255});
        rg_fill_rect(track.x + 2, thumb_y, track.w - 4, thumb_h,
                     (RG_Color){145,145,145,255});
    }

    rg_fill_rect(layout.preview.x, layout.preview.y, layout.preview.w,
                 layout.preview.h, (RG_Color){238,238,238,255});
    rg_stroke_rect(layout.preview.x, layout.preview.y, layout.preview.w,
                   layout.preview.h, (RG_Color){80,80,80,255});
    if (g->font) {
        char preview_title[160];
        if (g->shape_preview_valid && g->shape_selected >= 0) {
            snprintf(preview_title, sizeof(preview_title),
                     "%s - %d points, %d faces",
                     g->shape_names[g->shape_selected],
                     g->shape_preview->data.pointCount,
                     g->shape_preview->data.polygonCount);
        } else {
            snprintf(preview_title, sizeof(preview_title),
                     "Select a shape to preview");
        }
        font_draw(g->font, layout.preview.x + 6, layout.preview.y + 5,
                  preview_title, 0);
    }

    preview_view = (Rect){layout.preview.x + 4, layout.preview.y + 25,
                          layout.preview.w - 8, layout.preview.h - 29};
    if (g->shape_preview_valid && preview_view.w > 1 && preview_view.h > 1) {
        float scale_x = win_w > 0 ? (float)fb_w / (float)win_w : 1.0f;
        float scale_y = win_h > 0 ? (float)fb_h / (float)win_h : 1.0f;
        int pixel_x = (int)lroundf(preview_view.x * scale_x);
        int pixel_y = (int)lroundf(preview_view.y * scale_y);
        int pixel_right =
            (int)lroundf((preview_view.x + preview_view.w) * scale_x);
        int pixel_bottom =
            (int)lroundf((preview_view.y + preview_view.h) * scale_y);
        int pixel_w = pixel_right - pixel_x;
        int pixel_h = pixel_bottom - pixel_y;
        int gl_y = fb_h - (pixel_y + pixel_h);
        if (gl_y < 0) gl_y = 0;
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_SCISSOR_TEST);
        glScissor(pixel_x, gl_y, pixel_w, pixel_h);
        glClearDepth(1.0);
        glClear(GL_DEPTH_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
        CadView_Render(&g->shape_preview_view, g->shape_preview,
                       pixel_x, pixel_y, pixel_w, pixel_h, fb_h,
                       preview_view.w, preview_view.h);
        rg_reset_viewport(win_w, win_h, fb_w, fb_h);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        rg_stroke_rect(preview_view.x, preview_view.y, preview_view.w,
                       preview_view.h, (RG_Color){100,100,100,255});
    }

    replace_fill = g->shape_preview_valid
                       ? (RG_Color){195,218,198,255}
                       : (RG_Color){225,225,225,255};
    rg_fill_rect(layout.replace_button.x, layout.replace_button.y,
                 layout.replace_button.w, layout.replace_button.h,
                 replace_fill);
    rg_stroke_rect(layout.replace_button.x, layout.replace_button.y,
                   layout.replace_button.w, layout.replace_button.h,
                   (RG_Color){100,100,100,255});
    if (g->font) {
        font_draw(g->font, layout.replace_button.x + 20,
                  layout.replace_button.y + 5, "Replace", 0);
    }
}

static void draw_aux_windows(GuiState* g, int win_w, int win_h,
                             int fb_w, int fb_h) {
    if (!g) return;
    for (int slot = 0; slot < GUI_AUX_COUNT; ++slot) {
        int id = g->aux_z_order[slot];
        if (!aux_window_visible(g, id)) continue;
        switch ((GuiAuxWindowId)id) {
        case GUI_AUX_TOOL_PALETTE:
            draw_tool_palette_overlay(g, win_h);
            break;
        case GUI_AUX_COORDINATES:
            draw_coordinates_window(g, win_h);
            break;
        case GUI_AUX_ANIMATION:
            draw_window_chrome(g, &g->animationWindow, win_h, 1.0f, 1.0f);
            draw_animation_panel(g);
            break;
        case GUI_AUX_SHAPE_BROWSER:
            draw_shape_browser_window(g, win_w, win_h, fb_w, fb_h);
            break;
        default:
            break;
        }
    }
}

/* ============================================================================
   MAIN DRAW FUNCTION
   ============================================================================ */

void gui_draw(GuiState* g, const GuiInput* in, int win_w, int win_h, int fb_w, int fb_h) {
    if (!g) return;

    /* Use framebuffer size for viewport (physical pixels) */
    /* Use window size for projection (logical pixels) */
    glViewport(0, 0, fb_w, fb_h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_SCISSOR_TEST);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    /* Projection uses window size so coordinates work correctly */
    glOrtho(0.0, (double)win_w, (double)win_h, 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    
    /* Clear depth buffer once per frame before any 3D drawing */
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);

    /* Modeling views are the base desktop layer.  Auxiliary panels are then
       painted as complete windows in their shared back-to-front order. */
    gui_draw_cad_views(g, win_w, win_h, fb_w, fb_h, in);
    draw_aux_windows(g, win_w, win_h, fb_w, fb_h);

    /* The persistent menu and status surfaces stay above all desktop
       windows, regardless of their focus order. */
    draw_menu_bar(g, win_w);
    /* Persistent contextual status; dropdowns remain the topmost layer. */
    int status_y = win_h - 22;
    rg_fill_rect(0, status_y, win_w, 22, (RG_Color){225,225,225,255});
    rg_line(0, status_y, win_w, status_y, (RG_Color){120,120,120,255});
    if (g->font) {
        font_draw(g->font, 8, status_y + 4, g->status_text, 0);
        if (g->document.isDirty) {
            const char* modified = "Modified";
            int width = font_measure(g->font, modified);
            font_draw(g->font, win_w - width - 10, status_y + 4, modified, 0);
        }
    }

    /* The numeric transform panel is modal over all desktop windows, while
       dropdown menus remain the topmost input and paint layer. */
    draw_state_panel(g, win_h);

    /* Step 4: Draw dropdown menu last (on top of everything) */
    gui_draw_dropdown(g);
    
    /* Final reset to ensure clean state */
    rg_reset_viewport(win_w, win_h, fb_w, fb_h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
}


