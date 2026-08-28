#define _CRT_SECURE_NO_WARNINGS

#include "gui.h"
#include "render_gl.h"
#include "font_win32.h"
#include "cad_core.h"
#include "file_dialog.h"
#include "cad_view.h"
#include "cad_export_obj.h"
#include "cad_export_3dg1.h"
#include "cad_import_3dg1.h"
#include "cad_import_obj.h"
#include "cad_document.h"
#include "editor_commands.h"
#include "editor_tool.h"
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

typedef struct Rect {
    int x, y, w, h;
} Rect;

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
    GUI_POINTER_SCROLLBAR
} GuiPointerOwner;

struct GuiState {
    FontWin32* font;
    GuiCommand pending_command;

    /* CAD core */
    CadDocument document;
    CadCore* cad; /* Stable convenience alias for document.core. */
    EditorTool edit_tool;
    
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
    int tool_palette_visible;
    int coordinates_visible;

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
    
    /* Animation state */
    int anim_current_frame; /* Current frame number (0-based) */
    int anim_total_frames;  /* Total number of frames */
    int anim_playing;        /* 1 if playing, 0 if paused */
    int anim_loop;          /* 1 if looping, 0 if not */
};

static int MenuBarHeight(void) { return 20; }

/* Forward declarations */
static void scan_asm_folder_for_shapes(GuiState* g, const char* folder_path);
static int load_shape_from_asm(CadCore* core, const char* shape_name, const char* folder_path);
static void load_all_constants(const char* shapes_folder);

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
    { CAD_COMMAND_FILE_IMPORT_OBJ, ".obj (Wavefront)", 0 }
};

/* Export submenu */
static const CadMenuItemDescriptor exportSubMenuItems[] = {
    { CAD_COMMAND_FILE_EXPORT_3DG1, ".3dg1 (Fundoshi)", 0 },
    { CAD_COMMAND_FILE_EXPORT_OBJ, ".obj (Wavefront)", 0 }
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
    { CAD_COMMAND_OPTION_SELECT_ALL, "Select All", 0 },
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
    const uint8_t* source = NULL;
    if (g->document.paletteDataSize == CAD_PALETTE_DATA_SIZE) source = g->document.paletteData;
    else if (g->document.colorDataSize == CAD_COLOR_DATA_SIZE) source = g->document.colorData;
    if (!source) {
        for (int i = 0; i < 4; ++i) CadView_ClearPalette(&g->views[i]);
        return;
    }
    uint8_t rgba[256][4];
    for (int i = 0; i < 256; ++i) {
        unsigned word = (unsigned)source[i * 2] | ((unsigned)source[i * 2 + 1] << 8);
        rgba[i][0] = (uint8_t)(((word >> 0) & 31u) * 255u / 31u);
        rgba[i][1] = (uint8_t)(((word >> 5) & 31u) * 255u / 31u);
        rgba[i][2] = (uint8_t)(((word >> 10) & 31u) * 255u / 31u);
        rgba[i][3] = 255;
    }
    for (int i = 0; i < 4; ++i) CadView_SetPalette(&g->views[i], &rgba[0][0]);
}

static void reset_interaction(GuiState* g) {
    if (!g) return;
    /* A document edit is inseparable from the gesture that owns it.  Any
       workflow reset must roll that gesture back before clearing capture, so
       menus, file dialogs, and document replacement cannot strand a partial
       drag or a stale EditorTool phase. */
    if (EditorTool_IsActive(&g->edit_tool) || g->document.transactionBefore) {
        EditorTool_Cancel(&g->edit_tool);
        g->cad = &g->document.core;
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
}

static void history_clear(GuiState* g) {
    if (!g) return;
    CadDocument_ClearHistory(&g->document);
}

static int history_push(GuiState* g) {
    if (!g || !g->cad) return 0;
    if (EditorTool_IsActive(&g->edit_tool) || g->document.transactionBefore)
        EditorTool_Cancel(&g->edit_tool);
    CadResult result = EditorTool_Begin(&g->edit_tool, g->selected_tool);
    if (!CadResult_IsSuccess(&result)) {
        gui_set_status(g, "Edit cancelled: could not create an undo snapshot (%s)",
                       CadStatus_Name(result.status));
        return 0;
    }
    return 1;
}

static int history_commit(GuiState* g) {
    if (!g || !g->document.transactionBefore) return 0;
    CadResult result = EditorTool_Update(&g->edit_tool);
    if (CadResult_IsSuccess(&result)) result = EditorTool_Commit(&g->edit_tool);
    else EditorTool_Cancel(&g->edit_tool);
    g->cad = &g->document.core;
    if (!CadResult_IsSuccess(&result)) {
        gui_set_status(g, "Edit rolled back: could not commit its undo snapshot (%s)",
                       CadStatus_Name(result.status));
        return 0;
    }
    return 1;
}

static void history_cancel(GuiState* g) {
    if (!g) return;
    if (EditorTool_IsActive(&g->edit_tool) || g->document.transactionBefore)
        EditorTool_Cancel(&g->edit_tool);
    g->cad = &g->document.core;
}

static int history_undo(GuiState* g) {
    if (!g || !CadDocument_CanUndo(&g->document)) {
        gui_set_status(g, "Nothing to undo");
        return 0;
    }
    CadDocument_Undo(&g->document);
    g->cad = &g->document.core;
    apply_document_palette(g);
    reset_interaction(g);
    gui_set_status(g, "Undo");
    return 1;
}

static int history_redo(GuiState* g) {
    if (!g || !CadDocument_CanRedo(&g->document)) {
        gui_set_status(g, "Nothing to redo");
        return 0;
    }
    CadDocument_Redo(&g->document);
    g->cad = &g->document.core;
    apply_document_palette(g);
    reset_interaction(g);
    gui_set_status(g, "Redo");
    return 1;
}

static int save_document_as(GuiState* g) {
    char filename[GUI_PATH_CAPACITY];
    if (!g || !g->cad || !FileDialog_SaveCAD(filename, sizeof(filename))) return 0;
    CadResult result = CadDocument_Save(&g->document, filename);
    if (!CadResult_IsSuccess(&result)) {
        gui_set_status(g, "Could not save %s", filename);
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
        gui_set_status(g, "Could not save %s", g->document.savePath);
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
    EditorTool_BindDocument(&g->edit_tool, &g->document);
    g->document.core = *replacement;
    g->cad = &g->document.core;
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
static void editor_polygon_sort(GuiState* g);
static void layout_cleanup(GuiState* g, int win_w, int win_h);
static void activate_tool(GuiState* g, CadToolId tool);
static void state_panel_open(GuiState* g);
static void state_panel_close(GuiState* g);
static int state_panel_apply(GuiState* g);
static void state_panel_key(GuiState* g, int key, unsigned modifiers);
static void gui_draw_interaction_overlays(GuiState* g, int view_index);

static FILE* gui_fopen_utf8(const char* path, const char* mode) {
    if (!path || !mode) return NULL;
#ifdef _WIN32
    wchar_t wide_path[GUI_PATH_CAPACITY * 2];
    wchar_t wide_mode[16];
    int length = MultiByteToWideChar(CP_UTF8, 0, path, -1, wide_path, ARRAY_COUNT(wide_path));
    int mode_length = MultiByteToWideChar(CP_UTF8, 0, mode, -1, wide_mode, ARRAY_COUNT(wide_mode));
    return length > 0 && mode_length > 0 ? _wfopen(wide_path, wide_mode) : NULL;
#else
    return fopen(path, mode);
#endif
}

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

static size_t read_binary_file_utf8(const char* path, void* buffer, size_t capacity) {
    FILE* file = gui_fopen_utf8(path, "rb");
    if (!file) return 0;
    size_t read = fread(buffer, 1, capacity, file);
    int extra = fgetc(file);
    fclose(file);
    return extra == EOF ? read : 0;
}

static size_t read_binary_prefix_utf8(const char* path, void* buffer, size_t required,
                                      int* had_trailing_data) {
    FILE* file = gui_fopen_utf8(path, "rb");
    if (had_trailing_data) *had_trailing_data = 0;
    if (!file) return 0;
    size_t read = fread(buffer, 1, required, file);
    if (read == required) {
        int extra = fgetc(file);
        if (had_trailing_data) *had_trailing_data = extra != EOF;
    }
    fclose(file);
    return read;
}

static void execute_editor_command(GuiState* g, CadCommandId command) {
    char filename[GUI_PATH_CAPACITY];
    if (!g || !g->cad) return;

    if (EditorTool_IsActive(&g->edit_tool) || g->document.transactionBefore)
        reset_interaction(g);

    switch (command) {
    case CAD_COMMAND_FILE_NEW:
        if (confirm_replace_document(g, "creating a new document")) {
            CadDocument_New(&g->document);
            EditorTool_BindDocument(&g->edit_tool, &g->document);
            g->cad = &g->document.core;
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
                gui_set_status(g, "Could not open %s; current document was not changed", filename);
            } else if (confirm_replace_document(g, "opening another document")) {
                CadDocument_Destroy(&g->document);
                g->document = *temp;
                memset(temp, 0, sizeof(*temp));
                EditorTool_BindDocument(&g->edit_tool, &g->document);
                g->cad = &g->document.core;
                reset_interaction(g);
                for (int i = 0; i < 4; ++i) CadView_Reset(&g->views[i]);
                apply_document_palette(g);
                gui_set_status(g, result.format == CAD_FORMAT_LEGACY_PACKED
                    ? "Imported legacy CAD %s; use Save As" : "Opened %s", filename);
            }
            CadDocument_Destroy(temp);
            free(temp);
        }
        break;
    case CAD_COMMAND_FILE_SAVE: save_document(g); break;
    case CAD_COMMAND_FILE_SAVE_AS: save_document_as(g); break;
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
            int ok = is_obj ? CadExport_OBJ(g->cad, filename) : CadExport_3DG1(g->cad, filename);
            if (ok) CadDocument_SetLastExportPath(&g->document, filename);
            gui_set_status(g, ok ? "Exported %s" : "Export failed: %s", filename);
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
            void* target = palette ? (void*)g->document.paletteData : (void*)g->document.colorData;
            size_t capacity = palette ? sizeof(g->document.paletteData) : sizeof(g->document.colorData);
            int ignored_trailing_data = 0;
            if (!history_push(g)) break;
            size_t size = palette
                          ? read_binary_file_utf8(filename, target, capacity)
                          : read_binary_prefix_utf8(filename, target, capacity,
                                                    &ignored_trailing_data);
            if (size != capacity) {
                history_cancel(g);
                gui_set_status(g, palette
                    ? "%s must be exactly 0x%zX bytes"
                    : "%s must contain at least 0x%zX bytes",
                    filename, capacity);
            } else {
                if (palette) g->document.paletteDataSize = size;
                else g->document.colorDataSize = size;
                if (!history_commit(g)) break;
                apply_document_palette(g);
                if (ignored_trailing_data) {
                    gui_set_status(g, "Loaded first %zu color bytes from %s; trailing data ignored",
                                   size, filename);
                } else {
                    gui_set_status(g, "Loaded %zu bytes from %s", size, filename);
                }
            }
        }
        break;
    }
    case CAD_COMMAND_FILE_ANIMATION:
        if (g->animationWindow.r.w > 0) {
            g->animationWindow.r.w = g->animationWindow.r.h = 0;
        } else {
            g->animationWindow.r = (Rect){ 420, 180, 430, 150 };
        }
        gui_set_status(g, "Animation records are preserved; editing is deferred");
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
    case CAD_COMMAND_WINDOW_COORDINATES: g->coordinates_visible = !g->coordinates_visible; g->auto_layout = 1; break;
    case CAD_COMMAND_WINDOW_TOOL_PALETTE: g->tool_palette_visible = !g->tool_palette_visible; g->auto_layout = 1; break;
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
        for (int i = 0; i < 4; ++i) CadView_Reset(&g->views[i]);
        apply_document_palette(g);
        gui_set_status(g, "View cameras returned home");
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
    case CAD_COMMAND_MERGE_ALL: editor_grid_merge(g); editor_point_merge(g); editor_polygon_merge(g); break;
    case CAD_COMMAND_POLYGON_SORT: editor_polygon_sort(g); break;
    default: break;
    }
}

GuiState* gui_create(void) {
    GuiState* g = (GuiState*)calloc(1, sizeof(GuiState));
    if (!g) return NULL;

    CadDocument_Init(&g->document);
    g->cad = &g->document.core;
    EditorTool_Init(&g->edit_tool, &g->document);
    
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
    for (int i = 0; i < 4; ++i) g->view_visible[i] = 1;
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
    g->animationWindow = (GuiWin){ "ANIMATION", { 500, 200, 430, 150 }, 1 };
    g->animationWindow.r.w = 0; /* Start hidden (width 0) */
    g->animationWindow.r.h = 0; /* Start hidden (height 0) */
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
    
    /* Initialize animation state */
    g->anim_current_frame = 0;
    g->anim_total_frames = 0;
    g->anim_playing = 0;
    g->anim_loop = 0;

    return g;
}

void gui_destroy(GuiState* g) {
    if (!g) return;
    EditorTool_Cancel(&g->edit_tool);
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

/* Scan ASM files in a folder for shape definitions */
static void scan_asm_folder_for_shapes(GuiState* g, const char* folder_path) {
    if (!g || !folder_path) return;
    
    /* Free existing shape names */
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
    g->shape_selected = -1;
    g->shape_scroll_offset = 0;
    g->shape_search_active = 0;
    g->shape_search[0] = '\0';
    g->shape_preview_valid = 0;
    CadCore_Clear(g->shape_preview);
    strncpy(g->shape_folder_path, folder_path, sizeof(g->shape_folder_path) - 1);
    g->shape_folder_path[sizeof(g->shape_folder_path) - 1] = '\0';
    
    /* Load constants from INC files for resolving symbolic values */
    load_all_constants(folder_path);
    
#ifdef _WIN32
    wchar_t wide_folder[GUI_PATH_CAPACITY * 2];
    wchar_t search_path[GUI_PATH_CAPACITY * 2];
    if (!gui_utf8_to_wide(folder_path, wide_folder, ARRAY_COUNT(wide_folder))) {
        gui_set_status(g, "Shape folder path is not valid UTF-8");
        return;
    }
    if (_snwprintf_s(search_path, ARRAY_COUNT(search_path), _TRUNCATE,
                     L"%ls\\*.asm", wide_folder) < 0) {
        gui_set_status(g, "Shape folder path is too long");
        return;
    }
    WIN32_FIND_DATAW find_data;
    HANDLE hFind = FindFirstFileW(search_path, &find_data);
    
    if (hFind == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "No ASM files found in: %s\n", folder_path);
        return;
    }
    
    /* First pass: count shapes */
    int shape_capacity = 256;
    g->shape_names = (char**)calloc(shape_capacity, sizeof(char*));
    if (!g->shape_names) {
        FindClose(hFind);
        return;
    }
    
    do {
        if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            wchar_t file_path[GUI_PATH_CAPACITY * 2];
            if (_snwprintf_s(file_path, ARRAY_COUNT(file_path), _TRUNCATE,
                             L"%ls\\%ls", wide_folder, find_data.cFileName) < 0) continue;
            
            /* Read file and extract shape names */
            FILE* f = _wfopen(file_path, L"rb");
            if (f) {
                char line[1024];
                while (fgets(line, sizeof(line), f)) {
                    /* Look for shape_P pattern (points section indicates a shape)
                       Pattern: shape_name_p (case insensitive, can have whitespace before) */
                    char line_lower[1024];
                    strncpy(line_lower, line, sizeof(line_lower) - 1);
                    line_lower[sizeof(line_lower) - 1] = '\0';
                    for (int k = 0; line_lower[k]; k++) {
                        line_lower[k] = (char)tolower((unsigned char)line_lower[k]);
                    }
                    
                    char* p_pos = strstr(line_lower, "_p");
                    if (p_pos) {
                        /* Check that after _p there's whitespace or end of line (not _p1, _p2, etc.) */
                        char after_p = p_pos[2];
                        if (after_p == '\0' || after_p == '\n' || after_p == '\r' || isspace((unsigned char)after_p)) {
                            /* Extract shape name (everything before _p) */
                            char* start = line;
                            while (*start && isspace((unsigned char)*start)) start++;
                            char* p_pos_orig = line + (p_pos - line_lower);
                            if (start < p_pos_orig) {
                                char shape_name[128];
                                const size_t name_len = (size_t)(p_pos_orig - start);
                                if (name_len > 0 && name_len < sizeof(shape_name)) {
                                    memcpy(shape_name, start, name_len);
                                    shape_name[name_len] = '\0';
                                
                                /* Check if we already have this shape */
                                int found = 0;
                                for (int i = 0; i < g->shape_count; i++) {
                                    if (g->shape_names[i] && strcmp(g->shape_names[i], shape_name) == 0) {
                                        found = 1;
                                        break;
                                    }
                                }
                                
                                if (!found) {
                                    /* Add shape name */
                                    if (g->shape_count >= shape_capacity) {
                                        shape_capacity *= 2;
                                        char** expanded = (char**)realloc(
                                            g->shape_names, shape_capacity * sizeof(char*));
                                        if (!expanded) {
                                            fclose(f);
                                            FindClose(hFind);
                                            return;
                                        }
                                        g->shape_names = expanded;
                                    }
                                    g->shape_names[g->shape_count] = (char*)malloc(name_len + 1);
                                    if (g->shape_names[g->shape_count]) {
                                        strcpy(g->shape_names[g->shape_count], shape_name);
                                        g->shape_count++;
                                    }
                                    }
                                }
                            }
                        }
                    }
                }
                fclose(f);
            }
        }
    } while (FindNextFileW(hFind, &find_data));
    
    
    FindClose(hFind);
#else
    /* Non-Windows: Use dirent */
    DIR* dir = opendir(folder_path);
    if (!dir) {
        fprintf(stderr, "Cannot open folder: %s\n", folder_path);
        return;
    }
    
    int shape_capacity = 256;
    g->shape_names = (char**)calloc(shape_capacity, sizeof(char*));
    if (!g->shape_names) {
        closedir(dir);
        return;
    }
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            const char* name = entry->d_name;
            int len = strlen(name);
            if (len > 4 && strcasecmp(name + len - 4, ".asm") == 0) {
                char file_path[GUI_PATH_CAPACITY * 2];
                snprintf(file_path, sizeof(file_path), "%s/%s", folder_path, name);
                
                FILE* f = fopen(file_path, "r");
                if (f) {
                    char line[1024];
                    while (fgets(line, sizeof(line), f)) {
                        char line_lower[1024];
                        strncpy(line_lower, line, sizeof(line_lower) - 1);
                        line_lower[sizeof(line_lower) - 1] = '\0';
                        for (int k = 0; line_lower[k]; k++) {
                            line_lower[k] = (char)tolower((unsigned char)line_lower[k]);
                        }
                        
                        char* p_pos = strstr(line_lower, "_p");
                        if (p_pos) {
                            /* Check that after _p there's whitespace or end of line */
                            char after_p = p_pos[2];
                            if (after_p == '\0' || after_p == '\n' || after_p == '\r' || isspace((unsigned char)after_p)) {
                                char* start = line;
                                while (*start && isspace((unsigned char)*start)) start++;
                                char* p_pos_orig = line + (p_pos - line_lower);
                                if (start < p_pos_orig) {
                                    int name_len = p_pos_orig - start;
                                    if (name_len > 0 && name_len < 128) {
                                        char shape_name[128];
                                        strncpy(shape_name, start, name_len);
                                        shape_name[name_len] = '\0';
                                        
                                        int found = 0;
                                        for (int i = 0; i < g->shape_count; i++) {
                                            if (g->shape_names[i] && strcmp(g->shape_names[i], shape_name) == 0) {
                                                found = 1;
                                                break;
                                            }
                                        }
                                        
                                        if (!found) {
                                            if (g->shape_count >= shape_capacity) {
                                                shape_capacity *= 2;
                                                char** expanded = (char**)realloc(
                                                    g->shape_names, shape_capacity * sizeof(char*));
                                                if (!expanded) {
                                                    fclose(f);
                                                    closedir(dir);
                                                    return;
                                                }
                                                g->shape_names = expanded;
                                            }
                                            g->shape_names[g->shape_count] = (char*)malloc(name_len + 1);
                                            if (g->shape_names[g->shape_count]) {
                                                strcpy(g->shape_names[g->shape_count], shape_name);
                                                g->shape_count++;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    fclose(f);
                }
            }
        }
    }
    closedir(dir);
#endif
    
    /* Sort shape names alphabetically */
    for (int i = 0; i < g->shape_count - 1; i++) {
        for (int j = i + 1; j < g->shape_count; j++) {
            if (g->shape_names[i] && g->shape_names[j] && 
                strcmp(g->shape_names[i], g->shape_names[j]) > 0) {
                char* temp = g->shape_names[i];
                g->shape_names[i] = g->shape_names[j];
                g->shape_names[j] = temp;
            }
        }
    }
    
    fprintf(stdout, "Found %d shapes in folder: %s\n", g->shape_count, folder_path);
    
    /* Show shape browser window */
    if (g->shape_count > 0) {
        int width = g->layout_width > 20 && g->layout_width - 20 < 640
                    ? g->layout_width - 20 : 640;
        int height = g->layout_height > 70 && g->layout_height - 50 < 520
                     ? g->layout_height - 50 : 520;
        if (width < 420) width = 420;
        if (height < 360) height = 360;
        int x = g->layout_width > width ? (g->layout_width - width) / 2 : 8;
        int y = g->layout_height > height ? (g->layout_height - height) / 2
                                          : MenuBarHeight() + 4;
        if (y < MenuBarHeight() + 4) y = MenuBarHeight() + 4;
        g->shapeBrowserWindow.r = (Rect){ x, y, width, height };
        gui_set_status(g, "Found %d ASM shapes; search and select one to preview",
                       g->shape_count);
    }
}

/* Simple JSON parser to extract shape-to-file mapping from Shapes.SFEOPTIM */
static char* find_shape_file_in_json(const char* json_content, const char* shape_name) {
    if (!json_content || !shape_name) return NULL;
    
    /* Build search pattern: "SHAPE_NAME":" */
    char pattern[512];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", shape_name);
    
    /* Find the pattern in JSON */
    char* pos = strstr(json_content, pattern);
    if (!pos) return NULL;
    
    /* Skip past the pattern to get to the filename */
    pos += strlen(pattern);
    
    /* Extract filename until closing quote */
    static char filename[GUI_PATH_CAPACITY];
    size_t filename_length = 0;
    while (*pos && *pos != '"' && filename_length + 1 < sizeof(filename)) {
        filename[filename_length++] = *pos++;
    }
    filename[filename_length] = '\0';
    
    if (filename_length == 0) return NULL;
    
    return filename;
}

/* ========== Constant Resolver for ASM symbolic constants ========== */

#define MAX_CONSTANTS 4096
#define MAX_CONST_NAME 64

typedef struct {
    char name[MAX_CONST_NAME];
    int value;
    int resolved;  /* 1 if value is final, 0 if needs resolution */
} AsmConstant;

typedef struct {
    AsmConstant constants[MAX_CONSTANTS];
    int count;
} ConstantTable;

static ConstantTable g_constants = { .count = 0 };
static ConstantTable g_constant_baseline = { .count = 0 };

static void constants_clear(void) {
    g_constants.count = 0;
}

static int constants_find(const char* name) {
    for (int i = 0; i < g_constants.count; i++) {
        if (_stricmp(g_constants.constants[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static void constants_add(const char* name, int value) {
    if (g_constants.count >= MAX_CONSTANTS) return;
    
    /* Check if already exists */
    int idx = constants_find(name);
    if (idx >= 0) {
        g_constants.constants[idx].value = value;
        g_constants.constants[idx].resolved = 1;
        return;
    }
    
    strncpy(g_constants.constants[g_constants.count].name, name, MAX_CONST_NAME - 1);
    g_constants.constants[g_constants.count].name[MAX_CONST_NAME - 1] = '\0';
    g_constants.constants[g_constants.count].value = value;
    g_constants.constants[g_constants.count].resolved = 1;
    g_constants.count++;
}

static int constants_get(const char* name, int* out_value) {
    int idx = constants_find(name);
    if (idx >= 0 && g_constants.constants[idx].resolved) {
        *out_value = g_constants.constants[idx].value;
        return 1;
    }
    return 0;
}

/* Parse a value that may be a number, constant name, or expression */
static int parse_const_value(const char* str, int* out_value) {
    if (!str || !out_value) return 0;
    
    /* Skip leading whitespace */
    while (*str == ' ' || *str == '\t') str++;
    
    /* Check for negated constant: -constantname */
    if (*str == '-' && isalpha((unsigned char)str[1])) {
        /* It's a negated constant like -size */
        str++; /* skip the minus */
        char const_name[MAX_CONST_NAME];
        int name_len = 0;
        while (isalnum((unsigned char)*str) || *str == '_') {
            if (name_len < MAX_CONST_NAME - 1) {
                const_name[name_len++] = *str;
            }
            str++;
        }
        const_name[name_len] = '\0';
        
        int val;
        if (!constants_get(const_name, &val)) {
            return 0; /* Can't resolve */
        }
        *out_value = -val;
        return 1;
    }
    
    /* If it starts with a digit or minus followed by digit, it's a number */
    if (isdigit((unsigned char)*str) || (*str == '-' && isdigit((unsigned char)str[1]))) {
        char* end;
        long val = strtol(str, &end, 10);
        
        /* Check for operators in the expression */
        while (*end == '+' || *end == '-' || *end == '*') {
            char op = *end++;
            while (*end == ' ' || *end == '\t') end++;
            
            /* Next part could be a number or constant */
            long next_val;
            if (isdigit((unsigned char)*end) || (*end == '-' && isdigit((unsigned char)end[1]))) {
                next_val = strtol(end, &end, 10);
            } else {
                /* It's a constant name - extract it */
                char const_name[MAX_CONST_NAME];
                int name_len = 0;
                while (isalnum((unsigned char)*end) || *end == '_') {
                    if (name_len < MAX_CONST_NAME - 1) {
                        const_name[name_len++] = *end;
                    }
                    end++;
                }
                const_name[name_len] = '\0';
                
                int const_val;
                if (!constants_get(const_name, &const_val)) {
                    return 0; /* Can't resolve */
                }
                next_val = const_val;
            }
            
            if (op == '+') val += next_val;
            else if (op == '-') val -= next_val;
            else if (op == '*') val *= next_val;
        }
        
        *out_value = (int)val;
        return 1;
    }
    
    /* It starts with a letter - it's a constant name or expression starting with constant */
    char const_name[MAX_CONST_NAME];
    int name_len = 0;
    const char* p = str;
    while (isalnum((unsigned char)*p) || *p == '_') {
        if (name_len < MAX_CONST_NAME - 1) {
            const_name[name_len++] = *p;
        }
        p++;
    }
    const_name[name_len] = '\0';
    
    int val;
    if (!constants_get(const_name, &val)) {
        return 0; /* Can't resolve */
    }
    
    /* Check for operators after the constant */
    while (*p == ' ' || *p == '\t') p++;
    while (*p == '+' || *p == '-' || *p == '*') {
        char op = *p++;
        while (*p == ' ' || *p == '\t') p++;
        
        long next_val;
        if (isdigit((unsigned char)*p) || (*p == '-' && isdigit((unsigned char)p[1]))) {
            char* end;
            next_val = strtol(p, &end, 10);
            p = end;
        } else {
            /* Another constant */
            name_len = 0;
            while (isalnum((unsigned char)*p) || *p == '_') {
                if (name_len < MAX_CONST_NAME - 1) {
                    const_name[name_len++] = *p;
                }
                p++;
            }
            const_name[name_len] = '\0';
            
            int const_val;
            if (!constants_get(const_name, &const_val)) {
                return 0;
            }
            next_val = const_val;
        }
        
        if (op == '+') val += (int)next_val;
        else if (op == '-') val -= (int)next_val;
        else if (op == '*') val *= (int)next_val;
    }
    
    *out_value = val;
    return 1;
}

/* Parse a single line for constant definition */
static void parse_constant_line(const char* line) {
    /* Format: name equ value  OR  name = value */
    char line_copy[512];
    strncpy(line_copy, line, sizeof(line_copy) - 1);
    line_copy[sizeof(line_copy) - 1] = '\0';
    
    /* Skip leading whitespace */
    char* p = line_copy;
    while (*p == ' ' || *p == '\t') p++;
    
    /* Skip comments */
    if (*p == ';' || *p == '\0' || *p == '\n' || *p == '\r') return;
    
    /* Extract name */
    char name[MAX_CONST_NAME];
    int name_len = 0;
    while (isalnum((unsigned char)*p) || *p == '_') {
        if (name_len < MAX_CONST_NAME - 1) {
            name[name_len++] = *p;
        }
        p++;
    }
    name[name_len] = '\0';
    if (name_len == 0) return;
    
    /* Skip whitespace */
    while (*p == ' ' || *p == '\t') p++;
    
    /* Check for 'equ' or '=' */
    int is_equ = 0;
    if (_strnicmp(p, "equ", 3) == 0 && (p[3] == ' ' || p[3] == '\t')) {
        p += 3;
        is_equ = 1;
    } else if (*p == '=') {
        p++;
        is_equ = 1;
    }
    
    if (!is_equ) return;
    
    /* Skip whitespace */
    while (*p == ' ' || *p == '\t') p++;
    
    /* Remove trailing comment */
    char* comment = strchr(p, ';');
    if (comment) *comment = '\0';
    
    /* Remove trailing whitespace */
    int len = (int)strlen(p);
    while (len > 0 && (p[len-1] == ' ' || p[len-1] == '\t' || p[len-1] == '\n' || p[len-1] == '\r')) {
        p[--len] = '\0';
    }
    
    /* Try to parse the value */
    int value;
    if (parse_const_value(p, &value)) {
        constants_add(name, value);
    }
}

/* Load constants from an INC file */
static void load_constants_from_file(const char* filepath) {
    FILE* f = gui_fopen_utf8(filepath, "rb");
    if (!f) return;
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char* content = (char*)malloc(size + 1);
    if (!content) {
        fclose(f);
        return;
    }
    
    size_t read = fread(content, 1, size, f);
    fclose(f);
    content[read] = '\0';
    
    /* Parse line by line - do multiple passes to resolve dependencies */
    for (int pass = 0; pass < 3; pass++) {
        char* line = content;
        while (*line) {
            char* next = strchr(line, '\n');
            if (next) {
                *next = '\0';
                parse_constant_line(line);
                line = next + 1;
            } else {
                parse_constant_line(line);
                break;
            }
        }
        
        /* Reset for next pass */
        /* Reread line breaks */
        for (size_t i = 0; i < read; i++) {
            if (content[i] == '\0' && i + 1 < read) content[i] = '\n';
        }
    }
    
    free(content);
}

/* Load all constants from INC folder */
static void load_all_constants(const char* shapes_folder) {
    constants_clear();
    
    /* Build path to INC folder - go up one level from SHAPES */
    char inc_path[GUI_PATH_CAPACITY * 2];
    strncpy(inc_path, shapes_folder, sizeof(inc_path) - 1);
    inc_path[sizeof(inc_path) - 1] = '\0';
    
    /* Remove trailing slash if present */
    int len = (int)strlen(inc_path);
    while (len > 0 && (inc_path[len-1] == '/' || inc_path[len-1] == '\\')) {
        inc_path[--len] = '\0';
    }
    
    /* Go up one directory (from SHAPES to SF) */
    char* last_sep = strrchr(inc_path, '\\');
    if (!last_sep) last_sep = strrchr(inc_path, '/');
    if (last_sep) {
        *last_sep = '\0';
        strncat(inc_path, "\\INC", sizeof(inc_path) - strlen(inc_path) - 1);
    } else {
        strncat(inc_path, "\\..\\INC", sizeof(inc_path) - strlen(inc_path) - 1);
    }
    
    fprintf(stdout, "load_all_constants: Looking for INC folder at '%s'\n", inc_path);
    
    char filepath[GUI_PATH_CAPACITY * 2];
    
    /* Load INC files in order of dependency (most basic first) */
    const char* inc_files[] = {
        "STRATEQU.INC",  /* Shape-related constants */
        "VARS.INC",      /* Variables */
        "STRUCTS.INC",   /* Structure definitions */
        "MACROS.INC",    /* Macros */
        NULL
    };
    
    for (int f = 0; inc_files[f] != NULL; f++) {
        snprintf(filepath, sizeof(filepath), "%s\\%s", inc_path, inc_files[f]);
        load_constants_from_file(filepath);
    }
    
    /* Shape ASM assignments are intentionally not added here: many are local
       to one ShapeHdr and reusing them globally can silently corrupt a later
       preview.  Each selected shape overlays its locals on this INC baseline. */
    g_constant_baseline = g_constants;
    fprintf(stdout, "load_all_constants: Loaded %d constants\n", g_constants.count);
}

/* ========== End Constant Resolver ========== */

/* Helper: Create a polygon with its own point chain (points are copied, not shared) */
/* max_vertices: maximum valid vertex index (for bounds checking) */
static int16_t create_polygon_with_points_safe(CadCore* core, double vertices[][3],
                                                int vertex_indices[], int num_vertices,
                                                uint8_t color, int max_vertices,
                                                int* hard_error) {
    if (!core || num_vertices < CAD_MIN_FACE_POINTS ||
        num_vertices > CAD_MAX_FACE_POINTS) {
        if (hard_error) *hard_error = 1;
        return INVALID_INDEX;
    }
    int16_t added[CAD_MAX_FACE_POINTS];
    int added_count = 0;
    int previous_dirty = core->isDirty;
    int16_t previous_new_point = core->newPoint;
    
    /* Create new points for this polygon and link them */
    int16_t first_point = INVALID_INDEX;
    int16_t prev_point = INVALID_INDEX;
    
    for (int i = 0; i < num_vertices; i++) {
        int v_idx = vertex_indices[i];
        /* Bounds check to prevent crashes */
        if (v_idx < 0 || v_idx >= max_vertices) {
            fprintf(stderr, "create_polygon_with_points: vertex index %d out of bounds (max %d)\n", v_idx, max_vertices);
            if (hard_error) *hard_error = 1;
            for (int rollback = 0; rollback < added_count; ++rollback) {
                memset(&core->data.points[added[rollback]], 0, sizeof(CadPoint));
                core->data.points[added[rollback]].nextPoint = INVALID_INDEX;
            }
            CadCore_RebuildDerivedState(core);
            core->isDirty = previous_dirty;
            core->newPoint = previous_new_point;
            return INVALID_INDEX;
        }
        int16_t new_pt = CadCore_AddPoint(core, vertices[v_idx][0], vertices[v_idx][1], vertices[v_idx][2]);
        if (new_pt == INVALID_INDEX) {
            if (hard_error) *hard_error = 1;
            for (int rollback = 0; rollback < added_count; ++rollback) {
                memset(&core->data.points[added[rollback]], 0, sizeof(CadPoint));
                core->data.points[added[rollback]].nextPoint = INVALID_INDEX;
            }
            CadCore_RebuildDerivedState(core);
            core->isDirty = previous_dirty;
            core->newPoint = previous_new_point;
            return INVALID_INDEX;
        }
        added[added_count++] = new_pt;
        
        if (first_point == INVALID_INDEX) {
            first_point = new_pt;
        }
        
        if (prev_point != INVALID_INDEX) {
            CadPoint* prev = CadCore_GetPoint(core, prev_point);
            if (prev) prev->nextPoint = new_pt;
        }
        
        prev_point = new_pt;
    }
    
    /* Mark last point as end of chain */
    if (prev_point != INVALID_INDEX) {
        CadPoint* last = CadCore_GetPoint(core, prev_point);
        if (last) last->nextPoint = -1;
    }
    
    /* Create the polygon */
    int16_t polygon = CadCore_AddPolygon(core, first_point, color, (uint8_t)num_vertices);
    if (polygon == INVALID_INDEX) {
        if (hard_error) *hard_error = 1;
        for (int rollback = 0; rollback < added_count; ++rollback) {
            memset(&core->data.points[added[rollback]], 0, sizeof(CadPoint));
            core->data.points[added[rollback]].nextPoint = INVALID_INDEX;
        }
        CadCore_RebuildDerivedState(core);
        core->isDirty = previous_dirty;
        core->newPoint = previous_new_point;
    }
    return polygon;
}

/* Load a shape from an ASM file into the CAD system */
static int load_shape_from_asm(CadCore* core, const char* shape_name, const char* folder_path) {
    if (!core || !shape_name || !folder_path) {
        fprintf(stderr, "load_shape_from_asm: Invalid parameters\n");
        return 0;
    }

    fprintf(stdout, "load_shape_from_asm: Looking for shape '%s' in folder '%s'\n", shape_name, folder_path);

    /* Clear existing CAD data */
    CadCore_Clear(core);
    g_constants = g_constant_baseline;

    /* Try to load the JSON mapping file first */
    char json_path[GUI_PATH_CAPACITY * 2];
    snprintf(json_path, sizeof(json_path), "%s\\Shapes.SFEOPTIM", folder_path);
    
    FILE* json_file = gui_fopen_utf8(json_path, "rb");
    char* target_filename = NULL;
    
    if (json_file) {
        fseek(json_file, 0, SEEK_END);
        long json_size = ftell(json_file);
        fseek(json_file, 0, SEEK_SET);
        
        char* json_content = (char*)malloc(json_size + 1);
        if (json_content) {
            size_t bytes_read = fread(json_content, 1, json_size, json_file);
            json_content[bytes_read] = '\0';
            
            target_filename = find_shape_file_in_json(json_content, shape_name);
            if (target_filename) {
                fprintf(stdout, "load_shape_from_asm: Found shape '%s' in file '%s' (from JSON mapping)\n", 
                        shape_name, target_filename);
            }
            
            free(json_content);
        }
        fclose(json_file);
    }
    
    /* Find the ASM file containing this shape */
    int found = 0;
#ifdef _WIN32
    wchar_t wide_folder[GUI_PATH_CAPACITY * 2];
    wchar_t search_path[GUI_PATH_CAPACITY * 2];
    if (!gui_utf8_to_wide(folder_path, wide_folder, ARRAY_COUNT(wide_folder)) ||
        _snwprintf_s(search_path, ARRAY_COUNT(search_path), _TRUNCATE,
                     L"%ls\\*.asm", wide_folder) < 0) {
        fprintf(stderr, "load_shape_from_asm: Invalid or overlong UTF-8 folder path\n");
        return 0;
    }

    WIN32_FIND_DATAW find_data;
    HANDLE hFind = FindFirstFileW(search_path, &find_data);

    if (hFind == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "load_shape_from_asm: No ASM files found in folder '%s'\n", folder_path);
        return 0;
    }

    do {
        if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            char filename_utf8[GUI_PATH_CAPACITY];
            if (!gui_wide_to_utf8(find_data.cFileName, filename_utf8,
                                  ARRAY_COUNT(filename_utf8))) continue;
            /* If we have a target filename from JSON, skip files that don't match */
            if (target_filename && _stricmp(filename_utf8, target_filename) != 0) {
                continue;
            }
            
            wchar_t file_path[GUI_PATH_CAPACITY * 2];
            if (_snwprintf_s(file_path, ARRAY_COUNT(file_path), _TRUNCATE,
                             L"%ls\\%ls", wide_folder, find_data.cFileName) < 0) continue;
            fprintf(stdout, "load_shape_from_asm: Checking file '%s'\n", filename_utf8);

            /* Open in binary mode to avoid text translation issues on Windows */
            FILE* f = _wfopen(file_path, L"rb");
            if (f) {
                fprintf(stdout, "load_shape_from_asm: Opened file '%s'\n", filename_utf8);
                /* Read entire file into memory for easier parsing */
                fseek(f, 0, SEEK_END);
                long file_size = ftell(f);
                fseek(f, 0, SEEK_SET);
                fprintf(stdout, "load_shape_from_asm: File size: %ld bytes\n", file_size);

                if (file_size <= 0) {
                    fprintf(stderr, "load_shape_from_asm: File '%s' is empty\n", filename_utf8);
                    fclose(f);
                    continue;
                }

                char* content = (char*)malloc(file_size + 1);
                if (!content) {
                    fprintf(stderr, "load_shape_from_asm: Failed to allocate memory for file '%s'\n", filename_utf8);
                    fclose(f);
                    continue;
                }
                size_t bytes_read = fread(content, 1, file_size, f);
                fclose(f);
                
                /* On Windows, text mode can cause fewer bytes to be read due to \r\n translation
                   Accept if we read at least 95% of the file (usually just a few bytes difference) */
                if (bytes_read < (size_t)(file_size * 0.95)) {
                    fprintf(stderr, "load_shape_from_asm: Failed to read file '%s' (read %zu of %ld bytes, less than 95%%)\n", 
                            filename_utf8, bytes_read, file_size);
                    free(content);
                    continue;
                }
                
                /* Null-terminate at the actual bytes read */
                content[bytes_read] = '\0';
                fprintf(stdout, "load_shape_from_asm: Successfully read %zu bytes from '%s' (file size: %ld)\n", 
                        bytes_read, filename_utf8, file_size);

                /* Normalize line endings - handle both \r\n and \n */
                /* Use bytes_read instead of file_size since we might have read fewer bytes */
                for (size_t i = 0; i < bytes_read; i++) {
                    if (content[i] == '\r') {
                        if (i + 1 < bytes_read && content[i + 1] == '\n') {
                            /* \r\n -> \n, shift remaining content left by 1 */
                            memmove(&content[i], &content[i + 1], bytes_read - i - 1);
                            bytes_read--;
                            content[bytes_read] = '\0';
                        } else {
                            /* Standalone \r -> \n */
                            content[i] = '\n';
                        }
                    }
                }

                /* Split into lines */
                char** lines = NULL;
                int line_count = 0;
                int line_capacity = 1000;
                lines = (char**)malloc(line_capacity * sizeof(char*));
                if (!lines) {
                    fprintf(stderr, "load_shape_from_asm: Failed to allocate memory for lines\n");
                    free(content);
                    continue;
                }

                char* line_start = content;
                for (size_t i = 0; i <= bytes_read; i++) {
                    if (content[i] == '\n' || content[i] == '\0') {
                        if (line_count >= line_capacity) {
                            line_capacity *= 2;
                            char** expanded = (char**)realloc(
                                lines, line_capacity * sizeof(char*));
                            if (!expanded) {
                                fprintf(stderr, "load_shape_from_asm: Failed to reallocate memory for lines\n");
                                free(lines);
                                free(content);
                                lines = NULL;
                                break;
                            }
                            lines = expanded;
                        }
                        content[i] = '\0';
                        lines[line_count++] = line_start;
                        line_start = &content[i + 1];
                    }
                }
                if (!lines) continue;
                fprintf(stdout, "load_shape_from_asm: Split file into %d lines\n", line_count);

                /* Find points_start and faces_start by parsing ShapeHdr */
                int points_start = -1;
                int faces_start = -1;
                char shape_name_lower[256];
                strncpy(shape_name_lower, shape_name, sizeof(shape_name_lower) - 1);
                shape_name_lower[sizeof(shape_name_lower) - 1] = '\0';
                for (int k = 0; shape_name_lower[k]; k++) {
                    shape_name_lower[k] = (char)tolower((unsigned char)shape_name_lower[k]);
                }

                /* First, find the ShapeHdr line to extract actual _P and _F section names */
                char actual_points_section[256] = {0};
                char actual_faces_section[256] = {0};
                int shapehdr_line = -1;
                
                for (int i = 0; i < line_count; i++) {
                    char line_lower[1024];
                    strncpy(line_lower, lines[i], sizeof(line_lower) - 1);
                    line_lower[sizeof(line_lower) - 1] = '\0';
                    for (int k = 0; line_lower[k]; k++) {
                        line_lower[k] = (char)tolower((unsigned char)line_lower[k]);
                    }
                    
                    /* Look for shapehdr with this shape name */
                    /* Format: shapename<whitespace>shapehdr<whitespace>points_section,0,faces_section,... */
                    char* stripped = line_lower;
                    while (*stripped && (*stripped == ' ' || *stripped == '\t')) stripped++;
                    
                    /* Check if line starts with shape name */
                    size_t name_len = strlen(shape_name_lower);
                    if (strncmp(stripped, shape_name_lower, name_len) == 0) {
                        char* after_name = stripped + name_len;
                        /* Must be followed by whitespace or tab, then shapehdr */
                        if (*after_name == ' ' || *after_name == '\t') {
                            while (*after_name == ' ' || *after_name == '\t') after_name++;
                            if (_strnicmp(after_name, "shapehdr", 8) == 0) {
                                shapehdr_line = i;
                                /* Parse the ShapeHdr parameters */
                                char* params = after_name + 8;
                                while (*params == ' ' || *params == '\t') params++;
                                
                                /* Extract points section name (first parameter before comma) */
                                char* comma1 = strchr(params, ',');
                                if (comma1) {
                                    int len = (int)(comma1 - params);
                                    if (len > 0 && len < 256) {
                                        strncpy(actual_points_section, params, len);
                                        actual_points_section[len] = '\0';
                                        /* Trim whitespace */
                                        while (len > 0 && (actual_points_section[len-1] == ' ' || actual_points_section[len-1] == '\t')) {
                                            actual_points_section[--len] = '\0';
                                        }
                                    }
                                    
                                    /* Skip to third parameter (faces section) */
                                    /* Format: points,0,faces,... */
                                    char* comma2 = strchr(comma1 + 1, ',');
                                    if (comma2) {
                                        char* faces_param = comma2 + 1;
                                        while (*faces_param == ' ' || *faces_param == '\t') faces_param++;
                                        char* comma3 = strchr(faces_param, ',');
                                        if (comma3) {
                                            int flen = (int)(comma3 - faces_param);
                                            if (flen > 0 && flen < 256) {
                                                strncpy(actual_faces_section, faces_param, flen);
                                                actual_faces_section[flen] = '\0';
                                                while (flen > 0 && (actual_faces_section[flen-1] == ' ' || actual_faces_section[flen-1] == '\t')) {
                                                    actual_faces_section[--flen] = '\0';
                                                }
                                            }
                                        }
                                    }
                                }
                                fprintf(stdout, "Found ShapeHdr for %s at line %d: points='%s', faces='%s'\n", 
                                        shape_name, i, actual_points_section, actual_faces_section);
                                break;
                            }
                        }
                    }
                }
                
                /* If no ShapeHdr found, fall back to default naming */
                char shape_p[256];
                char shape_f[256];
                if (actual_points_section[0]) {
                    strncpy(shape_p, actual_points_section, sizeof(shape_p) - 1);
                    shape_p[sizeof(shape_p) - 1] = '\0';
                    for (int k = 0; shape_p[k]; k++) shape_p[k] = (char)tolower((unsigned char)shape_p[k]);
                } else {
                    snprintf(shape_p, sizeof(shape_p), "%s_p", shape_name_lower);
                }
                
                if (actual_faces_section[0]) {
                    strncpy(shape_f, actual_faces_section, sizeof(shape_f) - 1);
                    shape_f[sizeof(shape_f) - 1] = '\0';
                    for (int k = 0; shape_f[k]; k++) shape_f[k] = (char)tolower((unsigned char)shape_f[k]);
                } else {
                    snprintf(shape_f, sizeof(shape_f), "%s_f", shape_name_lower);
                }

                /* Now find the actual sections */
                for (int i = 0; i < line_count; i++) {
                    char line_lower[1024];
                    strncpy(line_lower, lines[i], sizeof(line_lower) - 1);
                    line_lower[sizeof(line_lower) - 1] = '\0';
                    for (int k = 0; line_lower[k]; k++) {
                        line_lower[k] = (char)tolower((unsigned char)line_lower[k]);
                    }
                    char* stripped = line_lower;
                    while (*stripped && (*stripped == ' ' || *stripped == '\t')) stripped++;
                    
                    char* end = stripped + strlen(stripped) - 1;
                    while (end > stripped && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
                        *end = '\0';
                        end--;
                    }

                    /* Look for points section */
                    if (points_start == -1) {
                        size_t shape_p_len = strlen(shape_p);
                        if (strncmp(stripped, shape_p, shape_p_len) == 0) {
                            char after_p = stripped[shape_p_len];
                            if (!after_p || after_p == '\0' || after_p == '\n' || after_p == '\r' ||
                                after_p == ' ' || after_p == '\t') {
                                points_start = i;
                                fprintf(stdout, "Found points section for %s at line %d: %s\n", shape_name, i, stripped);
                            }
                        }
                    }
                    /* Look for faces section */
                    if (faces_start == -1) {
                        size_t shape_f_len = strlen(shape_f);
                        if (strncmp(stripped, shape_f, shape_f_len) == 0) {
                            char after_f = stripped[shape_f_len];
                            if (!after_f || !isdigit((unsigned char)after_f)) {
                                faces_start = i;
                                fprintf(stdout, "Found faces section for %s at line %d: %s\n", shape_name, i, stripped);
                            }
                        }
                    }
                }

                fprintf(stdout, "load_shape_from_asm: Searching for '%s' and '%s' in file '%s' (%d lines)\n", 
                        shape_p, shape_f, filename_utf8, line_count);
                
                if (points_start == -1) {
                    /* Shape not in this file, continue to next file (this is normal) */
                    free(lines);
                    free(content);
                    continue;
                }
                
                if (faces_start == -1) {
                    fprintf(stderr, "WARNING: Could not find faces section '%s' for shape: %s in file: %s (will continue without faces)\n", shape_f, shape_name, filename_utf8);
                }

                /* Parse points - build vertex array first */
                double (*vertices)[3] = (double (*)[3])calloc(8192, sizeof(*vertices));
                if (!vertices) {
                    fprintf(stderr, "load_shape_from_asm: Failed to allocate vertex workspace\n");
                    free(lines);
                    free(content);
                    continue;
                }
                int vertex_count = 0;
                int in_mirrored_section = 0;
                int parse_error = 0;

                /* First, parse local constants from between ShapeHdr and the first Points directive */
                /* Local constants can appear BEFORE the points section label (e.g., d = 5 before Lcube_P) */
                int const_scan_start = (shapehdr_line >= 0) ? shapehdr_line : (points_start > 10 ? points_start - 10 : 0);
                fprintf(stdout, "load_shape_from_asm: Scanning for local constants from line %d to %d\n", const_scan_start, points_start + 20);
                for (int i = const_scan_start; i < line_count && i < points_start + 20; i++) {
                    const char* line = lines[i];
                    
                    /* Check if we hit a Points directive - stop scanning for constants */
                    char line_lower[1024];
                    strncpy(line_lower, line, sizeof(line_lower) - 1);
                    line_lower[sizeof(line_lower) - 1] = '\0';
                    for (int k = 0; line_lower[k]; k++) {
                        line_lower[k] = (char)tolower((unsigned char)line_lower[k]);
                    }
                    if (strstr(line_lower, "pointsb") || strstr(line_lower, "pointsw") ||
                        strstr(line_lower, "pointsxb") || strstr(line_lower, "pointsxw")) {
                        break;
                    }
                    
                    /* Look for local constant definition: name = expression */
                    const char* eq = strchr(line, '=');
                    if (eq) {
                        /* Extract name (before =) */
                        char name[MAX_CONST_NAME];
                        int name_len = 0;
                        const char* p = line;
                        while (*p == ' ' || *p == '\t') p++;
                        while (p < eq && (isalnum((unsigned char)*p) || *p == '_')) {
                            if (name_len < MAX_CONST_NAME - 1) {
                                name[name_len++] = *p;
                            }
                            p++;
                        }
                        name[name_len] = '\0';
                        
                        /* Skip if name is empty or starts with a directive */
                        if (name_len > 0 && name[0] != '\0' && 
                            _stricmp(name, "equ") != 0 && _stricmp(name, "set") != 0) {
                            /* Parse value (after =) */
                            p = eq + 1;
                            while (*p == ' ' || *p == '\t') p++;
                            
                            int value;
                            if (parse_const_value(p, &value)) {
                                constants_add(name, value);
                                fprintf(stdout, "load_shape_from_asm: Added local constant %s = %d\n", name, value);
                            }
                        }
                    }
                }

                fprintf(stdout, "load_shape_from_asm: Starting point parsing from line %d\n", points_start);
                for (int i = points_start; i < line_count; i++) {
                    char line_lower[1024];
                    strncpy(line_lower, lines[i], sizeof(line_lower) - 1);
                    line_lower[sizeof(line_lower) - 1] = '\0';
                    for (int k = 0; line_lower[k]; k++) {
                        line_lower[k] = (char)tolower((unsigned char)line_lower[k]);
                    }

                    /* Check for EndPoints - stop parsing points */
                    if (strstr(line_lower, "endpoints")) {
                        fprintf(stdout, "load_shape_from_asm: Found EndPoints at line %d\n", i);
                        break;
                    }

                    /* Check for Pointsb (non-mirrored) - comes first */
                    if (strstr(line_lower, "pointsb") && !strstr(line_lower, "pointsxb")) {
                        in_mirrored_section = 0;
                        fprintf(stdout, "load_shape_from_asm: Found Pointsb at line %d\n", i);
                        continue;
                    }

                    /* Check for PointsXb (mirrored) - comes after Pointsb */
                    if (strstr(line_lower, "pointsxb")) {
                        in_mirrored_section = 1;
                        fprintf(stdout, "load_shape_from_asm: Found PointsXb at line %d\n", i);
                        continue;
                    }

                    /* Check for Pointsw (non-mirrored word) - comes first */
                    if (strstr(line_lower, "pointsw") && !strstr(line_lower, "pointsxw")) {
                        in_mirrored_section = 0;
                        fprintf(stdout, "load_shape_from_asm: Found Pointsw at line %d\n", i);
                        continue;
                    }

                    /* Check for PointsXw (mirrored word) - comes after Pointsw */
                    if (strstr(line_lower, "pointsxw")) {
                        in_mirrored_section = 1;
                        fprintf(stdout, "load_shape_from_asm: Found PointsXw at line %d\n", i);
                        continue;
                    }

                    /* Parse point: pb x,y,z, pw x,y,z, pbd2 x,y,z, pwd2 x,y,z
                       Regex pattern: r'p[wb]d?2?\s+(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)'
                       pbd2/pwd2 divide coordinates by 2
                    */
                    char* pbd2_pos = strstr(line_lower, "pbd2");
                    char* pwd2_pos = strstr(line_lower, "pwd2");
                    char* pb_pos = strstr(line_lower, "pb");
                    char* pw_pos = strstr(line_lower, "pw");
                    char* point_pos = NULL;
                    int is_pw = 0;
                    int divide_by_2 = 0;
                    int skip_len = 2; /* Default: pb/pw are 2 chars */
                    
                    /* Check for pbd2/pwd2 first (they also contain pb/pw) */
                    if (pbd2_pos && (pbd2_pos == line_lower || pbd2_pos[-1] == ' ' || pbd2_pos[-1] == '\t')) {
                        point_pos = pbd2_pos;
                        is_pw = 0;
                        divide_by_2 = 1;
                        skip_len = 4;
                    } else if (pwd2_pos && (pwd2_pos == line_lower || pwd2_pos[-1] == ' ' || pwd2_pos[-1] == '\t')) {
                        point_pos = pwd2_pos;
                        is_pw = 1;
                        divide_by_2 = 1;
                        skip_len = 4;
                    } else if (pb_pos && (!pw_pos || pb_pos < pw_pos)) {
                        /* Make sure it's pb, not pbd2 */
                        if (pb_pos[2] != 'd') {
                            point_pos = pb_pos;
                            is_pw = 0;
                        }
                    } else if (pw_pos) {
                        /* Make sure it's pw, not pwd2 */
                        if (pw_pos[2] != 'd') {
                            point_pos = pw_pos;
                            is_pw = 1;
                        }
                    }
                    
                    if (point_pos) {
                        /* Make sure point directive is at start of a word */
                        if (point_pos == line_lower ||
                            point_pos[-1] == ' ' || point_pos[-1] == '\t' || point_pos[-1] == '\n' || point_pos[-1] == '\r') {
                            /* Skip directive and any whitespace after it - use original line for parsing */
                            int offset = (int)(point_pos - line_lower);
                            char* orig_coord_start = (char*)lines[i] + offset + skip_len;
                            while (*orig_coord_start == ' ' || *orig_coord_start == '\t') orig_coord_start++;
                            
                            /* Split into three comma-separated parts */
                            char coord_buf[256];
                            strncpy(coord_buf, orig_coord_start, sizeof(coord_buf) - 1);
                            coord_buf[sizeof(coord_buf) - 1] = '\0';
                            
                            /* Remove trailing comment */
                            char* comment = strchr(coord_buf, ';');
                            if (comment) *comment = '\0';
                            
                            /* Split by comma */
                            char* x_str = coord_buf;
                            char* y_str = strchr(x_str, ',');
                            char* z_str = NULL;
                            
                            if (y_str) {
                                *y_str++ = '\0';
                                while (*y_str == ' ' || *y_str == '\t') y_str++;
                                z_str = strchr(y_str, ',');
                                if (z_str) {
                                    *z_str++ = '\0';
                                    while (*z_str == ' ' || *z_str == '\t') z_str++;
                                }
                            }
                            
                            if (x_str && y_str && z_str) {
                                /* Try to parse each coordinate using constant resolver */
                                int x, y, z;
                                int x_ok = parse_const_value(x_str, &x);
                                int y_ok = parse_const_value(y_str, &y);
                                int z_ok = parse_const_value(z_str, &z);
                                
                                if (x_ok && y_ok && z_ok) {
                                    /* Apply divide by 2 for pbd2/pwd2 */
                                    if (divide_by_2) {
                                        x /= 2;
                                        y /= 2;
                                        z /= 2;
                                    }
                                    /* Negate Y to convert from SNES coordinate system (Y down) to OpenGL (Y up) */
                                    y = -y;
                                    fprintf(stdout, "load_shape_from_asm: Parsed point: p%c%s %d,%d,%d (line %d)\n", 
                                            is_pw ? 'w' : 'b', divide_by_2 ? "d2" : "", x, y, z, i);
                                    if (in_mirrored_section) {
                                        /* Mirrored point - add both +x and -x versions */
                                        if (vertex_count < 8190) {
                                            vertices[vertex_count][0] = x;
                                            vertices[vertex_count][1] = y;
                                            vertices[vertex_count][2] = z;
                                            vertex_count++;
                                            vertices[vertex_count][0] = -x;
                                            vertices[vertex_count][1] = y;
                                            vertices[vertex_count][2] = z;
                                            vertex_count++;
                                        } else {
                                            parse_error = 1;
                                            fprintf(stderr, "load_shape_from_asm: Source vertex capacity exceeded at line %d\n", i);
                                        }
                                    }
                                    else {
                                        /* Non-mirrored point */
                                        if (vertex_count < 8191) {
                                            vertices[vertex_count][0] = x;
                                            vertices[vertex_count][1] = y;
                                            vertices[vertex_count][2] = z;
                                            vertex_count++;
                                        } else {
                                            parse_error = 1;
                                            fprintf(stderr, "load_shape_from_asm: Source vertex capacity exceeded at line %d\n", i);
                                        }
                                    }
                                } else {
                                    /* Failed to parse - log which coordinate failed */
                                    fprintf(stderr, "load_shape_from_asm: Could not resolve point (line %d): x=%s(%s) y=%s(%s) z=%s(%s)\n",
                                        i, x_str, x_ok ? "ok" : "FAIL", y_str, y_ok ? "ok" : "FAIL", z_str, z_ok ? "ok" : "FAIL");
                                    parse_error = 1;
                                }
                            } else {
                                fprintf(stderr, "load_shape_from_asm: Malformed point directive at line %d\n", i);
                                parse_error = 1;
                            }
                        }
                    }

                    /* Check for EndPoints */
                    if (strstr(line_lower, "endpoints")) {
                        break;
                    }
                }

                /* Note: We don't add vertices to CAD system here anymore.
                   Points are created per-polygon by create_polygon_with_points_safe() */
                fprintf(stdout, "Loaded %d vertices for shape: %s\n", vertex_count, shape_name);

                /* Parse faces - scan from faces_start until EndShape
                   Also need to check for shape_f1, shape_f2, etc. sections */
                int face_count = 0;
                
                /* Find all face sections (shape_f, shape_f1, shape_f2, etc.) */
                int face_sections[32];
                int face_section_count = 0;
                
                /* Only process faces if we found a valid faces section */
                if (faces_start < 0) {
                    fprintf(stderr, "WARNING: No faces section found for shape '%s', loading points only\n", shape_name);
                    faces_start = line_count; /* Prevent any face parsing loops */
                }
                
                /* Find where this shape ends (EndShape) to limit our search */
                int shape_end = line_count;
                for (int i = faces_start; i < line_count; i++) {
                    char line_lower[1024];
                    strncpy(line_lower, lines[i], sizeof(line_lower) - 1);
                    line_lower[sizeof(line_lower) - 1] = '\0';
                    for (int k = 0; line_lower[k]; k++) {
                        line_lower[k] = (char)tolower((unsigned char)line_lower[k]);
                    }
                    if (strstr(line_lower, "endshape")) {
                        shape_end = i + 1;
                        break;
                    }
                }
                
                /* Look for ALL face sections (shape_f, shape_f1, shape_f2, etc.) WITHIN this shape only */
                /* Use the actual face section name from ShapeHdr (shape_f already set above) */
                char shape_f_base[256];
                strncpy(shape_f_base, shape_f, sizeof(shape_f_base) - 1);
                shape_f_base[sizeof(shape_f_base) - 1] = '\0';
                size_t base_len = strlen(shape_f_base);
                
                for (int i = faces_start; i < shape_end && face_section_count < 32; i++) {
                    char line_lower[1024];
                    strncpy(line_lower, lines[i], sizeof(line_lower) - 1);
                    line_lower[sizeof(line_lower) - 1] = '\0';
                    for (int k = 0; line_lower[k]; k++) {
                        line_lower[k] = (char)tolower((unsigned char)line_lower[k]);
                    }
                    char* stripped = line_lower;
                    while (*stripped && (*stripped == ' ' || *stripped == '\t')) stripped++;
                    char* end = stripped + strlen(stripped) - 1;
                    while (end > stripped && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
                        *end = '\0';
                        end--;
                    }
                    
                    /* Check if this line starts with shape_f (could be shape_f, shape_f1, shape_f2, etc.) */
                    if (strncmp(stripped, shape_f_base, base_len) == 0) {
                        /* Check what comes after _f */
                        char after_f = stripped[base_len];
                        /* Accept if: end of string, whitespace, or a digit (for f1, f2, etc.) */
                        if (!after_f || after_f == '\0' || after_f == ' ' || after_f == '\t' || 
                            isdigit((unsigned char)after_f)) {
                            /* Make sure it's not already in our list */
                            int already_found = 0;
                            for (int j = 0; j < face_section_count; j++) {
                                if (face_sections[j] == i) {
                                    already_found = 1;
                                    break;
                                }
                            }
                            if (!already_found) {
                                face_sections[face_section_count++] = i;
                                fprintf(stdout, "Found face section for %s at line %d: %s\n", shape_name, i, stripped);
                            }
                        }
                    }
                }
                
                fprintf(stdout, "load_shape_from_asm: Found %d face section(s) for %s\n", face_section_count, shape_name);
                
                /* Parse faces from all face sections */
                for (int section_idx = 0; section_idx < face_section_count; section_idx++) {
                    int section_start = face_sections[section_idx];
                    fprintf(stdout, "load_shape_from_asm: Parsing face section %d starting at line %d\n", section_idx, section_start);
                    
                    /* Determine where this section ends - either at Fend/EndShape, or at the start of the next face section */
                    int section_end = line_count;
                    if (section_idx + 1 < face_section_count) {
                        section_end = face_sections[section_idx + 1];
                    }
                    
                    /* Find the actual end of this section (Fend or EndShape) */
                    for (int i = section_start; i < section_end && i < line_count; i++) {
                        char line_lower[1024];
                        strncpy(line_lower, lines[i], sizeof(line_lower) - 1);
                        line_lower[sizeof(line_lower) - 1] = '\0';
                        for (int k = 0; line_lower[k]; k++) {
                            line_lower[k] = (char)tolower((unsigned char)line_lower[k]);
                        }
                        if (strstr(line_lower, "endshape") || strstr(line_lower, "fend")) {
                            section_end = i + 1; /* Stop after this line */
                            break;
                        }
                    }
                    
                    /* Skip the label line and look for "Faces" keyword or actual face definitions */
                    int actual_start = section_start;
                    for (int i = section_start; i < section_end && i < line_count; i++) {
                        char line_lower[1024];
                        strncpy(line_lower, lines[i], sizeof(line_lower) - 1);
                        line_lower[sizeof(line_lower) - 1] = '\0';
                        for (int k = 0; line_lower[k]; k++) {
                            line_lower[k] = (char)tolower((unsigned char)line_lower[k]);
                        }
                        /* If we find "faces" keyword or a face definition, start parsing from here */
                        if (strstr(line_lower, "faces") || strstr(line_lower, "face3") || 
                            strstr(line_lower, "face4") || strstr(line_lower, "face5")) {
                            actual_start = i;
                            break;
                        }
                    }
                    
                    fprintf(stdout, "load_shape_from_asm: Face section %d: parsing from line %d to %d\n", section_idx, actual_start, section_end);
                    
                    for (int i = actual_start; i < section_end && i < line_count; i++) {
                        char line_lower[1024];
                        strncpy(line_lower, lines[i], sizeof(line_lower) - 1);
                        line_lower[sizeof(line_lower) - 1] = '\0';
                        for (int k = 0; line_lower[k]; k++) {
                            line_lower[k] = (char)tolower((unsigned char)line_lower[k]);
                        }
                        
                        /* Check for EndShape or Fend - stop parsing this section */
                        if (strstr(line_lower, "endshape") || strstr(line_lower, "fend")) {
                            fprintf(stdout, "load_shape_from_asm: Found end of face section at line %d\n", i);
                            break;
                        }

                        /* Parse Face2: line (2 vertices)
                           Format: Face2 color, viz, nx, ny, nz, v0, v1 */
                        char* face2_pos = strstr(line_lower, "face2");
                        if (face2_pos) {
                            char* num_start = face2_pos + 5;
                            while (*num_start == ' ' || *num_start == '\t') num_start++;
                            
                            char* parse_pos = num_start;
                            int color = (int)strtol(parse_pos, &parse_pos, 10);
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            if (*parse_pos == ',') parse_pos++;
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            
                            (void)strtol(parse_pos, &parse_pos, 10); /* visibility */
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            if (*parse_pos == ',') parse_pos++;
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            
                            (void)strtol(parse_pos, &parse_pos, 10); /* normal X */
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            if (*parse_pos == ',') parse_pos++;
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            
                            (void)strtol(parse_pos, &parse_pos, 10); /* normal Y */
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            if (*parse_pos == ',') parse_pos++;
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            
                            (void)strtol(parse_pos, &parse_pos, 10); /* normal Z */
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            if (*parse_pos == ',') parse_pos++;
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            
                            int v0 = (int)strtol(parse_pos, &parse_pos, 10);
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            if (*parse_pos == ',') parse_pos++;
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            
                            int v1 = (int)strtol(parse_pos, &parse_pos, 10);
                            
                            if (parse_pos > num_start) {
                                if (v0 >= 0 && v0 < vertex_count &&
                                    v1 >= 0 && v1 < vertex_count) {
                                    /* Create Face2 (line) with its own point chain */
                                    int line_verts[2] = { v0, v1 };
                                    if (create_polygon_with_points_safe(core, vertices, line_verts, 2,
                                            (uint8_t)color, vertex_count, &parse_error) != INVALID_INDEX) {
                                        face_count++;
                                    }
                                }
                            }
                        }

                        /* Parse Face3: triangle
                           Regex: r'face3\s+(\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)'
                           Format: Face3<whitespace>color<optional ws>,<optional ws>viz<optional ws>,<optional ws>nx<optional ws>,<optional ws>ny<optional ws>,<optional ws>nz<optional ws>,<optional ws>v0<optional ws>,<optional ws>v1<optional ws>,<optional ws>v2 */
                        char* face3_pos = strstr(line_lower, "face3");
                        if (face3_pos) {
                            /* Skip 'face3' and any whitespace after it */
                            char* num_start = face3_pos + 5;
                            while (*num_start == ' ' || *num_start == '\t') num_start++;
                            
                            /* Parse using strtol to handle flexible whitespace */
                            char* parse_pos = num_start;
                            int color = (int)strtol(parse_pos, &parse_pos, 10);
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            if (*parse_pos == ',') parse_pos++;
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            
                            (void)strtol(parse_pos, &parse_pos, 10); /* visibility */
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            if (*parse_pos == ',') parse_pos++;
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            
                            (void)strtol(parse_pos, &parse_pos, 10); /* normal X */
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            if (*parse_pos == ',') parse_pos++;
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            
                            (void)strtol(parse_pos, &parse_pos, 10); /* normal Y */
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            if (*parse_pos == ',') parse_pos++;
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            
                            (void)strtol(parse_pos, &parse_pos, 10); /* normal Z */
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            if (*parse_pos == ',') parse_pos++;
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            
                            int v0 = (int)strtol(parse_pos, &parse_pos, 10);
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            if (*parse_pos == ',') parse_pos++;
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            
                            int v1 = (int)strtol(parse_pos, &parse_pos, 10);
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            if (*parse_pos == ',') parse_pos++;
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            
                            int v2 = (int)strtol(parse_pos, &parse_pos, 10);
                            
                            /* Check if we successfully parsed all 8 values */
                            if (parse_pos > num_start) {
                                if (v0 >= 0 && v0 < vertex_count &&
                                    v1 >= 0 && v1 < vertex_count &&
                                    v2 >= 0 && v2 < vertex_count) {
                                    /* Create Face3 (triangle) with its own point chain */
                                    int tri_verts[3] = { v0, v1, v2 };
                                    if (create_polygon_with_points_safe(core, vertices, tri_verts, 3,
                                            (uint8_t)color, vertex_count, &parse_error) != INVALID_INDEX) {
                                        face_count++;
                                    }
                                }
                            }
                        }

                        /* Parse Face4: quad
                           Regex: r'face4\s+(\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)' */
                        char* face4_pos = strstr(line_lower, "face4");
                        if (face4_pos) {
                            char* num_start = face4_pos + 5;
                            while (*num_start == ' ' || *num_start == '\t') num_start++;
                            
                            char* parse_pos = num_start;
                            int color = (int)strtol(parse_pos, &parse_pos, 10);
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            if (*parse_pos == ',') parse_pos++;
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            
                            (void)strtol(parse_pos, &parse_pos, 10); /* visibility */
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            if (*parse_pos == ',') parse_pos++;
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            
                            (void)strtol(parse_pos, &parse_pos, 10); /* normal X */
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            if (*parse_pos == ',') parse_pos++;
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            
                            (void)strtol(parse_pos, &parse_pos, 10); /* normal Y */
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            if (*parse_pos == ',') parse_pos++;
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            
                            (void)strtol(parse_pos, &parse_pos, 10); /* normal Z */
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            if (*parse_pos == ',') parse_pos++;
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            
                            int v0 = (int)strtol(parse_pos, &parse_pos, 10);
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            if (*parse_pos == ',') parse_pos++;
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            
                            int v1 = (int)strtol(parse_pos, &parse_pos, 10);
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            if (*parse_pos == ',') parse_pos++;
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            
                            int v2 = (int)strtol(parse_pos, &parse_pos, 10);
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            if (*parse_pos == ',') parse_pos++;
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            
                            int v3 = (int)strtol(parse_pos, &parse_pos, 10);
                            
                            if (parse_pos > num_start) {
                                if (v0 >= 0 && v0 < vertex_count &&
                                    v1 >= 0 && v1 < vertex_count &&
                                    v2 >= 0 && v2 < vertex_count &&
                                    v3 >= 0 && v3 < vertex_count) {
                                    /* Split quad into 2 triangles: v0,v1,v2 and v0,v2,v3 */
                                    /* Each triangle gets its own point chain */
                                    int tri1_verts[3] = { v0, v1, v2 };
                                    if (create_polygon_with_points_safe(core, vertices, tri1_verts, 3,
                                            (uint8_t)color, vertex_count, &parse_error) != INVALID_INDEX) {
                                        face_count++;
                                    }
                                    
                                    int tri2_verts[3] = { v0, v2, v3 };
                                    if (create_polygon_with_points_safe(core, vertices, tri2_verts, 3,
                                            (uint8_t)color, vertex_count, &parse_error) != INVALID_INDEX) {
                                        face_count++;
                                    }
                                }
                            }
                        }

                        /* Parse Face5: pentagon
                           Regex: r'face5\s+(\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)' */
                        char* face5_pos = strstr(line_lower, "face5");
                        if (face5_pos) {
                            char* num_start = face5_pos + 5;
                            while (*num_start == ' ' || *num_start == '\t') num_start++;
                            
                            char* parse_pos = num_start;
                            int color = (int)strtol(parse_pos, &parse_pos, 10);
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            if (*parse_pos == ',') parse_pos++;
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            
                            (void)strtol(parse_pos, &parse_pos, 10); /* visibility */
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            if (*parse_pos == ',') parse_pos++;
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            
                            (void)strtol(parse_pos, &parse_pos, 10); /* normal X */
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            if (*parse_pos == ',') parse_pos++;
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            
                            (void)strtol(parse_pos, &parse_pos, 10); /* normal Y */
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            if (*parse_pos == ',') parse_pos++;
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            
                            (void)strtol(parse_pos, &parse_pos, 10); /* normal Z */
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            if (*parse_pos == ',') parse_pos++;
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            
                            int v0 = (int)strtol(parse_pos, &parse_pos, 10);
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            if (*parse_pos == ',') parse_pos++;
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            
                            int v1 = (int)strtol(parse_pos, &parse_pos, 10);
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            if (*parse_pos == ',') parse_pos++;
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            
                            int v2 = (int)strtol(parse_pos, &parse_pos, 10);
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            if (*parse_pos == ',') parse_pos++;
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            
                            int v3 = (int)strtol(parse_pos, &parse_pos, 10);
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            if (*parse_pos == ',') parse_pos++;
                            while (*parse_pos == ' ' || *parse_pos == '\t') parse_pos++;
                            
                            int v4 = (int)strtol(parse_pos, &parse_pos, 10);
                            
                            if (parse_pos > num_start) {
                                if (v0 >= 0 && v0 < vertex_count &&
                                    v1 >= 0 && v1 < vertex_count &&
                                    v2 >= 0 && v2 < vertex_count &&
                                    v3 >= 0 && v3 < vertex_count &&
                                    v4 >= 0 && v4 < vertex_count) {
                                    /* Split pentagon into 3 triangles: v0,v1,v2, v0,v2,v3, and v0,v3,v4 */
                                    /* Each triangle gets its own point chain */
                                    int tri1_verts[3] = { v0, v1, v2 };
                                    if (create_polygon_with_points_safe(core, vertices, tri1_verts, 3,
                                            (uint8_t)color, vertex_count, &parse_error) != INVALID_INDEX) {
                                        face_count++;
                                    }
                                    
                                    int tri2_verts[3] = { v0, v2, v3 };
                                    if (create_polygon_with_points_safe(core, vertices, tri2_verts, 3,
                                            (uint8_t)color, vertex_count, &parse_error) != INVALID_INDEX) {
                                        face_count++;
                                    }
                                    
                                    int tri3_verts[3] = { v0, v3, v4 };
                                    if (create_polygon_with_points_safe(core, vertices, tri3_verts, 3,
                                            (uint8_t)color, vertex_count, &parse_error) != INVALID_INDEX) {
                                        face_count++;
                                    }
                                }
                            }
                        }

                        /* Check for EndShape */
                        if (strstr(line_lower, "endshape")) {
                            found = 1;
                            break;
                        }
                    }
                }

                fprintf(stdout, "Loaded %d faces for shape: %s\n", face_count, shape_name);

                /* Capacity or malformed-index failures invalidate the whole
                   temporary parse.  Never expose a truncated preview that
                   could later replace the user's document. */
                if (parse_error || vertex_count <= 0 || face_count <= 0) {
                    fprintf(stderr,
                            "load_shape_from_asm: Rejected partial shape %s "
                            "(vertices=%d faces=%d capacity/error=%d)\n",
                            shape_name, vertex_count, face_count, parse_error);
                    CadCore_Clear(core);
                    found = 0;
                } else {
                    found = 1;
                    fprintf(stdout, "Successfully parsed shape: %s (vertices: %d, polygons: %d)\n",
                        shape_name, vertex_count, core->data.polygonCount);
                }

                free(vertices);
                free(lines);
                free(content);
                if (found) break;
            }
        }
    } while (FindNextFileW(hFind, &find_data));

    FindClose(hFind);
#else
    /* Non-Windows implementation would go here */
    (void)folder_path;
    (void)shape_name;
#endif

    return found ? 1 : 0;
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
    CadCore_Clear(g->shape_preview);
    g->shape_preview_valid = load_shape_from_asm(
        g->shape_preview, g->shape_names[index], g->shape_folder_path);
    if (!g->shape_preview_valid) {
        gui_set_status(g, "Could not preview shape %s; document unchanged",
                       g->shape_names[index]);
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
    if (EditorTool_IsActive(&g->edit_tool) || g->document.transactionBefore)
        reset_interaction(g);
    if (!confirm_replace_document(g, "quitting")) return 0;
    g->pending_command = GUI_COMMAND_QUIT;
    return 1;
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
        if (g->document.transactionBefore) history_cancel(g);
        reset_interaction(g);
        g->menu_open = -1;
        g->submenu_open = 0;
        if (g->selected_tool == CAD_TOOL_FACE_CREATE) CadCore_ClearSelection(g->cad);
        if (had_operation) gui_set_status(g, "Operation cancelled");
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
        case 'a': execute_editor_command(g, CAD_COMMAND_OPTION_SELECT_ALL); return;
        case 'c': execute_editor_command(g, CAD_COMMAND_EDIT_COPY); return;
        case 'v': execute_editor_command(g, CAD_COMMAND_EDIT_PASTE); return;
        case 'q': gui_request_quit(g); return;
        default: break;
        }
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
    if (!g || win_w <= 0 || win_h <= 0) return;
    const int margin = 4;
    const int menu_bottom = MenuBarHeight() + margin;
    const int status_h = 22;
    const int palette_w = g->tool_palette_visible ? 86 : 0;
    const int content_x = margin + (palette_w ? palette_w + margin : 0);
    const int coord_h = g->coordinates_visible ? 54 : 0;
    int view_bottom = win_h - status_h - margin - (coord_h ? coord_h + margin : 0);
    if (view_bottom < menu_bottom + 100) view_bottom = menu_bottom + 100;

    if (g->tool_palette_visible) {
        g->toolPalette.r = (Rect){ margin, menu_bottom, palette_w,
                                  win_h - menu_bottom - status_h - margin };
    }
    if (g->coordinates_visible) {
        g->coordBox.r = (Rect){ content_x, view_bottom + margin,
                               win_w - content_x - margin, coord_h };
    }

    int visible_count = 0;
    for (int i = 0; i < 4; ++i) visible_count += g->view_visible[i] != 0;
    if (visible_count > 0) {
        int cols = visible_count == 1 ? 1 : 2;
        int rows = (visible_count + cols - 1) / cols;
        int available_w = win_w - content_x - margin;
        int available_h = view_bottom - menu_bottom;
        int cell_w = (available_w - margin * (cols - 1)) / cols;
        int cell_h = (available_h - margin * (rows - 1)) / rows;
        if (cell_w < 100) cell_w = 100;
        if (cell_h < 80) cell_h = 80;
        int slot = 0;
        for (int i = 0; i < 4; ++i) {
            if (!g->view_visible[i]) continue;
            int col = slot % cols;
            int row = slot / cols;
            g->view[i].r = (Rect){ content_x + col * (cell_w + margin),
                                  menu_bottom + row * (cell_h + margin),
                                  cell_w, cell_h };
            ++slot;
        }
    }
    g->layout_width = win_w;
    g->layout_height = win_h;
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

static int collect_transform_animation_points(GuiState* g,
                                              const int16_t* base_points,
                                              int base_count,
                                              int16_t* result,
                                              int capacity) {
    int result_count = 0;
    if (!g || !base_points || !result || base_count <= 0) return 0;
    for (int polygon_index = 0; polygon_index < g->cad->data.polygonCount; ++polygon_index) {
        CadPolygon* polygon = &g->cad->data.polygons[polygon_index];
        if (!polygon->flags || polygon->animation < 0 ||
            polygon->animation >= CAD_MAX_ANIMATION_INDICES ||
            !g->cad->data.animationIndices[polygon->animation].flags) continue;
        int16_t base_chain[CAD_MAX_FACE_POINTS];
        int count = polygon_point_indices(g->cad, (int16_t)polygon_index,
                                          base_chain, ARRAY_COUNT(base_chain));
        for (int ordinal = 0; ordinal < count; ++ordinal) {
            int selected = 0;
            for (int b = 0; b < base_count; ++b) selected |= base_chain[ordinal] == base_points[b];
            if (!selected) continue;
            CadAnimationIndex* animation = &g->cad->data.animationIndices[polygon->animation];
            for (int frame = 0; frame < CAD_ANIMATION_FRAMES; ++frame) {
                int16_t current = animation->frame[frame];
                for (int step = 0; step < ordinal && current >= 0 &&
                     current < CAD_MAX_ANIMATION_POINTS; ++step) {
                    current = g->cad->data.animationPoints[current].nextPoint;
                }
                if (current < 0 || current >= CAD_MAX_ANIMATION_POINTS ||
                    !g->cad->data.animationPoints[current].flags) continue;
                int duplicate = 0;
                for (int i = 0; i < result_count; ++i) duplicate |= result[i] == current;
                if (!duplicate && result_count < capacity) result[result_count++] = current;
            }
        }
    }
    return result_count;
}

static int ensure_static_topology(GuiState* g, const char* action) {
    if (!g || !CadDocument_HasAnimation(&g->document)) return 1;
#ifdef _WIN32
    char message[512];
    snprintf(message, sizeof(message),
             "%s changes model topology and cannot preserve animation links.\n\n"
             "Create an unnamed static copy and discard animation data?",
             action ? action : "This operation");
    if (MessageBoxA(GetActiveWindow(), message, "3DCad - Animated document",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        gui_set_status(g, "%s cancelled; animation data remains intact", action ? action : "Operation");
        return 0;
    }
    CadCore* static_core = (CadCore*)malloc(sizeof(*static_core));
    uint8_t* palette_copy = (uint8_t*)malloc(CAD_COLOR_DATA_SIZE + CAD_PALETTE_DATA_SIZE);
    if (!static_core || !palette_copy) {
        free(static_core);
        free(palette_copy);
        gui_set_status(g, "Not enough memory to create a static copy");
        return 0;
    }
    size_t color_size = g->document.colorDataSize;
    size_t palette_size = g->document.paletteDataSize;
    memcpy(palette_copy, g->document.colorData, CAD_COLOR_DATA_SIZE);
    memcpy(palette_copy + CAD_COLOR_DATA_SIZE, g->document.paletteData,
           CAD_PALETTE_DATA_SIZE);
    *static_core = *g->cad;
    memset(static_core->data.animationIndices, 0, sizeof(static_core->data.animationIndices));
    memset(static_core->data.animationPoints, 0, sizeof(static_core->data.animationPoints));
    static_core->data.animationIndexCount = 0;
    static_core->data.animationPointCount = 0;
    for (int i = 0; i < static_core->data.polygonCount; ++i) {
        if (static_core->data.polygons[i].flags) static_core->data.polygons[i].animation = INVALID_INDEX;
    }
    replace_document(g, static_core, NULL, 1);
    memcpy(g->document.colorData, palette_copy, CAD_COLOR_DATA_SIZE);
    memcpy(g->document.paletteData, palette_copy + CAD_COLOR_DATA_SIZE,
           CAD_PALETTE_DATA_SIZE);
    g->document.colorDataSize = color_size;
    g->document.paletteDataSize = palette_size;
    apply_document_palette(g);
    free(palette_copy);
    free(static_core);
    gui_set_status(g, "Created unnamed static copy; animation data removed");
    return 1;
#else
    gui_set_status(g, "%s blocked: create a static copy before changing topology", action ? action : "Operation");
    return 0;
#endif
}

static void selection_center(CadCore* core, const int16_t* points, int count,
                             double* x, double* y, double* z) {
    *x = *y = *z = 0.0;
    if (!core || !points || count <= 0) return;
    for (int i = 0; i < count; ++i) {
        *x += core->data.points[points[i]].pointx;
        *y += core->data.points[points[i]].pointy;
        *z += core->data.points[points[i]].pointz;
    }
    *x /= count; *y /= count; *z /= count;
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
    selection_center(g->cad, points, count, &x, &y, &z);
    state_set_value(g, 0, 0.0); state_set_value(g, 1, 0.0); state_set_value(g, 2, 0.0);
    state_set_value(g, 3, x); state_set_value(g, 4, y); state_set_value(g, 5, z);
    state_set_value(g, 6, 0.0); state_set_value(g, 7, 0.0); state_set_value(g, 8, 0.0);
    state_set_value(g, 9, x); state_set_value(g, 10, y); state_set_value(g, 11, z);
    state_set_value(g, 12, 1.0); state_set_value(g, 13, 1.0); state_set_value(g, 14, 1.0);
}

static void state_panel_open(GuiState* g) {
    if (!g) return;
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
    int16_t animation_points[CAD_MAX_ANIMATION_POINTS];
    if (!g || !state_parse_values(g, values)) return 0;
    int point_count = collect_transform_points(g, g->state_face_target,
                                               points, ARRAY_COUNT(points));
    if (!point_count) {
        gui_set_status(g, "The STATE target selection is no longer available");
        return 0;
    }
    int animation_count = collect_transform_animation_points(g, points, point_count,
        animation_points, ARRAY_COUNT(animation_points));
    if (!history_push(g)) return 0;
    for (int i = 0; i < point_count; ++i) {
        state_transform_point(&g->cad->data.points[points[i]], values);
    }
    for (int i = 0; i < animation_count; ++i) {
        state_transform_point(&g->cad->data.animationPoints[animation_points[i]], values);
    }
    if (!history_commit(g)) return 0;
    gui_set_status(g, "Applied numeric transform to %d point(s)%s%s", point_count,
                   animation_count ? " and " : "",
                   animation_count ? "corresponding animation frames" : "");
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
    if (!history_push(g)) return;
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
    if (!history_push(g)) return;
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
    int count = polygon_point_indices(core, polygon_index, chain, ARRAY_COUNT(chain));
    if (count <= 3) return 1;
    const CadPoint* a = &core->data.points[chain[0]];
    const CadPoint* b = &core->data.points[chain[1]];
    const CadPoint* c = &core->data.points[chain[2]];
    double ux = b->pointx - a->pointx, uy = b->pointy - a->pointy, uz = b->pointz - a->pointz;
    double vx = c->pointx - a->pointx, vy = c->pointy - a->pointy, vz = c->pointz - a->pointz;
    double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    double length = sqrt(nx * nx + ny * ny + nz * nz);
    if (length < 1e-9) return 0;
    for (int i = 3; i < count; ++i) {
        const CadPoint* p = &core->data.points[chain[i]];
        double distance = fabs(nx * (p->pointx - a->pointx) +
                               ny * (p->pointy - a->pointy) +
                               nz * (p->pointz - a->pointz)) / length;
        if (distance > 0.01) return 0;
    }
    return 1;
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
    if (!history_push(g)) return;
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
    int pairs = 0;
    for (int i = 0; i < g->cad->data.pointCount; ++i) {
        CadPoint* a = &g->cad->data.points[i];
        if (!a->flags) continue;
        for (int j = i + 1; j < g->cad->data.pointCount; ++j) {
            CadPoint* b = &g->cad->data.points[j];
            if (!b->flags) continue;
            double dx = a->pointx - b->pointx, dy = a->pointy - b->pointy, dz = a->pointz - b->pointz;
            if (dx * dx + dy * dy + dz * dz <= 0.0001 &&
                (dx != 0.0 || dy != 0.0 || dz != 0.0)) ++pairs;
        }
    }
    if (!pairs) { gui_set_status(g, "Point Merge: no near-duplicate coordinates"); return; }
    if (!history_push(g)) return;
    int merged = 0;
    for (int i = 0; i < g->cad->data.pointCount; ++i) {
        CadPoint* a = &g->cad->data.points[i];
        if (!a->flags) continue;
        for (int j = i + 1; j < g->cad->data.pointCount; ++j) {
            CadPoint* b = &g->cad->data.points[j];
            if (!b->flags) continue;
            double dx = a->pointx - b->pointx, dy = a->pointy - b->pointy, dz = a->pointz - b->pointz;
            if (dx * dx + dy * dy + dz * dz <= 0.0001) {
                b->pointx = a->pointx; b->pointy = a->pointy; b->pointz = a->pointz;
                ++merged;
            }
        }
    }
    g->cad->isDirty = 1;
    if (!history_commit(g)) return;
    gui_set_status(g, "Point Merge: aligned %d near-duplicate point(s)", merged);
}

static int polygons_equal(const CadCore* core, int16_t left, int16_t right) {
    int16_t a[CAD_MAX_FACE_POINTS], b[CAD_MAX_FACE_POINTS];
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
    if (!history_push(g)) return;
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
    if (!history_push(g)) return;
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

static int screen_point_in_polygon(int x, int y, const int* px, const int* py, int count) {
    int inside = 0;
    for (int i = 0, j = count - 1; i < count; j = i++) {
        if (((py[i] > y) != (py[j] > y)) &&
            (x < (px[j] - px[i]) * (double)(y - py[i]) /
                 (double)(py[j] - py[i] ? py[j] - py[i] : 1) + px[i])) inside = !inside;
    }
    return inside;
}

static int16_t find_polygon_at(GuiState* g, int view_index, int screen_x, int screen_y,
                               int* nearest_edge) {
    if (nearest_edge) *nearest_edge = -1;
    Rect content = view_content_rect(g, view_index);
    int16_t chain[CAD_MAX_FACE_POINTS];
    for (int p = g->cad->data.polygonCount - 1; p >= 0; --p) {
        if (!CadCore_IsPolygonValid(g->cad, (int16_t)p)) continue;
        int count = polygon_point_indices(g->cad, (int16_t)p, chain, ARRAY_COUNT(chain));
        if (count < 2) continue;
        int px[CAD_MAX_FACE_POINTS], py[CAD_MAX_FACE_POINTS];
        for (int i = 0; i < count; ++i) {
            CadPoint* point = &g->cad->data.points[chain[i]];
            CadView_ProjectPoint(&g->views[view_index], point->pointx, point->pointy, point->pointz,
                                 &px[i], &py[i], content.w, content.h);
            px[i] += content.x; py[i] += content.y;
        }
        double best_edge = 1e30;
        int best_index = -1;
        int edges = count == 2 ? 1 : count;
        for (int i = 0; i < edges; ++i) {
            int next = (i + 1) % count;
            double distance = point_segment_distance(screen_x, screen_y,
                                                     px[i], py[i], px[next], py[next]);
            if (distance < best_edge) { best_edge = distance; best_index = i; }
        }
        if ((count >= 3 && screen_point_in_polygon(screen_x, screen_y, px, py, count)) || best_edge <= 8.0) {
            if (nearest_edge) *nearest_edge = best_index;
            return (int16_t)p;
        }
    }
    return INVALID_INDEX;
}

static int coordinates_are_flat(const double coords[][3], int count) {
    if (count <= 3) return 1;
    double ux = coords[1][0] - coords[0][0], uy = coords[1][1] - coords[0][1], uz = coords[1][2] - coords[0][2];
    double vx = coords[2][0] - coords[0][0], vy = coords[2][1] - coords[0][1], vz = coords[2][2] - coords[0][2];
    double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    double length = sqrt(nx * nx + ny * ny + nz * nz);
    if (length < 1e-9) return 0;
    for (int i = 3; i < count; ++i) {
        double distance = fabs(nx * (coords[i][0] - coords[0][0]) +
                               ny * (coords[i][1] - coords[0][1]) +
                               nz * (coords[i][2] - coords[0][2])) / length;
        if (distance > 0.01) return 0;
    }
    return 1;
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
    if (!coordinates_are_flat(coords, count)) {
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
    int count = collect_transform_points(g, 0, points, ARRAY_COUNT(points));
    if (!count) { gui_set_status(g, "Select points to flip"); return; }
    double cx, cy, cz;
    selection_center(g->cad, points, count, &cx, &cy, &cz);
    (void)cy; (void)cz;
    int16_t animation_points[CAD_MAX_ANIMATION_POINTS];
    int animation_count = collect_transform_animation_points(g, points, count,
        animation_points, ARRAY_COUNT(animation_points));
    if (!history_push(g)) return;
    for (int i = 0; i < count; ++i) g->cad->data.points[points[i]].pointx = 2.0 * cx - g->cad->data.points[points[i]].pointx;
    for (int i = 0; i < animation_count; ++i) {
        CadAnimationPoint* p = &g->cad->data.animationPoints[animation_points[i]];
        p->pointx = 2.0 * cx - p->pointx;
    }
    g->cad->isDirty = 1;
    if (!history_commit(g)) return;
    gui_set_status(g, "Flipped %d point(s) across X=%.2f", count, cx);
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
    selection_center(g->cad, points, point_count, &mirror_center, &cy, &cz);
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
    reset_interaction(g);
    if (descriptor->flags & CAD_TOOL_FLAG_DISABLED) {
        g->selected_tool = CAD_TOOL_NONE;
        gui_set_status(g, "%s unavailable: %s", descriptor->name, descriptor->help);
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
    if (!count || (dx == 0 && dy == 0)) return 1;
    int16_t animation_points[CAD_MAX_ANIMATION_POINTS];
    int animation_count = collect_transform_animation_points(g, points, count,
        animation_points, ARRAY_COUNT(animation_points));
    if (!g->transform_history_pushed) {
        if (!history_push(g)) return 0;
        g->transform_history_pushed = 1;
    }
    CadView* view = &g->views[g->point_move_view];
    if (g->selected_tool == CAD_TOOL_POINT_MOVE || g->selected_tool == CAD_TOOL_FACE_MOVE) {
        double tx, ty, tz;
        Rect content = view_content_rect(g, g->point_move_view);
        CadView_UnprojectDelta(view, dx, dy, content.w, content.h, &tx, &ty, &tz);
        for (int i = 0; i < count; ++i) {
            CadPoint* p = &g->cad->data.points[points[i]];
            p->pointx += tx; p->pointy += ty; p->pointz += tz;
        }
        for (int i = 0; i < animation_count; ++i) {
            CadAnimationPoint* p = &g->cad->data.animationPoints[animation_points[i]];
            p->pointx += tx; p->pointy += ty; p->pointz += tz;
        }
    } else {
        double cx, cy, cz;
        selection_center(g->cad, points, count, &cx, &cy, &cz);
        if (g->selected_tool == CAD_TOOL_POINT_SCALE || g->selected_tool == CAD_TOOL_FACE_SCALE) {
            double factor = exp((double)(dx - dy) * 0.01);
            if (factor < 0.25) factor = 0.25;
            if (factor > 4.0) factor = 4.0;
            for (int i = 0; i < count; ++i) {
                CadPoint* p = &g->cad->data.points[points[i]];
                p->pointx = cx + (p->pointx - cx) * factor;
                p->pointy = cy + (p->pointy - cy) * factor;
                p->pointz = cz + (p->pointz - cz) * factor;
            }
            for (int i = 0; i < animation_count; ++i) {
                CadAnimationPoint* p = &g->cad->data.animationPoints[animation_points[i]];
                p->pointx = cx + (p->pointx - cx) * factor;
                p->pointy = cy + (p->pointy - cy) * factor;
                p->pointz = cz + (p->pointz - cz) * factor;
            }
        } else {
            double angle = (double)(dx - dy) * 0.01;
            double s = sin(angle), c = cos(angle);
            for (int i = 0; i < count; ++i) {
                CadPoint* p = &g->cad->data.points[points[i]];
                double x = p->pointx - cx, y = p->pointy - cy, z = p->pointz - cz;
                if (view->type == CAD_VIEW_TOP || view->type == CAD_VIEW_3D) {
                    p->pointx = cx + x * c - z * s; p->pointz = cz + x * s + z * c;
                } else if (view->type == CAD_VIEW_FRONT) {
                    p->pointx = cx + x * c - y * s; p->pointy = cy + x * s + y * c;
                } else {
                    p->pointy = cy + y * c - z * s; p->pointz = cz + y * s + z * c;
                }
            }
            for (int i = 0; i < animation_count; ++i) {
                CadAnimationPoint* p = &g->cad->data.animationPoints[animation_points[i]];
                double x = p->pointx - cx, y = p->pointy - cy, z = p->pointz - cz;
                if (view->type == CAD_VIEW_TOP || view->type == CAD_VIEW_3D) {
                    p->pointx = cx + x * c - z * s; p->pointz = cz + x * s + z * c;
                } else if (view->type == CAD_VIEW_FRONT) {
                    p->pointx = cx + x * c - y * s; p->pointy = cy + x * s + y * c;
                } else {
                    p->pointy = cy + y * c - z * s; p->pointz = cz + y * s + z * c;
                }
            }
        }
    }
    g->cad->isDirty = 1;
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
        gui_set_status(g, "Point pending: choose a different orthographic view to set the remaining axis");
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
            CadPoint* p = &g->cad->data.points[i];
            if (!p->flags) continue;
            int x, y;
            CadView_ProjectPoint(&g->views[g->area_select_view], p->pointx, p->pointy, p->pointz,
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
                CadPoint* point = &g->cad->data.points[chain[i]];
                int x, y;
                CadView_ProjectPoint(&g->views[g->area_select_view], point->pointx, point->pointy, point->pointz,
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

static int topmost_view_at(const GuiState* g, int x, int y) {
    if (!g) return -1;
    for (int i = 3; i >= 0; --i) {
        if (g->view_visible[i] && pt_in_rect(x, y, g->view[i].r)) return i;
    }
    return -1;
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
    switch (item->command) {
    case CAD_COMMAND_EDIT_UNDO: return CadDocument_CanUndo(&g->document);
    case CAD_COMMAND_EDIT_REDO: return CadDocument_CanRedo(&g->document);
    case CAD_COMMAND_EDIT_COPY: return g->cad->selection.pointCount || g->cad->selection.polygonCount;
    case CAD_COMMAND_EDIT_PASTE: return g->clipboard_has_data;
    case CAD_COMMAND_OPTION_CHANGE_FIRST_POINT:
    case CAD_COMMAND_OPTION_FACE_SUPPORT:
    case CAD_COMMAND_OPTION_FACE_INFORMATION: return g->cad->selection.polygonCount > 0;
    default: return 1;
    }
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
            int width = g->font ? font_measure(g->font, menu->items[i].label)
                                : (int)strlen(menu->items[i].label) * 8;
            if (width > max_width) max_width = width;
        }
    }
    return (Rect){ x, MenuBarHeight(), max_width + 28, menu ? menu->count * 20 : 0 };
}

static void update_submenu_rect(GuiState* g, Rect popup, int row) {
    const CadMenuItemDescriptor* submenu = row == 4 ? importSubMenuItems : exportSubMenuItems;
    int max_width = 0;
    for (int i = 0; i < 2; ++i) {
        int width = g->font ? font_measure(g->font, submenu[i].label)
                            : (int)strlen(submenu[i].label) * 8;
        if (width > max_width) max_width = width;
    }
    g->submenu_rect = (Rect){ popup.x + popup.w - 2, popup.y + row * 20,
                              max_width + 24, 40 };
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
        g->submenu_hover_item = row >= 0 && row < 2 ? row : -1;
        if (in->mouse_pressed && g->submenu_hover_item >= 0) {
            const CadMenuItemDescriptor* submenu = g->submenu_open == 5 ? importSubMenuItems : exportSubMenuItems;
            CadCommandId command = submenu[g->submenu_hover_item].command;
            g->menu_open = -1; g->submenu_open = 0; g->submenu_hover_item = -1;
            execute_editor_command(g, command);
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
    int count = CadView_FindPointsAtLocation(&g->views[view_index], g->cad,
        mouse_x, mouse_y, content.x, content.y, content.w, content.h,
        10, 0.01, points, ARRAY_COUNT(points));
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
        if (!g->shape_preview_valid || g->shape_selected < 0) {
            gui_set_status(g, "Select a valid ASM shape preview before Replace");
            return;
        }
        if (confirm_replace_document(g, "replacing the document with the previewed ASM shape")) {
            const char* name = g->shape_names[g->shape_selected];
            replace_document(g, g->shape_preview, NULL, 1);
            CadDocument_SetLastImportPath(&g->document, g->shape_folder_path);
            gui_set_status(g, "Replaced document with %s; use Save As for native CAD", name);
        }
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

void gui_update(GuiState* g, const GuiInput* in, int win_w, int win_h) {
    if (!g || !in) return;
    if (g->auto_layout && (g->layout_width != win_w || g->layout_height != win_h)) {
        layout_cleanup(g, win_w, win_h);
    } else {
        g->layout_width = win_w;
        g->layout_height = win_h;
    }

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
            int right = r->x + g->resize_start_w, bottom = r->y + g->resize_start_h;
            if (g->resize_edge & 1) { r->x = in->mouse_x; r->w = right - r->x; }
            if (g->resize_edge & 2) r->w = g->resize_start_w + dx;
            if (g->resize_edge & 4) { r->y = in->mouse_y; r->h = bottom - r->y; }
            if (g->resize_edge & 8) r->h = g->resize_start_h + dy;
            if (r->w < 120) r->w = 120;
            if (r->h < 90) r->h = 90;
        }
        if (in->mouse_released || !in->mouse_down) { g->resize_win = NULL; g->resize_edge = 0; g->pointer_owner = GUI_POINTER_NONE; }
        return;
    }
    if (g->drag_win) {
        if (in->mouse_down) {
            g->drag_win->r.x = in->mouse_x - g->drag_off_x;
            g->drag_win->r.y = in->mouse_y - g->drag_off_y;
            if (g->drag_win->r.x < 0) g->drag_win->r.x = 0;
            if (g->drag_win->r.y < MenuBarHeight()) g->drag_win->r.y = MenuBarHeight();
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
            CadView* view = &g->views[g->view_middle_interacting];
            CadView_SetZoom(view, view->zoom * exp((double)-dy * 0.015));
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

    /* Topmost auxiliary windows own their entire rectangles, not just their
       controls. */
    if (g->shapeBrowserWindow.r.w > 0 && pt_in_rect(in->mouse_x, in->mouse_y, g->shapeBrowserWindow.r)) {
        if (in->mouse_pressed && begin_title_drag(g, &g->shapeBrowserWindow, in)) return;
        handle_shape_browser_click(g, in);
        if (in->mouse_pressed && in->mouse_down && !in->mouse_released) {
            g->pointer_owner = GUI_POINTER_SHAPE_BROWSER;
        } else if (in->mouse_released || !in->mouse_down) {
            g->pointer_owner = GUI_POINTER_NONE;
        }
        return;
    }
    if (g->animationWindow.r.w > 0 && pt_in_rect(in->mouse_x, in->mouse_y, g->animationWindow.r)) {
        if (in->mouse_pressed && begin_title_drag(g, &g->animationWindow, in)) return;
        if (in->mouse_pressed) gui_set_status(g, "Animation controls are read-only in this release");
        return;
    }
    if (g->coordinates_visible && pt_in_rect(in->mouse_x, in->mouse_y, g->coordBox.r)) {
        if (in->mouse_pressed) begin_title_drag(g, &g->coordBox, in);
        return;
    }
    if (g->tool_palette_visible && pt_in_rect(in->mouse_x, in->mouse_y, g->toolPalette.r)) {
        if (in->mouse_pressed && begin_title_drag(g, &g->toolPalette, in)) return;
        for (int slot = 0; slot < CAD_TOOL_COUNT; ++slot) {
            if (!pt_in_rect(in->mouse_x, in->mouse_y, tool_button_rect(g, slot))) continue;
            CadToolId tool = toolPaletteOrder[slot];
            if (!in->mouse_pressed) {
                snprintf(g->status_text, sizeof(g->status_text), "%s - %s%s",
                         toolDescriptors[tool].name, toolDescriptors[tool].help,
                         (toolDescriptors[tool].flags & CAD_TOOL_FLAG_DISABLED) ? " (unavailable)" : "");
            } else {
                if (g->selected_tool == tool && !(toolDescriptors[tool].flags & CAD_TOOL_FLAG_IMMEDIATE)) {
                    g->selected_tool = CAD_TOOL_NONE;
                    reset_interaction(g);
                    gui_set_status(g, "Tool cancelled");
                } else activate_tool(g, tool);
            }
            break;
        }
        return;
    }

    /* Resolve one complete desktop view before considering any of its child
       regions.  This preserves z-order when movable windows overlap. */
    int topmost_view = topmost_view_at(g, in->mouse_x, in->mouse_y);
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
        CadView* view = &g->views[hovered_view];
        CadView_SetZoom(view, view->zoom * exp((double)in->wheel_delta * 0.10));
        return;
    }
    if (hovered_view >= 0 && in->mouse_middle_pressed) {
        if (in->mouse_middle_down && !in->mouse_middle_released) {
            g->view_middle_interacting = hovered_view;
            g->last_mouse_y = in->mouse_y;
            g->pointer_owner = GUI_POINTER_VIEW;
        }
        return;
    }
    if (hovered_view >= 0 && in->mouse_right_pressed) {
        if (g->selected_tool == CAD_TOOL_FACE_CREATE) {
            Rect content = view_content_rect(g, hovered_view);
            int16_t point = CadView_FindNearestPoint(&g->views[hovered_view], g->cad,
                in->mouse_x, in->mouse_y, content.x, content.y, content.w, content.h, 10);
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
        int16_t point = CadView_FindNearestPoint(&g->views[hovered_view], g->cad,
            in->mouse_x, in->mouse_y, content.x, content.y, content.w, content.h, 10);
        if (point != INVALID_INDEX && !CadCore_IsPointSelected(g->cad, point) &&
            g->cad->selection.pointCount < CAD_MAX_FACE_POINTS) {
            CadCore_SelectPoint(g->cad, point);
            gui_set_status(g, "Face: %d/%d points; right-click final point",
                           g->cad->selection.pointCount, CAD_MAX_FACE_POINTS);
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

static void gui_draw_gui_elements(GuiState* g, int win_w, int win_h) {
    if (!g) return;
    
    /* Viewport and projection already set in gui_draw */
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    /* Menu bar */
    rg_fill_rect(0, 0, win_w, MenuBarHeight(), (RG_Color){230,230,230,255});
    rg_line(0, MenuBarHeight(), win_w, MenuBarHeight(), (RG_Color){0,0,0,255});
    if (g->font) {
        int x = 8;
        for (int i = 0; i < g->menu_count; i++) {
            font_draw(g->font, x, 3, g->menus[i], 0);
            x += font_measure(g->font, g->menus[i]) + 16;
        }
    }

    if (g->tool_palette_visible) draw_window_chrome(g, &g->toolPalette, win_h, 1.0f, 1.0f);
    /* Note: coordBox and animationWindow chrome drawn after CAD views so they appear on top */

    /* Tool palette contents - draw tool icons in 2 columns */
    RG_Color btn = { 245,245,245,255 };
    RG_Color edge = { 120,120,120,255 };
    if (g->tool_palette_visible) {
        ToolMetrics metrics = tool_metrics(g);
        for (int slot = 0; slot < CAD_TOOL_COUNT; ++slot) {
            CadToolId tool = toolPaletteOrder[slot];
            Rect button = tool_button_rect(g, slot);
            int disabled = (toolDescriptors[tool].flags & CAD_TOOL_FLAG_DISABLED) != 0;
            rg_fill_rect(button.x, button.y, button.w, button.h,
                         disabled ? (RG_Color){220,220,220,255} : btn);
            rg_stroke_rect(button.x, button.y, button.w, button.h, edge);
            if (g->tool_icons[tool]) {
                int icon_x = button.x + (button.w - metrics.icon_w) / 2;
                int icon_y = button.y + (button.h - metrics.icon_h) / 2;
                if (g->selected_tool == tool) {
                    rg_fill_rect(button.x + 1, button.y + 1, button.w - 2, button.h - 2,
                                 (RG_Color){190,210,235,255});
                    rg_draw_texture(g->tool_icons[tool], icon_x, icon_y,
                                    metrics.icon_w, metrics.icon_h);
                } else {
                    rg_draw_texture_inverted(g->tool_icons[tool], icon_x, icon_y,
                                             metrics.icon_w, metrics.icon_h);
                }
            }
            if (disabled) {
                rg_line(button.x + 3, button.y + 3, button.x + button.w - 3,
                        button.y + button.h - 3, (RG_Color){130,130,130,255});
            }
        }
    }

    /* Note: Coordinates box and Animation window content drawn after CAD views */
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
        int disabled = (toolDescriptors[tool].flags & CAD_TOOL_FLAG_DISABLED) != 0;
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

static int command_checked(const GuiState* g, CadCommandId command) {
    switch (command) {
    case CAD_COMMAND_WINDOW_TOP: return g->view_visible[0];
    case CAD_COMMAND_WINDOW_3D: return g->view_visible[1];
    case CAD_COMMAND_WINDOW_FRONT: return g->view_visible[2];
    case CAD_COMMAND_WINDOW_RIGHT: return g->view_visible[3];
    case CAD_COMMAND_WINDOW_COORDINATES: return g->coordinates_visible;
    case CAD_COMMAND_WINDOW_TOOL_PALETTE: return g->tool_palette_visible;
    case CAD_COMMAND_WINDOW_TEN_KEY: return g->state_visible;
    case CAD_COMMAND_OPTION_WIREFRAME: return g->views[1].wireframe;
    case CAD_COMMAND_OPTION_SOLID: return !g->views[1].wireframe;
    default: return 0;
    }
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
                     ? " (corresponding animation frames included)" : "");
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
    if (g->font) font_draw(g->font, row.x + 18, row.y + 3, item->label, 0);
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
        Rect r = g->submenu_rect;
        rg_fill_rect(r.x, r.y, r.w, r.h, (RG_Color){245,245,245,255});
        rg_stroke_rect(r.x, r.y, r.w, r.h, (RG_Color){0,0,0,255});
        for (int i = 0; i < 2; ++i) {
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
    for (int i = 0; i < 4; i++) {
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
        
        CadView_Render(&g->views[i], g->cad,
                       scaled_x, scaled_y, scaled_w, scaled_h, fb_h,
                       content.w, content.h);
        
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
        if (!axes || (axes & g->point_known_axes) != axes) return;
        int x, y;
        CadView_ProjectPoint(&g->views[view_index], g->point_pending_x,
                             g->point_pending_y, g->point_pending_z,
                             &x, &y, content.w, content.h);
        x += content.x;
        y += content.y;
        if (y >= content.y && y < content.y + content.h) {
            int x1 = x - 7 < content.x ? content.x : x - 7;
            int x2 = x + 7 >= content.x + content.w ? content.x + content.w - 1 : x + 7;
            if (x2 >= x1) rg_line(x1, y, x2, y, (RG_Color){220,45,45,255});
        }
        if (x >= content.x && x < content.x + content.w) {
            int y1 = y - 7 < content.y ? content.y : y - 7;
            int y2 = y + 7 >= content.y + content.h ? content.y + content.h - 1 : y + 7;
            if (y2 >= y1) rg_line(x, y1, x, y2, (RG_Color){220,45,45,255});
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

    /* Step 1: Draw GUI elements (menu bar, tool palette, windows) */
    gui_draw_gui_elements(g, win_w, win_h);
    
    /* Step 2: Draw CAD models in viewports (with proper 3D/depth state) */
    gui_draw_cad_views(g, win_w, win_h, fb_w, fb_h, in);
    draw_tool_palette_overlay(g, win_h);
    
    /* Step 3: Draw windows that should appear on top of CAD models */
    if (g->coordinates_visible) draw_window_chrome(g, &g->coordBox, win_h, 1.0f, 1.0f);
    if (g->animationWindow.r.w > 0 && g->animationWindow.r.h > 0) {
        draw_window_chrome(g, &g->animationWindow, win_h, 1.0f, 1.0f);
    }
    
    if (g->shapeBrowserWindow.r.w > 0 && g->shapeBrowserWindow.r.h > 0) {
        draw_window_chrome(g, &g->shapeBrowserWindow, win_h, 1.0f, 1.0f);
    }
    
    /* Draw coordinates box content */
    if (g->coordinates_visible) {
    Rect cr = g->coordBox.r;
    Rect cinner = (Rect){ cr.x + 6, cr.y + 26, cr.w - 12, cr.h - 32 };
    rg_fill_rect(cinner.x, cinner.y, cinner.w, cinner.h, (RG_Color){250,250,250,255});
    rg_stroke_rect(cinner.x, cinner.y, cinner.w, cinner.h, (RG_Color){120,120,120,255});
    
    if (g->font && g->cad) {
        char coord_str[128];
        if (g->cad->selection.pointCount > 0) {
            /* Calculate average of selected points */
            double avg_x = 0.0, avg_y = 0.0, avg_z = 0.0;
            int valid_count = 0;
            
            for (int i = 0; i < g->cad->selection.pointCount; i++) {
                int16_t point_idx = g->cad->selection.selectedPoints[i];
                if (point_idx < 0) continue;
                
                CadPoint* pt = CadCore_GetPoint(g->cad, point_idx);
                if (!pt) continue;
                
                avg_x += pt->pointx;
                avg_y += pt->pointy;
                avg_z += pt->pointz;
                valid_count++;
            }
            
            if (valid_count > 0) {
                avg_x /= valid_count;
                avg_y /= valid_count;
                avg_z /= valid_count;
                
                /* Check if all points are at the same location (merged points) */
                int all_same_location = 1;
                const double location_threshold = 0.01; /* 0.01 unit threshold */
                
                if (valid_count > 1) {
                    for (int i = 0; i < g->cad->selection.pointCount; i++) {
                        int16_t point_idx = g->cad->selection.selectedPoints[i];
                        if (point_idx < 0) continue;
                        
                        CadPoint* pt = CadCore_GetPoint(g->cad, point_idx);
                        if (!pt) continue;
                        
                        double dx = pt->pointx - avg_x;
                        double dy = pt->pointy - avg_y;
                        double dz = pt->pointz - avg_z;
                        double dist_sq = dx * dx + dy * dy + dz * dz;
                        
                        if (dist_sq > location_threshold * location_threshold) {
                            all_same_location = 0;
                            break;
                        }
                    }
                }
                
                if (valid_count == 1 || all_same_location) {
                    /* Single point or all points at same location - show coordinates */
                    snprintf(coord_str, sizeof(coord_str), "X=%.2f   Y=%.2f   Z=%.2f", avg_x, avg_y, avg_z);
                } else {
                    /* Multiple points at different locations - show average */
                    snprintf(coord_str, sizeof(coord_str), "X=%.2f   Y=%.2f   Z=%.2f  (avg of %d)", avg_x, avg_y, avg_z, valid_count);
                }
            } else {
                snprintf(coord_str, sizeof(coord_str), "No valid points selected");
            }
        } else {
            snprintf(coord_str, sizeof(coord_str), "No points selected");
        }
        char panel_text[256];
        snprintf(panel_text, sizeof(panel_text), "%s   |   3D RX=%.1f RY=%.1f RZ=%.1f  Zoom=%.2fx",
                 coord_str, g->views[1].rot_x, g->views[1].rot_y,
                 g->views[1].rot_z, g->views[1].zoom);
        font_draw(g->font, cinner.x + 8, cinner.y + 6, panel_text, 0);
    }
    }
    
    /* Draw animation window content */
    if (g->animationWindow.r.w > 0 && g->animationWindow.r.h > 0) {
        Rect ar = g->animationWindow.r;
        Rect inner = { ar.x + 6, ar.y + 26, ar.w - 12, ar.h - 32 };
        rg_fill_rect(inner.x, inner.y, inner.w, inner.h, (RG_Color){242,242,242,255});
        rg_stroke_rect(inner.x, inner.y, inner.w, inner.h, (RG_Color){120,120,120,255});
        if (g->font) {
            char summary[160];
            snprintf(summary, sizeof(summary), "%s  |  %d index record(s), %d animation point(s)",
                     CadDocument_HasAnimation(&g->document) ? "Animation preserved" : "No animation data",
                     g->cad->data.animationIndexCount, g->cad->data.animationPointCount);
            font_draw(g->font, inner.x + 8, inner.y + 8, summary, 0);
            font_draw(g->font, inner.x + 8, inner.y + 32,
                      "Editing and playback are deferred; static transforms preserve frame coordinates.", 0);
        }
    }

    /* Draw shape browser window content */
    if (g->shapeBrowserWindow.r.w > 0 && g->shapeBrowserWindow.r.h > 0) {
        ShapeBrowserLayout layout = shape_browser_layout(g);
        rg_fill_rect(layout.inner.x, layout.inner.y, layout.inner.w, layout.inner.h,
                     (RG_Color){250,250,250,255});
        rg_stroke_rect(layout.inner.x, layout.inner.y, layout.inner.w, layout.inner.h,
                       (RG_Color){120,120,120,255});
        int result_count = filtered_shape_count(g);
        int visible_items = layout.list.h / 20;
        int max_scroll = result_count > visible_items ? result_count - visible_items : 0;
        if (g->shape_scroll_offset > max_scroll) g->shape_scroll_offset = max_scroll;
        if (g->shape_scroll_offset < 0) g->shape_scroll_offset = 0;

        if (g->font) {
            char title[160];
            snprintf(title, sizeof(title), "Recovered ASM shapes: %d total, %d matching",
                     g->shape_count, result_count);
            font_draw(g->font, layout.inner.x + 8, layout.inner.y + 7, title, 0);
            font_draw(g->font, layout.inner.x + 8, layout.inner.y + 27,
                      g->shape_folder_path, 0);
            font_draw(g->font, layout.inner.x + 8, layout.search.y + 5, "Find:", 0);
        }

        rg_fill_rect(layout.search.x, layout.search.y, layout.search.w, layout.search.h,
                     g->shape_search_active ? (RG_Color){210,224,248,255}
                                            : (RG_Color){255,255,255,255});
        rg_stroke_rect(layout.search.x, layout.search.y, layout.search.w, layout.search.h,
                       g->shape_search_active ? (RG_Color){45,90,170,255}
                                              : (RG_Color){110,110,110,255});
        if (g->font) {
            font_draw(g->font, layout.search.x + 5, layout.search.y + 5,
                      g->shape_search[0] ? g->shape_search : "type to filter...", 0);
        }

        rg_fill_rect(layout.list.x, layout.list.y, layout.list.w, layout.list.h,
                     (RG_Color){255,255,255,255});
        rg_stroke_rect(layout.list.x, layout.list.y, layout.list.w, layout.list.h,
                       (RG_Color){80,80,80,255});
        for (int row = 0; row < visible_items; ++row) {
            int source_index = filtered_shape_index(g, g->shape_scroll_offset + row);
            if (source_index < 0) break;
            Rect item = { layout.list.x + 2, layout.list.y + row * 20 + 1,
                          layout.list.w - (max_scroll > 0 ? 16 : 4), 18 };
            if (source_index == g->shape_selected) {
                rg_fill_rect(item.x, item.y, item.w, item.h, (RG_Color){190,210,240,255});
            }
            if (g->font) {
                font_draw(g->font, item.x + 3, item.y + 3,
                          g->shape_names[source_index], 0);
            }
        }

        if (max_scroll > 0) {
            Rect track = { layout.list.x + layout.list.w - 14, layout.list.y, 14,
                           layout.list.h };
            int thumb_h = visible_items * track.h / result_count;
            if (thumb_h < 24) thumb_h = 24;
            int thumb_y = track.y + g->shape_scroll_offset *
                          (track.h - thumb_h) / max_scroll;
            rg_fill_rect(track.x, track.y, track.w, track.h, (RG_Color){220,220,220,255});
            rg_stroke_rect(track.x, track.y, track.w, track.h, (RG_Color){100,100,100,255});
            rg_fill_rect(track.x + 2, thumb_y, track.w - 4, thumb_h,
                         (RG_Color){145,145,145,255});
        }

        rg_fill_rect(layout.preview.x, layout.preview.y, layout.preview.w, layout.preview.h,
                     (RG_Color){238,238,238,255});
        rg_stroke_rect(layout.preview.x, layout.preview.y, layout.preview.w, layout.preview.h,
                       (RG_Color){80,80,80,255});
        if (g->font) {
            char preview_title[160];
            if (g->shape_preview_valid && g->shape_selected >= 0) {
                snprintf(preview_title, sizeof(preview_title), "%s - %d points, %d faces",
                         g->shape_names[g->shape_selected],
                         g->shape_preview->data.pointCount,
                         g->shape_preview->data.polygonCount);
            } else {
                snprintf(preview_title, sizeof(preview_title), "Select a shape to preview");
            }
            font_draw(g->font, layout.preview.x + 6, layout.preview.y + 5,
                      preview_title, 0);
        }

        Rect preview_view = { layout.preview.x + 4, layout.preview.y + 25,
                              layout.preview.w - 8, layout.preview.h - 29 };
        if (g->shape_preview_valid && preview_view.w > 1 && preview_view.h > 1) {
            float scale_x = win_w > 0 ? (float)fb_w / (float)win_w : 1.0f;
            float scale_y = win_h > 0 ? (float)fb_h / (float)win_h : 1.0f;
            int pixel_x = (int)lroundf(preview_view.x * scale_x);
            int pixel_y = (int)lroundf(preview_view.y * scale_y);
            int pixel_right = (int)lroundf((preview_view.x + preview_view.w) * scale_x);
            int pixel_bottom = (int)lroundf((preview_view.y + preview_view.h) * scale_y);
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
            rg_stroke_rect(preview_view.x, preview_view.y, preview_view.w, preview_view.h,
                           (RG_Color){100,100,100,255});
        }

        RG_Color replace_fill = g->shape_preview_valid
                                ? (RG_Color){195,218,198,255}
                                : (RG_Color){225,225,225,255};
        rg_fill_rect(layout.replace_button.x, layout.replace_button.y,
                     layout.replace_button.w, layout.replace_button.h, replace_fill);
        rg_stroke_rect(layout.replace_button.x, layout.replace_button.y,
                       layout.replace_button.w, layout.replace_button.h,
                       (RG_Color){100,100,100,255});
        if (g->font) {
            font_draw(g->font, layout.replace_button.x + 20,
                      layout.replace_button.y + 5, "Replace", 0);
        }
    }
    
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


