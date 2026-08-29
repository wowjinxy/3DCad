#ifndef PALETTE_PANEL_H
#define PALETTE_PANEL_H

/* SDL-independent geometry and hit model for the floating palette editor.
   Drawing, palette data, and command dispatch remain GUI responsibilities;
   this module keeps the two-tab control layout and indexed grids deterministic
   and independently testable. */

#ifdef __cplusplus
extern "C" {
#endif

enum {
    CAD_PALETTE_PANEL_PRIMARY_COLUMNS = 16,
    CAD_PALETTE_PANEL_PRIMARY_ROWS = 16,
    CAD_PALETTE_PANEL_PRIMARY_COUNT = 256,
    CAD_PALETTE_PANEL_SAMPLE_COLUMNS = 16,
    CAD_PALETTE_PANEL_SAMPLE_ROWS = 8,
    CAD_PALETTE_PANEL_SAMPLE_COUNT = 128
};

typedef struct CadPalettePanelRect {
    int x;
    int y;
    int w;
    int h;
} CadPalettePanelRect;

typedef enum CadPalettePanelTab {
    CAD_PALETTE_PANEL_TAB_COL = 0,
    CAD_PALETTE_PANEL_TAB_PAL = 1
} CadPalettePanelTab;

typedef enum CadPalettePanelAction {
    CAD_PALETTE_PANEL_NONE = 0,
    CAD_PALETTE_PANEL_TAB_COL_ACTION,
    CAD_PALETTE_PANEL_TAB_PAL_ACTION,
    CAD_PALETTE_PANEL_NEW,
    CAD_PALETTE_PANEL_OPEN,
    CAD_PALETTE_PANEL_SAVE,
    CAD_PALETTE_PANEL_SAVE_AS,
    CAD_PALETTE_PANEL_PRIMARY_GRID,
    CAD_PALETTE_PANEL_COL_RED_MINUS,
    CAD_PALETTE_PANEL_COL_RED_PLUS,
    CAD_PALETTE_PANEL_COL_GREEN_MINUS,
    CAD_PALETTE_PANEL_COL_GREEN_PLUS,
    CAD_PALETTE_PANEL_COL_BLUE_MINUS,
    CAD_PALETTE_PANEL_COL_BLUE_PLUS,
    CAD_PALETTE_PANEL_COL_APPLY_SELECTED,
    CAD_PALETTE_PANEL_PAL_TYPE_MINUS,
    CAD_PALETTE_PANEL_PAL_TYPE_PLUS,
    CAD_PALETTE_PANEL_PAL_PALETTE_NUMBER_MINUS,
    CAD_PALETTE_PANEL_PAL_PALETTE_NUMBER_PLUS,
    CAD_PALETTE_PANEL_PAL_COLOR_COUNT_MINUS,
    CAD_PALETTE_PANEL_PAL_COLOR_COUNT_PLUS,
    CAD_PALETTE_PANEL_PAL_SAMPLE_MINUS,
    CAD_PALETTE_PANEL_PAL_SAMPLE_PLUS,
    CAD_PALETTE_PANEL_PAL_INDEX_MINUS,
    CAD_PALETTE_PANEL_PAL_INDEX_PLUS,
    CAD_PALETTE_PANEL_SAMPLE_GRID,
    CAD_PALETTE_PANEL_PAL_APPLY_SELECTED
} CadPalettePanelAction;

typedef struct CadPalettePanelLayout {
    CadPalettePanelRect panel;
    CadPalettePanelRect inner;

    CadPalettePanelRect new_palette;
    CadPalettePanelRect open_palette;
    CadPalettePanelRect save_palette;
    CadPalettePanelRect save_as_palette;
    CadPalettePanelRect tab_col;
    CadPalettePanelRect tab_pal;
    CadPalettePanelRect primary_grid;

    CadPalettePanelRect col_red_minus;
    CadPalettePanelRect col_red_value;
    CadPalettePanelRect col_red_plus;
    CadPalettePanelRect col_green_minus;
    CadPalettePanelRect col_green_value;
    CadPalettePanelRect col_green_plus;
    CadPalettePanelRect col_blue_minus;
    CadPalettePanelRect col_blue_value;
    CadPalettePanelRect col_blue_plus;
    CadPalettePanelRect col_apply_selected;

    CadPalettePanelRect pal_type_minus;
    CadPalettePanelRect pal_type_value;
    CadPalettePanelRect pal_type_plus;
    CadPalettePanelRect pal_palette_number_minus;
    CadPalettePanelRect pal_palette_number_value;
    CadPalettePanelRect pal_palette_number_plus;
    CadPalettePanelRect pal_color_count_minus;
    CadPalettePanelRect pal_color_count_value;
    CadPalettePanelRect pal_color_count_plus;
    CadPalettePanelRect pal_sample_minus;
    CadPalettePanelRect pal_sample_value;
    CadPalettePanelRect pal_sample_plus;
    CadPalettePanelRect pal_index_minus;
    CadPalettePanelRect pal_index_value;
    CadPalettePanelRect pal_index_plus;
    CadPalettePanelRect sample_grid;
    CadPalettePanelRect pal_apply_selected;

    int usable;
} CadPalettePanelLayout;

typedef struct CadPalettePanelHit {
    CadPalettePanelAction action;
    int primaryIndex;
    int sampleIndex;
} CadPalettePanelHit;

void CadPalettePanel_ComputeLayout(CadPalettePanelRect panel,
                                   CadPalettePanelLayout* output);

/* Both mappings use half-open grid bounds and row-major indices.  On failure,
   a supplied output index is set to -1. */
int CadPalettePanel_MapPrimary(const CadPalettePanelLayout* layout,
                               int pointerX, int pointerY, int* primaryIndex);
int CadPalettePanel_MapSample(const CadPalettePanelLayout* layout,
                              int pointerX, int pointerY, int* sampleIndex);

CadPalettePanelHit CadPalettePanel_HitTest(
    const CadPalettePanelLayout* layout, CadPalettePanelTab activeTab,
    int pointerX, int pointerY);

#ifdef __cplusplus
}
#endif

#endif
