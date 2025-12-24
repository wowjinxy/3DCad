#define _CRT_SECURE_NO_WARNINGS

#include "gui.h"
#include "render_gl.h"
#include "font_win32.h"
#include "cad_core.h"
#include "file_dialog.h"
#include "cad_view.h"
#include "cad_export_obj.h"
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

#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define TOOL_COUNT 24

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

struct GuiState {
    FontWin32* font;

    /* CAD core */
    CadCore* cad;
    char current_filename[260]; /* Current file path (MAX_PATH) */
    
    /* View states */
    CadView views[4];        /* One view state per view window */

    /* Layout windows (match screenshot-ish geometry) */
    GuiWin toolPalette;      /* 120x410 left */
    GuiWin view[4];          /* 4 view windows */
    GuiWin coordBox;         /* coordinates/info */
    GuiWin animationWindow;  /* Animation window */

    /* Menu bar */
    const char* menus[5];
    int menu_count;
    int menu_open; /* index or -1 */
    int menu_hover_item; /* 0-based within open menu, -1 none */

    /* Tool icons */
    RG_Texture* tool_icons[TOOL_COUNT];
    int selected_tool; /* Currently selected tool index, or -1 if none */
    
    /* Animation icons */
    RG_Texture* anim_icons[12]; /* Animation control icons */

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
    int last_mouse_x;
    int last_mouse_y;
    
    /* Point move state */
    int point_move_active; /* 1 if currently moving points, 0 otherwise */
    int point_move_view; /* View index where point move started */
    
    /* View window scaling (individual scale per view) */
    float view_scale[4]; /* Scale factor for each view window (default 1.0) */
    
    /* Animation state */
    int anim_current_frame; /* Current frame number (0-based) */
    int anim_total_frames;  /* Total number of frames */
    int anim_playing;        /* 1 if playing, 0 if paused */
    int anim_loop;          /* 1 if looping, 0 if not */
};

static int MenuBarHeight(void) { return 20; }

/* -------------------------------------------------------------------------
   Menu definitions (ported from 3DCad/include/MenuRes.h)
   ------------------------------------------------------------------------- */
static const char* fileMenuItems[] = {
    " File",
    "(N)New",
    "(O)Open...",
    "(S)Save",
    " Save As...",
    " Import",
    " Export",
    "-",
    " Load Color...",
    " Load Pallet...",
    " Animation",
    "-",
    "(Q)Quit",
    NULL
};

static const char* editMenuItems[] = {
    " Edit",
    "(U)Undo",
    " Memory",
    " Paste",
    "-",
    " Copy",
    NULL
};

static const char* windowMenuItems[] = {
    " Windows",
    " Top",
    " Front",
    " Right",
    " 3D View",
    "-",
    "(C)Coordinates",
    " tool palette",
    " TenKey",
    "-",
    " Clean Up",
    " Home",
    "-",
    " All Scales Reset",
    NULL
};

static const char* optionMenuItems[] = {
    " Options",
    " Area Select",
    " Select All",
    " Change Point",
    " Flat Check",
    " F.Support",
    " F.Information",
    "-",
    " Wire Frame",
    " Solid",
    NULL
};

static const char* mergeMenuItems[] = {
    " Merge",
    " Grid Merge",
    " Point Merge",
    " Polygon Merge ",
    " All Merge",
    "-",
    " Polygon Sort",
    NULL
};

static const char* const* menu_items_for_index(int idx) {
    switch (idx) {
    case 0: return fileMenuItems;
    case 1: return editMenuItems;
    case 2: return windowMenuItems;
    case 3: return optionMenuItems;
    case 4: return mergeMenuItems;
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

static void handle_file_menu_action(GuiState* g, int item_index) {
    if (!g || !g->cad) return;
    
    char filename[260];
    
    switch (item_index) {
    case 1: /* (N)New */
        /* Check if we need to save first */
        if (g->cad->isDirty && g->current_filename[0] != '\0') {
            /* TODO: Ask user if they want to save */
        }
        CadCore_Clear(g->cad);
        g->current_filename[0] = '\0';
        fprintf(stdout, "New file created\n");
        break;
    case 2: /* (O)Open... */
        if (FileDialog_OpenCAD(filename, sizeof(filename))) {
            /* Clear all state before loading */
            CadCore_ClearSelection(g->cad);
            g->point_move_active = 0;
            g->point_move_view = -1;
            g->view_interacting = -1;
            g->view_right_interacting = -1;
            
            /* Reset view states */
            for (int i = 0; i < 4; i++) {
                CadView_Reset(&g->views[i]);
            }
            
            if (CadCore_LoadFile(g->cad, filename)) {
                strncpy(g->current_filename, filename, sizeof(g->current_filename) - 1);
                g->current_filename[sizeof(g->current_filename) - 1] = '\0';
                g->cad->isDirty = 0; /* Reset dirty flag after successful load */
                fprintf(stdout, "Opened file: %s\n", filename);
            } else {
                fprintf(stderr, "Error: Failed to open file: %s\n", filename);
            }
        }
        break;
    case 3: /* (S)Save */
        if (g->current_filename[0] != '\0') {
            /* Save to current filename */
            if (CadCore_SaveFile(g->cad, g->current_filename)) {
                fprintf(stdout, "Saved file: %s\n", g->current_filename);
            } else {
                fprintf(stderr, "Error: Failed to save file: %s\n", g->current_filename);
            }
        } else {
            /* No current filename, use Save As */
            if (FileDialog_SaveCAD(filename, sizeof(filename))) {
                if (CadCore_SaveFile(g->cad, filename)) {
                    strncpy(g->current_filename, filename, sizeof(g->current_filename) - 1);
                    g->current_filename[sizeof(g->current_filename) - 1] = '\0';
                    fprintf(stdout, "Saved file: %s\n", filename);
                } else {
                    fprintf(stderr, "Error: Failed to save file: %s\n", filename);
                }
            }
        }
        break;
    case 4: /* Save As... */
        if (FileDialog_SaveCAD(filename, sizeof(filename))) {
            if (CadCore_SaveFile(g->cad, filename)) {
                strncpy(g->current_filename, filename, sizeof(g->current_filename) - 1);
                g->current_filename[sizeof(g->current_filename) - 1] = '\0';
                fprintf(stdout, "Saved file: %s\n", filename);
            } else {
                fprintf(stderr, "Error: Failed to save file: %s\n", filename);
            }
        }
        break;
    case 5: /* Import */
        fprintf(stdout, "Import (not implemented)\n");
        break;
    case 6: /* Export */
        {
            char filename[260];
            if (FileDialog_Save(filename, sizeof(filename), 
                              "OBJ Files\0*.obj\0All Files\0*.*\0", 
                              "Export OBJ")) {
                if (CadExport_OBJ(g->cad, filename)) {
                    fprintf(stdout, "Exported to: %s\n", filename);
                } else {
                    fprintf(stderr, "Error: Failed to export OBJ file\n");
                }
            }
        }
        break;
    case 8: /* Load Color... */
        fprintf(stdout, "Load Color (not implemented)\n");
        break;
    case 9: /* Load Pallet... */
        fprintf(stdout, "Load Palette (not implemented)\n");
        break;
    case 10: /* Animation */
        /* Toggle animation window visibility */
        if (g->animationWindow.r.w == 0 || g->animationWindow.r.h == 0) {
            /* Show window */
            g->animationWindow.r = (Rect){ 500, 200, 430, 150 };
            fprintf(stdout, "Animation window opened\n");
        } else {
            /* Hide window */
            g->animationWindow.r.w = 0;
            g->animationWindow.r.h = 0;
            fprintf(stdout, "Animation window closed\n");
        }
        break;
    case 12: /* (Q)Quit */
        fprintf(stdout, "Quit (application exit not handled here)\n");
        break;
    }
}

static void handle_edit_menu_action(GuiState* g, int item_index) {
    if (!g || !g->cad) return;
    
    switch (item_index) {
    case 1: /* (U)Undo */
        fprintf(stdout, "Undo (not implemented yet)\n");
        break;
    case 2: /* Memory */
        fprintf(stdout, "Memory (not implemented yet)\n");
        break;
    case 3: /* Paste */
        fprintf(stdout, "Paste (not implemented yet)\n");
        break;
    case 5: /* Copy */
        fprintf(stdout, "Copy (not implemented yet)\n");
        break;
    }
}

/* Helper function to update a view window size based on its scale */
static void update_view_window_size(GuiState* g, int view_idx) {
    if (!g || view_idx < 0 || view_idx >= 4) return;
    
    const int baseX = 180, baseY = 20;
    const int baseWinW = 560, baseWinH = 330;
    int winW = (int)(baseWinW * g->view_scale[view_idx]);
    int winH = (int)(baseWinH * g->view_scale[view_idx]);
    
    /* Calculate position based on grid layout */
    switch (view_idx) {
    case 0: /* Top - top-left */
        {
            int winW1 = (int)(baseWinW * g->view_scale[1]);
            g->view[0].r = (Rect){ baseX + 0, baseY + 0, winW, winH };
            /* Update 3D view position */
            g->view[1].r.x = baseX + winW;
        }
        break;
    case 1: /* 3D View - top-right */
        {
            int winW0 = (int)(baseWinW * g->view_scale[0]);
            g->view[1].r = (Rect){ baseX + winW0, baseY + 0, winW, winH };
        }
        break;
    case 2: /* Front - bottom-left */
        {
            int winH0 = (int)(baseWinH * g->view_scale[0]);
            int winW1 = (int)(baseWinW * g->view_scale[1]);
            g->view[2].r = (Rect){ baseX + 0, baseY + winH0, winW, winH };
            /* Update Right view position */
            g->view[3].r.x = baseX + winW;
        }
        break;
    case 3: /* Right - bottom-right */
        {
            int winH0 = (int)(baseWinH * g->view_scale[0]);
            int winW0 = (int)(baseWinW * g->view_scale[0]);
            g->view[3].r = (Rect){ baseX + winW0, baseY + winH0, winW, winH };
        }
        break;
    }
}

static void handle_window_menu_action(GuiState* g, int item_index) {
    if (!g) return;
    
    switch (item_index) {
    case 1: /* Top */
        fprintf(stdout, "Toggle Top view window\n");
        break;
    case 2: /* Front */
        fprintf(stdout, "Toggle Front view window\n");
        break;
    case 3: /* Right */
        fprintf(stdout, "Toggle Right view window\n");
        break;
    case 4: /* 3D View */
        fprintf(stdout, "Toggle 3D View window\n");
        break;
    case 6: /* (C)Coordinates */
        fprintf(stdout, "Toggle Coordinates window\n");
        break;
    case 7: /* tool palette */
        fprintf(stdout, "Toggle Tool Palette window\n");
        break;
    case 8: /* TenKey */
        fprintf(stdout, "Show TenKey window\n");
        break;
    case 10: /* Clean Up */
        /* Reset window positions to home (preserves current scales) */
        g->toolPalette.r = (Rect){ 20, 20, 90, 668 };
        {
            const int baseX = 180, baseY = 20;
            const int baseWinW = 560, baseWinH = 330;
            int winW0 = (int)(baseWinW * g->view_scale[0]);
            int winH0 = (int)(baseWinH * g->view_scale[0]);
            int winW1 = (int)(baseWinW * g->view_scale[1]);
            int winH1 = (int)(baseWinH * g->view_scale[1]);
            int winW2 = (int)(baseWinW * g->view_scale[2]);
            int winH2 = (int)(baseWinH * g->view_scale[2]);
            int winW3 = (int)(baseWinW * g->view_scale[3]);
            int winH3 = (int)(baseWinH * g->view_scale[3]);
            g->view[0].r = (Rect){ baseX + 0,     baseY + 0,     winW0, winH0 };
            g->view[1].r = (Rect){ baseX + winW0,  baseY + 0,     winW1, winH1 };
            g->view[2].r = (Rect){ baseX + 0,     baseY + winH0,  winW2, winH2 };
            g->view[3].r = (Rect){ baseX + winW0,  baseY + winH0,  winW3, winH3 };
        }
        g->coordBox.r = (Rect){ 20, 860, 425, 80 };
        fprintf(stdout, "Windows cleaned up\n");
        break;
    case 11: /* Home */
        fprintf(stdout, "Home (not implemented yet)\n");
        break;
    case 12: /* All Scales Reset */
        for (int i = 0; i < 4; i++) {
            g->view_scale[i] = 1.0f;
        }
        {
            const int baseX = 180, baseY = 20;
            const int baseWinW = 560, baseWinH = 330;
            int winW = baseWinW;
            int winH = baseWinH;
            g->view[0].r = (Rect){ baseX + 0,     baseY + 0,     winW, winH };
            g->view[1].r = (Rect){ baseX + winW,  baseY + 0,     winW, winH };
            g->view[2].r = (Rect){ baseX + 0,     baseY + winH,  winW, winH };
            g->view[3].r = (Rect){ baseX + winW,  baseY + winH,  winW, winH };
        }
        fprintf(stdout, "All view scales reset to 1.0x\n");
        break;
    }
}

static void handle_option_menu_action(GuiState* g, int item_index) {
    if (!g || !g->cad) return;
    
    switch (item_index) {
    case 1: /* Area Select */
        /* Toggle selection mode */
        g->cad->selectModeFlag = !g->cad->selectModeFlag;
        if (g->cad->selectModeFlag) {
            CadCore_SetEditMode(g->cad, CAD_MODE_SELECT_POINT);
            fprintf(stdout, "Selection mode: Point\n");
        } else {
            CadCore_SetEditMode(g->cad, CAD_MODE_SELECT_POLYGON);
            fprintf(stdout, "Selection mode: Polygon\n");
        }
        break;
    case 2: /* Select All */
        CadCore_SelectAll(g->cad);
        fprintf(stdout, "Selected all\n");
        break;
    case 3: /* Change Point */
        fprintf(stdout, "Change Point (not implemented yet)\n");
        break;
    case 4: /* Flat Check */
        fprintf(stdout, "Flat Check (not implemented yet)\n");
        break;
    case 5: /* F.Support */
        fprintf(stdout, "Face Support toggle (not implemented yet)\n");
        break;
    case 6: /* F.Information */
        fprintf(stdout, "Face Information window (not implemented yet)\n");
        break;
    case 8: /* Wire Frame */
        /* Toggle all views to wireframe mode */
        for (int i = 0; i < 4; i++) {
            g->views[i].wireframe = 1;
        }
        fprintf(stdout, "Wire Frame mode enabled\n");
        break;
    case 9: /* Solid */
        /* Toggle all views to solid mode */
        for (int i = 0; i < 4; i++) {
            g->views[i].wireframe = 0;
        }
        fprintf(stdout, "Solid mode enabled\n");
        break;
    }
}

static void handle_merge_menu_action(GuiState* g, int item_index) {
    if (!g || !g->cad) return;
    
    switch (item_index) {
    case 1: /* Merge */
        fprintf(stdout, "Merge coordinates (not implemented yet)\n");
        break;
    case 2: /* Grid Merge */
        fprintf(stdout, "Grid Merge (not implemented yet)\n");
        break;
    case 3: /* Point Merge */
        fprintf(stdout, "Point Merge (not implemented yet)\n");
        break;
    case 4: /* Polygon Merge */
        fprintf(stdout, "Polygon Merge (not implemented yet)\n");
        break;
    case 5: /* All Merge */
        fprintf(stdout, "All Merge (not implemented yet)\n");
        break;
    case 7: /* Polygon Sort */
        fprintf(stdout, "Polygon Sort (not implemented yet)\n");
        break;
    }
}

static void handle_menu_action(GuiState* g, int menu_index, int item_index) {
    if (!g) return;
    
    switch (menu_index) {
    case 0: handle_file_menu_action(g, item_index); break;
    case 1: handle_edit_menu_action(g, item_index); break;
    case 2: handle_window_menu_action(g, item_index); break;
    case 3: handle_option_menu_action(g, item_index); break;
    case 4: handle_merge_menu_action(g, item_index); break;
    }
}

GuiState* gui_create(void) {
    GuiState* g = (GuiState*)calloc(1, sizeof(GuiState));
    if (!g) return NULL;

    /* Initialize CAD core */
    g->cad = (CadCore*)calloc(1, sizeof(CadCore));
    if (g->cad) {
        CadCore_Init(g->cad);
    }
    
    /* Initialize current filename */
    g->current_filename[0] = '\0';
    
    /* Initialize views - match window titles */
    CadView_Init(&g->views[0], CAD_VIEW_TOP);    /* "Top" */
    CadView_Init(&g->views[1], CAD_VIEW_3D);     /* "3D View" */
    CadView_Init(&g->views[2], CAD_VIEW_FRONT);  /* "Front" */
    CadView_Init(&g->views[3], CAD_VIEW_RIGHT);  /* "Right" */

    g->menus[0] = "File";
    g->menus[1] = "Edit";
    g->menus[2] = "Windows";
    g->menus[3] = "Options";
    g->menus[4] = "Merge";
    g->menu_count = 5;
    g->menu_open = -1;
    g->menu_hover_item = -1;

    /* Initialize tool icons to NULL */
    for (int i = 0; i < TOOL_COUNT; i++) {
        g->tool_icons[i] = NULL;
    }
    for (int i = 0; i < 12; i++) {
        g->anim_icons[i] = NULL;
    }
    g->selected_tool = -1; /* No tool selected initially */
    g->point_move_active = 0;
    g->point_move_view = -1;
    g->view_interacting = -1;
    g->view_right_interacting = -1;
    
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
    
    /* Initialize animation state */
    g->anim_current_frame = 0;
    g->anim_total_frames = 0;
    g->anim_playing = 0;
    g->anim_loop = 0;

    return g;
}

void gui_destroy(GuiState* g) {
    if (!g) return;
    /* Free CAD core */
    if (g->cad) {
        CadCore_Destroy(g->cad);
        free(g->cad);
    }
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
    free(g);
}

void gui_set_font(GuiState* g, FontWin32* font) {
    if (!g) return;
    g->font = font;
}

void gui_load_tool_icons(GuiState* g, const char* resource_path) {
    if (!g) return;
    
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
        snprintf(path, sizeof(path), "%s/%s", resource_path, tool_names[i]);
        g->tool_icons[i] = rg_load_texture(path);
        if (!g->tool_icons[i]) {
            fprintf(stderr, "Warning: Failed to load tool icon %d: %s\n", i, tool_names[i]);
        }
    }
}

void gui_load_anim_icons(GuiState* g, const char* resource_path) {
    if (!g) return;
    
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
        snprintf(path, sizeof(path), "%s/%s", resource_path, anim_names[i]);
        g->anim_icons[i] = rg_load_texture(path);
        if (!g->anim_icons[i]) {
            fprintf(stderr, "Warning: Failed to load animation icon %d: %s\n", i, anim_names[i]);
        }
    }
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

static void draw_scrollbars_placeholder(Rect inner) {
    /* Right + bottom scrollbar tracks */
    RG_Color sb = { 200,200,200,255 };
    RG_Color edge = { 120,120,120,255 };
    rg_fill_rect(inner.x + inner.w - 14, inner.y, 14, inner.h - 14, sb);
    rg_stroke_rect(inner.x + inner.w - 14, inner.y, 14, inner.h - 14, edge);
    rg_fill_rect(inner.x, inner.y + inner.h - 14, inner.w - 14, 14, sb);
    rg_stroke_rect(inner.x, inner.y + inner.h - 14, inner.w - 14, 14, edge);
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

void gui_update(GuiState* g, const GuiInput* in, int win_w, int win_h) {
    (void)win_w; (void)win_h;
    if (!g || !in) return;

    /* Drag windows by title bar */
    if (in->mouse_pressed) {
        /* Check for window resizing first (edges have priority over title bar) */
        const int resize_threshold = 5; /* 5 pixel threshold for edge detection */
        
        /* Check view windows for resize */
        for (int i = 0; i < 4 && !g->resize_win; i++) {
            Rect vr = g->view[i].r;
            Rect content = (Rect){ vr.x + 6, vr.y + 26, vr.w - 12, vr.h - 32 };
            /* Check if mouse is over window edge (but not in content area) */
            if (pt_in_rect(in->mouse_x, in->mouse_y, vr) && 
                !pt_in_rect(in->mouse_x, in->mouse_y, content)) {
                int edge = get_resize_edge(in->mouse_x, in->mouse_y, vr, resize_threshold);
                if (edge) {
                    g->resize_win = &g->view[i];
                    g->resize_edge = edge;
                    g->resize_start_x = in->mouse_x;
                    g->resize_start_y = in->mouse_y;
                    g->resize_start_w = vr.w;
                    g->resize_start_h = vr.h;
                    break;
                }
            }
        }
        
        /* If not resizing, check for dragging */
        if (!g->resize_win) {
            Rect titlebar;
            /* Tool palette */
            titlebar = (Rect){ g->toolPalette.r.x, g->toolPalette.r.y, g->toolPalette.r.w, 20 };
            if (g->toolPalette.draggable && pt_in_rect(in->mouse_x, in->mouse_y, titlebar)) {
                g->drag_win = &g->toolPalette;
            }
            for (int i = 0; i < 4 && !g->drag_win; i++) {
                titlebar = (Rect){ g->view[i].r.x, g->view[i].r.y, g->view[i].r.w, 20 };
                if (g->view[i].draggable && pt_in_rect(in->mouse_x, in->mouse_y, titlebar)) {
                    g->drag_win = &g->view[i];
                }
            }
            titlebar = (Rect){ g->coordBox.r.x, g->coordBox.r.y, g->coordBox.r.w, 20 };
            if (!g->drag_win && g->coordBox.draggable && pt_in_rect(in->mouse_x, in->mouse_y, titlebar)) {
                g->drag_win = &g->coordBox;
            }
            if (g->animationWindow.r.w > 0 && g->animationWindow.r.h > 0) {
                titlebar = (Rect){ g->animationWindow.r.x, g->animationWindow.r.y, g->animationWindow.r.w, 20 };
                if (!g->drag_win && g->animationWindow.draggable && pt_in_rect(in->mouse_x, in->mouse_y, titlebar)) {
                    g->drag_win = &g->animationWindow;
                }
            }

            if (g->drag_win) {
                g->drag_off_x = in->mouse_x - g->drag_win->r.x;
                g->drag_off_y = in->mouse_y - g->drag_win->r.y;
            }
        }
    }

    if (!in->mouse_down && !in->mouse_right_down) {
        g->drag_win = NULL;
        g->resize_win = NULL;
        g->resize_edge = 0;
        g->view_interacting = -1;
        g->view_right_interacting = -1;
        g->point_move_active = 0;
        g->point_move_view = -1;
    } else if (g->resize_win) {
        /* Handle window resizing */
        int dx = in->mouse_x - g->resize_start_x;
        int dy = in->mouse_y - g->resize_start_y;
        
        Rect* r = &g->resize_win->r;
        int new_x = r->x;
        int new_y = r->y;
        int new_w = g->resize_start_w;
        int new_h = g->resize_start_h;
        
        if (g->resize_edge & 1) { /* Left edge */
            new_x = g->resize_start_x + dx;
            new_w = g->resize_start_w - dx;
            if (new_w < 100) { new_w = 100; new_x = r->x + r->w - 100; }
        }
        if (g->resize_edge & 2) { /* Right edge */
            new_w = g->resize_start_w + dx;
            if (new_w < 100) new_w = 100;
        }
        if (g->resize_edge & 4) { /* Top edge */
            new_y = g->resize_start_y + dy;
            new_h = g->resize_start_h - dy;
            if (new_h < 50) { new_h = 50; new_y = r->y + r->h - 50; }
        }
        if (g->resize_edge & 8) { /* Bottom edge */
            new_h = g->resize_start_h + dy;
            if (new_h < 50) new_h = 50;
        }
        
        r->x = new_x;
        r->y = new_y;
        r->w = new_w;
        r->h = new_h;
        
        /* Update scale factor for view windows */
        if (g->resize_win >= &g->view[0] && g->resize_win <= &g->view[3]) {
            int view_idx = (int)(g->resize_win - &g->view[0]);
            const int baseWinW = 560, baseWinH = 330;
            /* Calculate scale from current size */
            float scale_w = (float)new_w / (float)baseWinW;
            float scale_h = (float)new_h / (float)baseWinH;
            g->view_scale[view_idx] = (scale_w + scale_h) / 2.0f; /* Average of both */
            if (g->view_scale[view_idx] < 0.5f) g->view_scale[view_idx] = 0.5f;
            if (g->view_scale[view_idx] > 2.0f) g->view_scale[view_idx] = 2.0f;
        }
    } else if (g->drag_win) {
        g->drag_win->r.x = in->mouse_x - g->drag_off_x;
        g->drag_win->r.y = in->mouse_y - g->drag_off_y;
        if (g->drag_win->r.x < 0) g->drag_win->r.x = 0;
        if (g->drag_win->r.y < MenuBarHeight()) g->drag_win->r.y = MenuBarHeight();
    } else if (g->point_move_active && g->point_move_view >= 0) {
        /* Handle point movement */
        int dx = in->mouse_x - g->last_mouse_x;
        int dy = in->mouse_y - g->last_mouse_y;
        
        if (dx != 0 || dy != 0) {
            CadView* view = &g->views[g->point_move_view];
            Rect vr = g->view[g->point_move_view].r;
            Rect content = (Rect){ vr.x + 6, vr.y + 26, vr.w - 12, vr.h - 32 };
            
            /* Convert screen delta to world delta */
            double world_dx, world_dy, world_dz;
            CadView_UnprojectDelta(view, dx, dy, content.w, content.h,
                                  &world_dx, &world_dy, &world_dz);
            
            /* Apply movement to all selected points */
            for (int i = 0; i < g->cad->selection.pointCount; i++) {
                int16_t point_idx = g->cad->selection.selectedPoints[i];
                if (point_idx < 0) continue;
                
                CadPoint* pt = CadCore_GetPoint(g->cad, point_idx);
                if (!pt) continue;
                
                pt->pointx += world_dx;
                pt->pointy += world_dy;
                pt->pointz += world_dz;
            }
            
            g->cad->isDirty = 1; /* Mark as modified */
        }
        
        g->last_mouse_x = in->mouse_x;
        g->last_mouse_y = in->mouse_y;
    } else if (g->view_interacting >= 0 && !g->resize_win) {
        /* Handle view interaction (rotation for 3D view, pan for others) */
        /* Note: Resizing windows does NOT affect the CAD view (zoom/pan/rotation) */
        int dx = in->mouse_x - g->last_mouse_x;
        int dy = in->mouse_y - g->last_mouse_y;
        
        if (g->views[g->view_interacting].type == CAD_VIEW_3D) {
            /* Rotate 3D view */
            CadView_Rotate(&g->views[g->view_interacting], dy * 0.5, dx * 0.5);
        } else {
            /* Pan other views */
            CadView_Pan(&g->views[g->view_interacting], dx, -dy);
        }
        
        g->last_mouse_x = in->mouse_x;
        g->last_mouse_y = in->mouse_y;
    }
    
    /* Handle right-click view interaction (works even with tools selected) */
    /* BUT: Disable right-click panning when make tool is active (tool 3) to allow right-click to finalize faces */
    int make_tool_active = (g->selected_tool == 3);
    
    /* Check for right-click press to start interaction */
    if (in->mouse_right_pressed && !g->drag_win && !g->resize_win && g->view_right_interacting < 0 && !make_tool_active) {
        for (int i = 0; i < 4; i++) {
            Rect vr = g->view[i].r;
            Rect content = (Rect){ vr.x + 6, vr.y + 26, vr.w - 12, vr.h - 32 };
            Rect titlebar = (Rect){ vr.x, vr.y, vr.w, 20 };
            
            if (pt_in_rect(in->mouse_x, in->mouse_y, content) && 
                !pt_in_rect(in->mouse_x, in->mouse_y, titlebar)) {
                /* Right-click works in all views */
                g->view_right_interacting = i;
                g->last_mouse_x = in->mouse_x;
                g->last_mouse_y = in->mouse_y;
                break;
            }
        }
    }
    
    /* Handle right-click dragging (pan in all views) */
    /* Also disable if make tool is active */
    if (g->view_right_interacting >= 0 && in->mouse_right_down && !g->resize_win && !make_tool_active) {
        int dx = in->mouse_x - g->last_mouse_x;
        int dy = in->mouse_y - g->last_mouse_y;
        
        if (g->views[g->view_right_interacting].type == CAD_VIEW_3D) {
            /* Pan 3D view up/down relative to current angle */
            CadView_Pan3DVertical(&g->views[g->view_right_interacting], -dy * 0.5);
            /* Also pan left/right in 3D view */
            CadView* view = &g->views[g->view_right_interacting];
            double rx = view->rot_x * M_PI / 180.0;
            double ry = view->rot_y * M_PI / 180.0;
            /* Calculate right vector in world space */
            double right_x = cos(ry);
            double right_y = 0.0;
            double right_z = sin(ry);
            /* Apply panning along the right vector */
            double pan_scale = 1.0 / view->zoom;
            view->pan_x += right_x * dx * pan_scale;
            view->pan_y += right_y * dx * pan_scale;
        } else {
            /* Pan other views (Top, Front, Right) - standard pan */
            CadView_Pan(&g->views[g->view_right_interacting], dx, -dy);
        }
        
        g->last_mouse_x = in->mouse_x;
        g->last_mouse_y = in->mouse_y;
    }
    
    /* Handle right mouse button release */
    if (in->mouse_right_released) {
        /* If make tool is active, don't reset view interaction (let make tool handle it) */
        if (!make_tool_active) {
            g->view_right_interacting = -1;
        }
    }
    
    
    /* Check for view content area clicks (not title bar) - includes right-click for make tool */
    if ((in->mouse_pressed || (make_tool_active && in->mouse_right_pressed)) && !g->drag_win && !g->resize_win && g->view_interacting < 0 && g->view_right_interacting < 0) {
        for (int i = 0; i < 4; i++) {
            Rect vr = g->view[i].r;
            Rect content = (Rect){ vr.x + 6, vr.y + 26, vr.w - 12, vr.h - 32 };
            Rect titlebar = (Rect){ vr.x, vr.y, vr.w, 20 };
            
            /* Check if click is in content area (not title bar) */
            if (pt_in_rect(in->mouse_x, in->mouse_y, content) && 
                !pt_in_rect(in->mouse_x, in->mouse_y, titlebar)) {
                
                /* Check if point select tool is active (tool 0) or make tool (tool 3) */
                if ((g->selected_tool == 0 || g->selected_tool == 3) && g->cad->editMode == CAD_MODE_SELECT_POINT) {
                    /* Point selection mode - find and select/deselect point */
                    int viewport_x = content.x;
                    int viewport_y = content.y;
                    int viewport_w = content.w;
                    int viewport_h = content.h;
                    
                    if (g->selected_tool == 3) {
                        /* Make tool - left click adds points, right click selects final point and creates face */
                        if (in->mouse_pressed) {
                            /* Left click - add point to selection */
                            int16_t point_to_select = CadView_FindNearestPoint(
                                &g->views[i], g->cad,
                                in->mouse_x, in->mouse_y,
                                viewport_x, viewport_y,
                                viewport_w, viewport_h,
                                10 /* 10 pixel screen threshold */
                            );
                            
                            if (point_to_select >= 0) {
                                /* Make tool - allow selecting up to 11 points (12th will be right-clicked) */
                                if (g->cad->selection.pointCount < 11) {
                                    /* Only select if not already selected */
                                    if (!CadCore_IsPointSelected(g->cad, point_to_select)) {
                                        CadCore_SelectPoint(g->cad, point_to_select);
                                        fprintf(stdout, "Selected point %d for face creation (%d/11, right-click final point)\n", 
                                                point_to_select, g->cad->selection.pointCount);
                                    } else {
                                        fprintf(stdout, "Point %d already selected\n", point_to_select);
                                    }
                                } else {
                                    fprintf(stdout, "Maximum 11 points reached. Right-click a point to finalize face.\n");
                                }
                            }
                        } else if (in->mouse_right_pressed) {
                            /* Right click - select final point and create face */
                            int16_t final_point = CadView_FindNearestPoint(
                                &g->views[i], g->cad,
                                in->mouse_x, in->mouse_y,
                                viewport_x, viewport_y,
                                viewport_w, viewport_h,
                                10 /* 10 pixel screen threshold */
                            );
                            
                            if (final_point >= 0) {
                                /* Add final point to selection if not already selected */
                                if (!CadCore_IsPointSelected(g->cad, final_point)) {
                                    if (g->cad->selection.pointCount < 12) {
                                        CadCore_SelectPoint(g->cad, final_point);
                                    }
                                }
                                
                                int point_count = g->cad->selection.pointCount;
                                
                                if (point_count < 2) {
                                    fprintf(stderr, "Need at least 2 points to create a face\n");
                                    CadCore_ClearSelection(g->cad);
                                } else if (point_count > 12) {
                                    fprintf(stderr, "Maximum 12 points allowed per face\n");
                                    CadCore_ClearSelection(g->cad);
                                } else {
                                    /* Get all selected points */
                                    int16_t selected_points[12];
                                    int valid_count = 0;
                                    for (int j = 0; j < point_count && j < 12; j++) {
                                        int16_t pt_idx = g->cad->selection.selectedPoints[j];
                                        if (pt_idx >= 0 && CadCore_IsPointValid(g->cad, pt_idx)) {
                                            selected_points[valid_count++] = pt_idx;
                                        }
                                    }
                                    
                                    if (valid_count < 2) {
                                        fprintf(stderr, "Need at least 2 valid points to create a face\n");
                                        CadCore_ClearSelection(g->cad);
                                    } else {
                                        int16_t p1 = selected_points[0];
                                        
                                        /* Check if a polygon with these exact points already exists */
                                        int polygon_exists = 0;
                                        for (int poly_i = 0; poly_i < g->cad->data.polygonCount; poly_i++) {
                                            CadPolygon* existing_poly = CadCore_GetPolygon(g->cad, poly_i);
                                            if (!existing_poly || existing_poly->flags == 0) continue;
                                            if (existing_poly->npoints != valid_count) continue;
                                            
                                            /* Traverse the polygon's point chain */
                                            int16_t chain_points[12];
                                            int16_t current = existing_poly->firstPoint;
                                            int count = 0;
                                            int visited_count = 0;
                                            int16_t visited[64];
                                            
                                            while (current >= 0 && current < CAD_MAX_POINTS && count < valid_count && visited_count < 64) {
                                                /* Cycle detection */
                                                int already_visited = 0;
                                                for (int v = 0; v < visited_count; v++) {
                                                    if (visited[v] == current) {
                                                        already_visited = 1;
                                                        break;
                                                    }
                                                }
                                                if (already_visited) break;
                                                visited[visited_count++] = current;
                                                
                                                chain_points[count++] = current;
                                                
                                                CadPoint* curr_pt = CadCore_GetPoint(g->cad, current);
                                                if (!curr_pt || curr_pt->flags == 0) break;
                                                current = curr_pt->nextPoint;
                                            }
                                            
                                            /* Check if this polygon has the same points in the same order */
                                            if (count == valid_count) {
                                                int match = 1;
                                                for (int k = 0; k < valid_count; k++) {
                                                    if (chain_points[k] != selected_points[k]) {
                                                        match = 0;
                                                        break;
                                                    }
                                                }
                                                if (match) {
                                                    polygon_exists = 1;
                                                    break;
                                                }
                                            }
                                        }
                                        
                                        if (polygon_exists) {
                                            fprintf(stderr, "Polygon with these points already exists\n");
                                            CadCore_ClearSelection(g->cad);
                                        } else {
                                            /* Create polygon with first point */
                                            int16_t poly_idx = CadCore_AddPolygon(g->cad, p1, 0, valid_count);
                                            
                                            if (poly_idx != INVALID_INDEX) {
                                                /* Link the points together */
                                                for (int j = 0; j < valid_count; j++) {
                                                    int16_t current_pt = selected_points[j];
                                                    int16_t next_pt = (j < valid_count - 1) ? selected_points[j + 1] : INVALID_INDEX;
                                                    
                                                    CadPoint* pt = CadCore_GetPoint(g->cad, current_pt);
                                                    if (!pt) continue;
                                                    
                                                    /* Check if this point is already used as firstPoint of another polygon */
                                                    int is_firstPoint = 0;
                                                    for (int poly_i = 0; poly_i < g->cad->data.polygonCount; poly_i++) {
                                                        CadPolygon* existing_poly = CadCore_GetPolygon(g->cad, poly_i);
                                                        if (!existing_poly || existing_poly->flags == 0) continue;
                                                        if (existing_poly->firstPoint == current_pt && poly_i != poly_idx) {
                                                            is_firstPoint = 1;
                                                            break;
                                                        }
                                                    }
                                                    
                                                    /* Only set nextPoint if not already a firstPoint, or if nextPoint matches what we want */
                                                    if (!is_firstPoint) {
                                                        if (pt->nextPoint == INVALID_INDEX || pt->nextPoint == next_pt) {
                                                            pt->nextPoint = next_pt;
                                                        }
                                                    } else if (pt->nextPoint == next_pt) {
                                                        /* Already matches, no change needed */
                                                    }
                                                }
                                                
                                                fprintf(stdout, "Created face with %d points (polygon index %d)\n", 
                                                        valid_count, poly_idx);
                                                
                                                /* Clear selection after creating face */
                                                CadCore_ClearSelection(g->cad);
                                            } else {
                                                fprintf(stderr, "Failed to create polygon (no free slots)\n");
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    } else {
                        /* Normal point select tool - find all points at the same location (handles merged points) */
                        int16_t point_indices[64]; /* Max 64 points at same location */
                        int point_count = CadView_FindPointsAtLocation(
                            &g->views[i], g->cad,
                            in->mouse_x, in->mouse_y,
                            viewport_x, viewport_y,
                            viewport_w, viewport_h,
                            10, /* 10 pixel screen threshold */
                            0.01, /* 0.01 unit world threshold for merged points */
                            point_indices, 64
                        );
                        
                        if (point_count > 0) {
                            /* Normal point select tool behavior */
                            /* Check if all found points are already selected */
                            int all_selected = 1;
                            for (int j = 0; j < point_count; j++) {
                                if (!CadCore_IsPointSelected(g->cad, point_indices[j])) {
                                    all_selected = 0;
                                    break;
                                }
                            }
                            
                            if (all_selected) {
                                /* Deselect all points at this location */
                                for (int j = 0; j < point_count; j++) {
                                    CadCore_DeselectPoint(g->cad, point_indices[j]);
                                }
                                fprintf(stdout, "Deselected %d point(s) at location\n", point_count);
                            } else {
                                /* Select all points at this location */
                                for (int j = 0; j < point_count; j++) {
                                    CadCore_SelectPoint(g->cad, point_indices[j]);
                                }
                                fprintf(stdout, "Selected %d point(s) at location\n", point_count);
                            }
                        }
                    }
                } else if (g->selected_tool == 2) {
                    /* Point tool (tool 2) - add a new point at clicked location */
                    /* Convert screen coordinates to viewport-relative coordinates */
                    int vp_x = in->mouse_x - content.x;
                    int vp_y = in->mouse_y - content.y;
                    
                    /* Convert screen coordinates to world coordinates */
                    double world_x, world_y, world_z;
                    CadView_UnprojectPoint(
                        &g->views[i],
                        vp_x, vp_y,
                        content.w, content.h,
                        &world_x, &world_y, &world_z
                    );
                    
                    /* Add the point */
                    int16_t new_point_idx = CadCore_AddPoint(g->cad, world_x, world_y, world_z);
                    if (new_point_idx != INVALID_INDEX) {
                        /* Select the newly added point */
                        CadCore_SelectPoint(g->cad, new_point_idx);
                        fprintf(stdout, "Added point at (%.2f, %.2f, %.2f), index %d\n", 
                                world_x, world_y, world_z, new_point_idx);
                    } else {
                        fprintf(stderr, "Failed to add point (no free slots)\n");
                    }
                } else if (g->selected_tool == 6 && g->cad->selection.pointCount > 0) {
                    /* Point move tool (tool 6) - start moving selected points */
                    g->point_move_active = 1;
                    g->point_move_view = i;
                    g->last_mouse_x = in->mouse_x;
                    g->last_mouse_y = in->mouse_y;
                    fprintf(stdout, "Starting point move (%d points selected)\n", g->cad->selection.pointCount);
                } else {
                    /* Normal view interaction (pan/rotate) */
                    g->view_interacting = i;
                    g->last_mouse_x = in->mouse_x;
                    g->last_mouse_y = in->mouse_y;
                }
                break;
            }
        }
    }
    
    /* Handle mouse wheel zoom in views */
    if (in->wheel_delta != 0 && !g->drag_win) {
        for (int i = 0; i < 4; i++) {
            Rect vr = g->view[i].r;
            Rect content = (Rect){ vr.x + 6, vr.y + 26, vr.w - 12, vr.h - 32 };
            
            /* Check if mouse is over view content area */
            if (pt_in_rect(in->mouse_x, in->mouse_y, content)) {
                /* Apply zoom: positive delta = zoom in, negative = zoom out */
                double zoom_factor = 1.0 + (in->wheel_delta * 0.1); /* 10% per scroll step */
                double new_zoom = g->views[i].zoom * zoom_factor;
                CadView_SetZoom(&g->views[i], new_zoom);
                break; /* Only zoom the first view the mouse is over */
            }
        }
    }

    /* Tool palette button clicks */
    if (in->mouse_pressed && !g->drag_win) {
        Rect tp = g->toolPalette.r;
        Rect inner = (Rect){ tp.x + 6, tp.y + 26, tp.w - 12, tp.h - 32 };
        
        if (pt_in_rect(in->mouse_x, in->mouse_y, inner)) {
            const int cols = 2;
            const int icon_w = 32;
            const int icon_h = 48;
            const int padding = 2;
            const int button_w = icon_w + (padding * 2);
            const int button_h = icon_h + (padding * 2);
            const int col_gap = 2;
            const int row_spacing = 1;
            const int total_cols_w = (button_w * cols) + (col_gap * (cols - 1));
            const int col_start_x = inner.x + (inner.w - total_cols_w) / 2;
            
            /* Check which tool button was clicked */
            for (int i = 0; i < TOOL_COUNT; i++) {
                int col = i % cols;
                int row = i / cols;
                int x = col_start_x + col * (button_w + col_gap);
                int y = inner.y + row * (button_h + row_spacing);
                Rect btn_rect = { x, y, button_w, button_h };
                
                if (pt_in_rect(in->mouse_x, in->mouse_y, btn_rect)) {
                    g->selected_tool = (g->selected_tool == i) ? -1 : i; /* Toggle selection */
                    
                    /* Set edit mode based on selected tool */
                    if (g->selected_tool == 0) {
                        /* Point select tool */
                        CadCore_SetEditMode(g->cad, CAD_MODE_SELECT_POINT);
                        fprintf(stdout, "Point select tool activated\n");
                    } else if (g->selected_tool == 2) {
                        /* Point tool (add point) */
                        CadCore_SetEditMode(g->cad, CAD_MODE_EDIT_POINT);
                        fprintf(stdout, "Point tool activated\n");
                    } else if (g->selected_tool == 3) {
                        /* Make tool - clear selection and allow selecting 2-12 points */
                        CadCore_ClearSelection(g->cad);
                        CadCore_SetEditMode(g->cad, CAD_MODE_SELECT_POINT);
                        fprintf(stdout, "Make tool activated - left-click to add points, right-click to finalize face (2-12 points)\n");
                    } else if (g->selected_tool == 6) {
                        /* Point move tool */
                        CadCore_SetEditMode(g->cad, CAD_MODE_EDIT_POINT);
                        fprintf(stdout, "Point move tool activated\n");
                    } else if (g->selected_tool == -1) {
                        /* No tool selected - keep current mode */
                    }
                    /* Add more tool handlers here as needed */
                    
                    break;
                }
            }
        }
    }

    /* Menu open/close - check menu bar buttons first */
    int menu_bar_clicked = 0;
    if (in->mouse_pressed && in->mouse_y < MenuBarHeight()) {
        int x = 8;
        for (int i = 0; i < g->menu_count; i++) {
            int w = g->font ? (font_measure(g->font, g->menus[i]) + 16) : ((int)strlen(g->menus[i]) * 8 + 16);
            Rect r = { x, 0, w, MenuBarHeight() };
            if (pt_in_rect(in->mouse_x, in->mouse_y, r)) {
                g->menu_open = (g->menu_open == i) ? -1 : i;
                g->menu_hover_item = -1;
                menu_bar_clicked = 1;
                break;
            }
            x += w;
        }
    }
    
    /* Dropdown hover + click - only process if menu bar wasn't clicked */
    if (g->menu_open >= 0 && !menu_bar_clicked) {
        const char* const* items = menu_items_for_index(g->menu_open);
        if (items && items[0]) {
            /* Calculate x position to match menu bar item position exactly */
            /* Must match the calculation used in menu bar drawing (line 696-697) */
            int x = 8;
            for (int i = 0; i < g->menu_open; i++) {
                if (g->font) {
                    x += font_measure(g->font, g->menus[i]) + 16;
                } else {
                    x += (int)strlen(g->menus[i]) * 8 + 16;
                }
            }

            /* Skip first item (header), count remaining items */
            int maxW = 0;
            int count = 0;
            for (const char* const* it = items + 1; *it; it++) {
                count++;
                const char* disp = menu_display_text(*it);
                if (disp[0] == '-' && disp[1] == '\0') continue;
                int tw = g->font ? font_measure(g->font, disp) : (int)strlen(disp) * 8;
                if (tw > maxW) maxW = tw;
            }
            int dropW = maxW + 24;
            int itemH = 20;
            Rect drop = { x, MenuBarHeight(), dropW, count * itemH };

            /* Update hover state */
            if (pt_in_rect(in->mouse_x, in->mouse_y, drop)) {
                int idx = (in->mouse_y - drop.y) / itemH;
                if (idx >= 0 && idx < count) {
                    /* Map to actual item index (skip header, so +1) */
                    int actual_idx = idx + 1;
                    g->menu_hover_item = idx;
                    
                    /* Handle click on dropdown item */
                    if (in->mouse_pressed && items[actual_idx]) {
                        const char* raw = items[actual_idx];
                        const char* disp = menu_display_text(raw);
                        if (!(disp[0] == '-' && disp[1] == '\0')) {
                            /* Handle menu action */
                            handle_menu_action(g, g->menu_open, actual_idx);
                            g->menu_open = -1;
                            g->menu_hover_item = -1;
                        }
                    }
                } else {
                    g->menu_hover_item = -1;
                }
            } else {
                g->menu_hover_item = -1;
                /* Click outside dropdown (but not on menu bar) closes menu */
                if (in->mouse_pressed) {
                    g->menu_open = -1;
                    g->menu_hover_item = -1;
                }
            }
        }
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

    /* Windows chrome - view windows only (they contain CAD models, so drawn before CAD) */
    draw_window_chrome(g, &g->toolPalette, win_h, 1.0f, 1.0f);
    for (int i = 0; i < 4; i++) draw_window_chrome(g, &g->view[i], win_h, 1.0f, 1.0f);
    /* Note: coordBox and animationWindow chrome drawn after CAD views so they appear on top */

    /* Tool palette contents - draw tool icons in 2 columns */
    Rect tp = g->toolPalette.r;
    Rect inner = (Rect){ tp.x + 6, tp.y + 26, tp.w - 12, tp.h - 32 };
    RG_Color btn = { 245,245,245,255 };
    RG_Color edge = { 120,120,120,255 };
    
    const int cols = 2;
    const int rows = (TOOL_COUNT + cols - 1) / cols; /* Round up */
    
    /* Use original icon size (32x48) - buttons sized to fit icons */
    const int icon_w = 32;
    const int icon_h = 48;
    const int padding = 2; /* Small padding around icon */
    const int button_w = icon_w + (padding * 2);
    const int button_h = icon_h + (padding * 2);
    
    const int col_gap = 2; /* Gap between columns */
    const int row_spacing = 1; /* Spacing between rows */
    
    /* Center the columns if they don't fill the full width */
    const int total_cols_w = (button_w * cols) + (col_gap * (cols - 1));
    const int col_start_x = inner.x + (inner.w - total_cols_w) / 2;
    
    for (int i = 0; i < TOOL_COUNT; i++) {
        int col = i % cols;
        int row = i / cols;
        
        int x = col_start_x + col * (button_w + col_gap);
        int y = inner.y + row * (button_h + row_spacing);
        
        /* Draw button background */
        rg_fill_rect(x, y, button_w, button_h, btn);
        rg_stroke_rect(x, y, button_w, button_h, edge);
        
        /* Draw icon at original size, centered in button */
        if (g->tool_icons[i]) {
            int icon_x = x + padding;
            int icon_y = y + padding;
            if (g->selected_tool == i) {
                /* Selected: draw normally */
                rg_draw_texture(g->tool_icons[i], icon_x, icon_y, icon_w, icon_h);
            } else {
                /* Not selected: draw with inverted colors */
                rg_draw_texture_inverted(g->tool_icons[i], icon_x, icon_y, icon_w, icon_h);
            }
        }
    }

    /* Scrollbars (GUI elements, not CAD) */
    for (int i = 0; i < 4; i++) {
        Rect vr = g->view[i].r;
        Rect content = (Rect){ vr.x + 6, vr.y + 26, vr.w - 12, vr.h - 32 };
        draw_scrollbars_placeholder(content);
    }

    /* Note: Coordinates box and Animation window content drawn after CAD views */
}

/* ============================================================================
   DROPDOWN MENU RENDERING (must be drawn last, on top of everything)
   ============================================================================ */

static void gui_draw_dropdown(GuiState* g, int win_w, int win_h) {
    if (!g) return;
    
    /* Viewport and projection already set in gui_draw */
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    
    /* Dropdown menu - drawn last so it appears on top */
    if (g->menu_open >= 0 && g->menu_open < g->menu_count) {
        const char* const* items = menu_items_for_index(g->menu_open);
        if (items && items[0]) {
            /* Calculate x position to match menu bar item position exactly */
            int x = 8;
            for (int i = 0; i < g->menu_open; i++) {
                if (g->font) {
                    x += font_measure(g->font, g->menus[i]) + 16;
                } else {
                    x += (int)strlen(g->menus[i]) * 8 + 16;
                }
            }

            /* Skip first item (header), count remaining items */
            int maxW = 0;
            int count = 0;
            for (const char* const* it = items + 1; *it; it++) {
                count++;
                const char* disp = menu_display_text(*it);
                if (disp[0] == '-' && disp[1] == '\0') continue;
                int tw = g->font ? font_measure(g->font, disp) : (int)strlen(disp) * 8;
                if (tw > maxW) maxW = tw;
            }

            int w = maxW + 24;
            int y0 = MenuBarHeight();
            int itemH = 20;
            int h = count * itemH;

            rg_fill_rect(x, y0, w, h, (RG_Color){245,245,245,255});
            rg_stroke_rect(x, y0, w, h, (RG_Color){0,0,0,255});

            /* Draw items (skip header at index 0) */
            for (int i = 0; i < count; i++) {
                const char* raw = items[i + 1]; /* Skip header */
                const char* disp = menu_display_text(raw);
                int rowY = y0 + i * itemH;

                if (disp[0] == '-' && disp[1] == '\0') {
                    rg_line(x + 6, rowY + itemH / 2, x + w - 6, rowY + itemH / 2, (RG_Color){120,120,120,255});
                    continue;
                }

                if (i == g->menu_hover_item) {
                    rg_fill_rect(x + 1, rowY, w - 2, itemH, (RG_Color){210,210,210,255});
                }

                if (g->font) {
                    font_draw(g->font, x + 8, rowY + 3, disp, 0);
                }
            }
        }
    }
}

/* ============================================================================
   CAD MODEL RENDERING (3D with depth testing)
   ============================================================================ */

static void gui_draw_view_info_bar(GuiState* g, int view_idx, const GuiInput* in, int win_w, int win_h, int fb_w, int fb_h) {
    if (!g || !g->font || !in || view_idx < 0 || view_idx >= 4) return;
    
    Rect vr = g->view[view_idx].r;
    Rect content = (Rect){ vr.x + 6, vr.y + 26, vr.w - 12, vr.h - 32 };
    
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
    
    /* Draw info bar at bottom of view window (overlapping content area) */
    int info_bar_y = vr.y + vr.h - 20; /* 20 pixels high, at bottom of window */
    RG_Color info_bg = { 240, 240, 240, 255 };
    RG_Color info_border = { 180, 180, 180, 255 };
    rg_fill_rect(content.x, info_bar_y, content.w, 20, info_bg);
    rg_stroke_rect(content.x, info_bar_y, content.w, 20, info_border);
    
    /* Format coordinate string */
    char coord_str[128];
    snprintf(coord_str, sizeof(coord_str), "X:%.2f  Y:%.2f  Z:%.2f", world_x, world_y, world_z);
    
    /* Draw coordinate text */
    font_draw(g->font, content.x + 8, info_bar_y + 4, coord_str, 0);
}

static void gui_draw_cad_views(GuiState* g, int win_w, int win_h, int fb_w, int fb_h, const GuiInput* in) {
    if (!g || !g->cad) return;
    
    /* Calculate scale factors for coordinate conversion */
    float scale_x = (fb_w > 0 && win_w > 0) ? (float)fb_w / (float)win_w : 1.0f;
    float scale_y = (fb_h > 0 && win_h > 0) ? (float)fb_h / (float)win_h : 1.0f;
    
    /* Render CAD data in each view */
    for (int i = 0; i < 4; i++) {
        Rect vr = g->view[i].r;
        Rect content = (Rect){ vr.x + 6, vr.y + 26, vr.w - 12, vr.h - 32 };
        
        /* Scale viewport coordinates to framebuffer */
        int scaled_x = (int)(content.x * scale_x);
        int scaled_y = (int)(content.y * scale_y);
        int scaled_w = (int)(content.w * scale_x);
        int scaled_h = (int)(content.h * scale_y);
        
        /* Enable depth testing for this viewport */
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_TRUE);
        
        /* Clear depth buffer for this viewport only */
        glEnable(GL_SCISSOR_TEST);
        int gl_y = fb_h - (scaled_y + scaled_h);
        if (gl_y < 0) gl_y = 0;
        glScissor(scaled_x, gl_y, scaled_w, scaled_h);
        glClearDepth(1.0);
        glClear(GL_DEPTH_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
        
        /* Render CAD model in this viewport - use scaled coordinates */
        CadView_Render(&g->views[i], g->cad, scaled_x, scaled_y, scaled_w, scaled_h, fb_h);
        
        /* Reset to 2D after CAD rendering - restore main viewport/projection */
        rg_reset_viewport(win_w, win_h, fb_w, fb_h);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        
        /* Draw info bar for this view */
        if (in) {
            gui_draw_view_info_bar(g, i, in, win_w, win_h, fb_w, fb_h);
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
    
    /* Step 3: Draw windows that should appear on top of CAD models */
    draw_window_chrome(g, &g->coordBox, win_h, 1.0f, 1.0f);
    if (g->animationWindow.r.w > 0 && g->animationWindow.r.h > 0) {
        draw_window_chrome(g, &g->animationWindow, win_h, 1.0f, 1.0f);
    }
    
    /* Draw coordinates box content */
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
        font_draw(g->font, cinner.x + 8, cinner.y + 6, coord_str, 0);
    }
    
    /* Draw animation window content */
    if (g->animationWindow.r.w > 0 && g->animationWindow.r.h > 0) {
        Rect ar = g->animationWindow.r;
        Rect ainner = (Rect){ ar.x + 6, ar.y + 26, ar.w - 12, ar.h - 32 };
        rg_fill_rect(ainner.x, ainner.y, ainner.w, ainner.h, (RG_Color){250,250,250,255});
        rg_stroke_rect(ainner.x, ainner.y, ainner.w, ainner.h, (RG_Color){120,120,120,255});
        
        if (g->font) {
            int y = ainner.y + 8;
            int x = ainner.x + 8;
            
            /* Title: Current Frame No X */
            char frame_title[64];
            snprintf(frame_title, sizeof(frame_title), "Current Frame No %d", g->anim_current_frame);
            font_draw(g->font, x, y, frame_title, 0);
            y += 25;
            
            /* Playback controls row - using icons from bitmap.c */
            int icon_spacing = 5;
            int start_x = x;
            
            /* First Frame button (icon 1: topfram_bits, 24x48) */
            if (g->anim_icons[1]) {
                rg_draw_texture_inverted(g->anim_icons[1], start_x, y, 24, 48);
            }
            start_x += 24 + icon_spacing;
            
            /* 10 frames back button (icon 0: beframe_bits, 24x48) */
            if (g->anim_icons[0]) {
                rg_draw_texture_inverted(g->anim_icons[0], start_x, y, 24, 48);
            }
            start_x += 24 + icon_spacing;
            
            /* 1 frame back button (icon 2: beforeframe_bits, 24x48) */
            if (g->anim_icons[2]) {
                rg_draw_texture_inverted(g->anim_icons[2], start_x, y, 24, 48);
            }
            start_x += 24 + icon_spacing;
            
            /* Play/Preview button (icon 3: goframe_bits, 32x48) */
            if (g->anim_icons[3]) {
                RG_Color play_bg = g->anim_playing ? (RG_Color){180,255,180,255} : (RG_Color){220,220,220,255};
                rg_fill_rect(start_x - 2, y - 2, 36, 52, play_bg);
                rg_draw_texture_inverted(g->anim_icons[3], start_x, y, 32, 48);
            }
            start_x += 32 + icon_spacing;
            
            /* 1 frame forward button (icon 4: nextframe_bits, 24x48) */
            if (g->anim_icons[4]) {
                rg_draw_texture_inverted(g->anim_icons[4], start_x, y, 24, 48);
            }
            start_x += 24 + icon_spacing;
            
            /* 10 frames forward button (icon 5: nexframe_bits, 24x48) */
            if (g->anim_icons[5]) {
                rg_draw_texture_inverted(g->anim_icons[5], start_x, y, 24, 48);
            }
            start_x += 24 + icon_spacing;
            
            /* Last Frame button (icon 1: topfram_bits, 24x48) */
            if (g->anim_icons[1]) {
                rg_draw_texture_inverted(g->anim_icons[1], start_x, y, 24, 48);
            }
            
            y += 48 + 15;
            
            /* Right side: Frame counter, Loop button, and Action buttons */
            int right_x = ainner.x + ainner.w - 120;
            int right_y = ainner.y + 8;
            
            /* Frame counter */
            char frame_count[32];
            snprintf(frame_count, sizeof(frame_count), "%d", g->anim_total_frames);
            font_draw(g->font, right_x, right_y, frame_count, 0);
            right_y += 25;
            
            /* Loop button (icon 11: toguru_bits, 48x24) */
            if (g->anim_icons[11]) {
                RG_Color loop_bg = g->anim_loop ? (RG_Color){180,255,180,255} : (RG_Color){220,220,220,255};
                rg_fill_rect(right_x - 2, right_y - 2, 52, 28, loop_bg);
                rg_draw_texture(g->anim_icons[11], right_x, right_y, 48, 24);
            }
            right_y += 30;
            
            /* Action buttons column - using icons */
            int action_icon_y = right_y;
            
            /* Add button (icon 8: plus_bits, 30x30) */
            if (g->anim_icons[8]) {
                rg_fill_rect(right_x - 2, action_icon_y - 2, 34, 34, (RG_Color){220,220,220,255});
                rg_stroke_rect(right_x - 2, action_icon_y - 2, 34, 34, (RG_Color){0,0,0,255});
                rg_draw_texture(g->anim_icons[8], right_x, action_icon_y, 30, 30);
                if (g->font) font_draw(g->font, right_x + 35, action_icon_y + 8, "Add", 0);
            }
            action_icon_y += 35;
            
            /* Delete button (icon 9: minus_bits, 30x30) */
            if (g->anim_icons[9]) {
                rg_fill_rect(right_x - 2, action_icon_y - 2, 34, 34, (RG_Color){220,220,220,255});
                rg_stroke_rect(right_x - 2, action_icon_y - 2, 34, 34, (RG_Color){0,0,0,255});
                rg_draw_texture(g->anim_icons[9], right_x, action_icon_y, 30, 30);
                if (g->font) font_draw(g->font, right_x + 35, action_icon_y + 8, "delete", 0);
            }
            action_icon_y += 35;
            
            /* Copy button (icon 10: copy_bits, 30x30) */
            if (g->anim_icons[10]) {
                rg_fill_rect(right_x - 2, action_icon_y - 2, 34, 34, (RG_Color){220,220,220,255});
                rg_stroke_rect(right_x - 2, action_icon_y - 2, 34, 34, (RG_Color){0,0,0,255});
                rg_draw_texture(g->anim_icons[10], right_x, action_icon_y, 30, 30);
                if (g->font) font_draw(g->font, right_x + 35, action_icon_y + 8, "AllCopy", 0);
            }
            action_icon_y += 35;
            
            if (g->font) {
                font_draw(g->font, right_x, action_icon_y, "AllMove", 0);
                action_icon_y += 20;
                font_draw(g->font, right_x, action_icon_y, "PartCopy", 0);
            }
            
            /* Timeline scrubber at bottom */
            int timeline_y = ainner.y + ainner.h - 30;
            int timeline_h = 20;
            Rect timeline = (Rect){ ainner.x + 8, timeline_y, ainner.w - 16, timeline_h };
            rg_fill_rect(timeline.x, timeline.y, timeline.w, timeline.h, (RG_Color){240,240,240,255});
            rg_stroke_rect(timeline.x, timeline.y, timeline.w, timeline.h, (RG_Color){0,0,0,255});
            
            /* Timeline slider */
            if (g->anim_total_frames > 0) {
                int slider_w = 10;
                int slider_x = timeline.x + (int)((float)timeline.w * (float)g->anim_current_frame / (float)(g->anim_total_frames > 0 ? g->anim_total_frames : 1));
                if (slider_x + slider_w > timeline.x + timeline.w) slider_x = timeline.x + timeline.w - slider_w;
                Rect slider = (Rect){ slider_x, timeline.y + 2, slider_w, timeline_h - 4 };
                rg_fill_rect(slider.x, slider.y, slider.w, slider.h, (RG_Color){100,100,100,255});
            }
            
            /* End button at bottom right */
            if (g->font) {
                Rect btn_end = (Rect){ ainner.x + ainner.w - 60, timeline_y, 50, timeline_h };
                rg_fill_rect(btn_end.x, btn_end.y, btn_end.w, btn_end.h, (RG_Color){220,220,220,255});
                rg_stroke_rect(btn_end.x, btn_end.y, btn_end.w, btn_end.h, (RG_Color){0,0,0,255});
                font_draw(g->font, btn_end.x + 12, btn_end.y + 6, "end", 0);
            }
        }
    }
    
    /* Step 4: Draw dropdown menu last (on top of everything) */
    gui_draw_dropdown(g, win_w, win_h);
    
    /* Final reset to ensure clean state */
    rg_reset_viewport(win_w, win_h, fb_w, fb_h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
}


