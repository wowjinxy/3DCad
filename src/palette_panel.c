#include "palette_panel.h"

#include <stdint.h>
#include <string.h>

static CadPalettePanelRect make_rect(int x, int y, int w, int h) {
    CadPalettePanelRect result = {x, y, w > 0 ? w : 0, h > 0 ? h : 0};
    return result;
}

static int contains(CadPalettePanelRect rect, int x, int y) {
    int64_t right;
    int64_t bottom;
    if (rect.w <= 0 || rect.h <= 0) return 0;
    right = (int64_t)rect.x + rect.w;
    bottom = (int64_t)rect.y + rect.h;
    return (int64_t)x >= rect.x && (int64_t)y >= rect.y &&
           (int64_t)x < right && (int64_t)y < bottom;
}

static int fits(CadPalettePanelRect child, CadPalettePanelRect parent) {
    int64_t child_right;
    int64_t child_bottom;
    int64_t parent_right;
    int64_t parent_bottom;
    if (child.w <= 0 || child.h <= 0 || parent.w <= 0 || parent.h <= 0)
        return 0;
    child_right = (int64_t)child.x + child.w;
    child_bottom = (int64_t)child.y + child.h;
    parent_right = (int64_t)parent.x + parent.w;
    parent_bottom = (int64_t)parent.y + parent.h;
    return child.x >= parent.x && child.y >= parent.y &&
           child_right <= parent_right && child_bottom <= parent_bottom;
}

static void clip_control(CadPalettePanelRect* control,
                         CadPalettePanelRect inner) {
    if (control && !fits(*control, inner)) memset(control, 0, sizeof(*control));
}

static void make_adjuster(CadPalettePanelRect* minus,
                          CadPalettePanelRect* value,
                          CadPalettePanelRect* plus,
                          int x, int y) {
    if (minus) *minus = make_rect(x, y, 28, 24);
    if (value) *value = make_rect(x + 32, y, 104, 24);
    if (plus) *plus = make_rect(x + 140, y, 28, 24);
}

void CadPalettePanel_ComputeLayout(CadPalettePanelRect panel,
                                   CadPalettePanelLayout* output) {
    int toolbar_x;
    int toolbar_y;
    int content_y;
    int inspector_x;
    if (!output) return;
    memset(output, 0, sizeof(*output));
    output->panel = panel;
    output->inner = make_rect(panel.x + 6, panel.y + 26,
                              panel.w - 12, panel.h - 32);
    output->usable = output->inner.w >= 260 && output->inner.h >= 160;
    if (!output->usable) return;

    toolbar_x = output->inner.x + 4;
    toolbar_y = output->inner.y + 3;
    output->new_palette = make_rect(toolbar_x, toolbar_y, 52, 22);
    output->open_palette = make_rect(toolbar_x + 56, toolbar_y, 58, 22);
    output->save_palette = make_rect(toolbar_x + 118, toolbar_y, 52, 22);
    output->save_as_palette = make_rect(toolbar_x + 174, toolbar_y, 82, 22);

    output->tab_col = make_rect(output->inner.x + 4,
                                output->inner.y + 30, 72, 24);
    output->tab_pal = make_rect(output->inner.x + 80,
                                output->inner.y + 30, 72, 24);

    content_y = output->inner.y + 60;
    output->primary_grid = make_rect(output->inner.x + 4, content_y,
                                     320, 320);
    inspector_x = output->inner.x + 340;

    make_adjuster(&output->col_red_minus, &output->col_red_value,
                  &output->col_red_plus, inspector_x, content_y);
    make_adjuster(&output->col_green_minus, &output->col_green_value,
                  &output->col_green_plus, inspector_x, content_y + 30);
    make_adjuster(&output->col_blue_minus, &output->col_blue_value,
                  &output->col_blue_plus, inspector_x, content_y + 60);
    output->col_apply_selected = make_rect(inspector_x, content_y + 102,
                                           168, 26);

    make_adjuster(&output->pal_type_minus, &output->pal_type_value,
                  &output->pal_type_plus, inspector_x, content_y);
    make_adjuster(&output->pal_palette_number_minus,
                  &output->pal_palette_number_value,
                  &output->pal_palette_number_plus,
                  inspector_x, content_y + 30);
    make_adjuster(&output->pal_color_count_minus,
                  &output->pal_color_count_value,
                  &output->pal_color_count_plus,
                  inspector_x, content_y + 60);
    make_adjuster(&output->pal_sample_minus, &output->pal_sample_value,
                  &output->pal_sample_plus, inspector_x, content_y + 90);
    make_adjuster(&output->pal_index_minus, &output->pal_index_value,
                  &output->pal_index_plus, inspector_x, content_y + 120);
    output->sample_grid = make_rect(inspector_x, content_y + 154,
                                    352, 176);
    output->pal_apply_selected = make_rect(inspector_x, content_y + 336,
                                           168, 26);

#define CLIP(field) clip_control(&output->field, output->inner)
    CLIP(new_palette); CLIP(open_palette); CLIP(save_palette);
    CLIP(save_as_palette); CLIP(tab_col); CLIP(tab_pal); CLIP(primary_grid);
    CLIP(col_red_minus); CLIP(col_red_value); CLIP(col_red_plus);
    CLIP(col_green_minus); CLIP(col_green_value); CLIP(col_green_plus);
    CLIP(col_blue_minus); CLIP(col_blue_value); CLIP(col_blue_plus);
    CLIP(col_apply_selected);
    CLIP(pal_type_minus); CLIP(pal_type_value); CLIP(pal_type_plus);
    CLIP(pal_palette_number_minus); CLIP(pal_palette_number_value);
    CLIP(pal_palette_number_plus); CLIP(pal_color_count_minus);
    CLIP(pal_color_count_value); CLIP(pal_color_count_plus);
    CLIP(pal_sample_minus); CLIP(pal_sample_value); CLIP(pal_sample_plus);
    CLIP(pal_index_minus); CLIP(pal_index_value); CLIP(pal_index_plus);
    CLIP(sample_grid); CLIP(pal_apply_selected);
#undef CLIP
}

static int map_grid(CadPalettePanelRect grid, int columns, int rows,
                    int pointer_x, int pointer_y, int* index) {
    int column;
    int row;
    if (index) *index = -1;
    if (columns <= 0 || rows <= 0 ||
        !contains(grid, pointer_x, pointer_y)) return 0;
    column = (int)(((int64_t)pointer_x - grid.x) * columns / grid.w);
    row = (int)(((int64_t)pointer_y - grid.y) * rows / grid.h);
    if (column < 0 || column >= columns || row < 0 || row >= rows) return 0;
    if (index) *index = row * columns + column;
    return 1;
}

int CadPalettePanel_MapPrimary(const CadPalettePanelLayout* layout,
                               int pointerX, int pointerY, int* primaryIndex) {
    if (primaryIndex) *primaryIndex = -1;
    if (!layout || !layout->usable) return 0;
    return map_grid(layout->primary_grid,
                    CAD_PALETTE_PANEL_PRIMARY_COLUMNS,
                    CAD_PALETTE_PANEL_PRIMARY_ROWS,
                    pointerX, pointerY, primaryIndex);
}

int CadPalettePanel_MapSample(const CadPalettePanelLayout* layout,
                              int pointerX, int pointerY, int* sampleIndex) {
    if (sampleIndex) *sampleIndex = -1;
    if (!layout || !layout->usable) return 0;
    return map_grid(layout->sample_grid,
                    CAD_PALETTE_PANEL_SAMPLE_COLUMNS,
                    CAD_PALETTE_PANEL_SAMPLE_ROWS,
                    pointerX, pointerY, sampleIndex);
}

CadPalettePanelHit CadPalettePanel_HitTest(
    const CadPalettePanelLayout* layout, CadPalettePanelTab activeTab,
    int pointerX, int pointerY) {
    CadPalettePanelHit hit;
    hit.action = CAD_PALETTE_PANEL_NONE;
    hit.primaryIndex = -1;
    hit.sampleIndex = -1;
    if (!layout || !layout->usable ||
        !contains(layout->panel, pointerX, pointerY)) return hit;

#define HIT(field, value)                                                     \
    if (contains(layout->field, pointerX, pointerY)) {                        \
        hit.action = (value);                                                 \
        return hit;                                                           \
    }
    HIT(new_palette, CAD_PALETTE_PANEL_NEW);
    HIT(open_palette, CAD_PALETTE_PANEL_OPEN);
    HIT(save_palette, CAD_PALETTE_PANEL_SAVE);
    HIT(save_as_palette, CAD_PALETTE_PANEL_SAVE_AS);
    HIT(tab_col, CAD_PALETTE_PANEL_TAB_COL_ACTION);
    HIT(tab_pal, CAD_PALETTE_PANEL_TAB_PAL_ACTION);
#undef HIT

    if (CadPalettePanel_MapPrimary(layout, pointerX, pointerY,
                                   &hit.primaryIndex)) {
        hit.action = CAD_PALETTE_PANEL_PRIMARY_GRID;
        return hit;
    }

#define HIT(field, value)                                                     \
    if (contains(layout->field, pointerX, pointerY)) {                        \
        hit.action = (value);                                                 \
        return hit;                                                           \
    }
    if (activeTab == CAD_PALETTE_PANEL_TAB_COL) {
        HIT(col_red_minus, CAD_PALETTE_PANEL_COL_RED_MINUS);
        HIT(col_red_plus, CAD_PALETTE_PANEL_COL_RED_PLUS);
        HIT(col_green_minus, CAD_PALETTE_PANEL_COL_GREEN_MINUS);
        HIT(col_green_plus, CAD_PALETTE_PANEL_COL_GREEN_PLUS);
        HIT(col_blue_minus, CAD_PALETTE_PANEL_COL_BLUE_MINUS);
        HIT(col_blue_plus, CAD_PALETTE_PANEL_COL_BLUE_PLUS);
        HIT(col_apply_selected, CAD_PALETTE_PANEL_COL_APPLY_SELECTED);
    } else if (activeTab == CAD_PALETTE_PANEL_TAB_PAL) {
        HIT(pal_type_minus, CAD_PALETTE_PANEL_PAL_TYPE_MINUS);
        HIT(pal_type_plus, CAD_PALETTE_PANEL_PAL_TYPE_PLUS);
        HIT(pal_palette_number_minus,
            CAD_PALETTE_PANEL_PAL_PALETTE_NUMBER_MINUS);
        HIT(pal_palette_number_plus,
            CAD_PALETTE_PANEL_PAL_PALETTE_NUMBER_PLUS);
        HIT(pal_color_count_minus,
            CAD_PALETTE_PANEL_PAL_COLOR_COUNT_MINUS);
        HIT(pal_color_count_plus,
            CAD_PALETTE_PANEL_PAL_COLOR_COUNT_PLUS);
        HIT(pal_sample_minus, CAD_PALETTE_PANEL_PAL_SAMPLE_MINUS);
        HIT(pal_sample_plus, CAD_PALETTE_PANEL_PAL_SAMPLE_PLUS);
        HIT(pal_index_minus, CAD_PALETTE_PANEL_PAL_INDEX_MINUS);
        HIT(pal_index_plus, CAD_PALETTE_PANEL_PAL_INDEX_PLUS);
        HIT(pal_apply_selected, CAD_PALETTE_PANEL_PAL_APPLY_SELECTED);
        if (CadPalettePanel_MapSample(layout, pointerX, pointerY,
                                      &hit.sampleIndex)) {
            hit.action = CAD_PALETTE_PANEL_SAMPLE_GRID;
            return hit;
        }
    }
#undef HIT
    return hit;
}
