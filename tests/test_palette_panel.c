#include "palette_panel.h"

#include <stdio.h>

static int failures;

#define CHECK(expression)                                                     \
    do {                                                                      \
        if (!(expression)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,        \
                    __LINE__, #expression);                                   \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

static CadPalettePanelHit hit_center(const CadPalettePanelLayout* layout,
                                     CadPalettePanelTab tab,
                                     CadPalettePanelRect rect) {
    return CadPalettePanel_HitTest(layout, tab,
                                   rect.x + rect.w / 2,
                                   rect.y + rect.h / 2);
}

static void check_action(const CadPalettePanelLayout* layout,
                         CadPalettePanelTab tab, CadPalettePanelRect rect,
                         CadPalettePanelAction expected) {
    CadPalettePanelHit hit = hit_center(layout, tab, rect);
    CHECK(rect.w > 0 && rect.h > 0);
    CHECK(hit.action == expected);
}

static void test_reference_layout_and_common_hits(void) {
    CadPalettePanelLayout layout;
    CadPalettePanel_ComputeLayout(
        (CadPalettePanelRect){100, 50, 720, 520}, &layout);
    CHECK(layout.usable);
    CHECK(layout.inner.x == 106 && layout.inner.y == 76);
    CHECK(layout.inner.w == 708 && layout.inner.h == 488);
    CHECK(layout.primary_grid.w == 320 && layout.primary_grid.h == 320);
    CHECK(layout.sample_grid.w == 352 && layout.sample_grid.h == 176);

    check_action(&layout, CAD_PALETTE_PANEL_TAB_COL, layout.new_palette,
                 CAD_PALETTE_PANEL_NEW);
    check_action(&layout, CAD_PALETTE_PANEL_TAB_COL, layout.open_palette,
                 CAD_PALETTE_PANEL_OPEN);
    check_action(&layout, CAD_PALETTE_PANEL_TAB_COL, layout.save_palette,
                 CAD_PALETTE_PANEL_SAVE);
    check_action(&layout, CAD_PALETTE_PANEL_TAB_COL, layout.save_as_palette,
                 CAD_PALETTE_PANEL_SAVE_AS);
    check_action(&layout, CAD_PALETTE_PANEL_TAB_COL, layout.tab_col,
                 CAD_PALETTE_PANEL_TAB_COL_ACTION);
    check_action(&layout, CAD_PALETTE_PANEL_TAB_COL, layout.tab_pal,
                 CAD_PALETTE_PANEL_TAB_PAL_ACTION);
}

static void test_col_hits_and_tab_occlusion(void) {
    CadPalettePanelLayout layout;
    CadPalettePanelHit hit;
    CadPalettePanel_ComputeLayout(
        (CadPalettePanelRect){0, 0, 720, 520}, &layout);
#define CHECK_COL(field, action)                                              \
    check_action(&layout, CAD_PALETTE_PANEL_TAB_COL, layout.field, (action))
    CHECK_COL(col_red_minus, CAD_PALETTE_PANEL_COL_RED_MINUS);
    CHECK_COL(col_red_plus, CAD_PALETTE_PANEL_COL_RED_PLUS);
    CHECK_COL(col_green_minus, CAD_PALETTE_PANEL_COL_GREEN_MINUS);
    CHECK_COL(col_green_plus, CAD_PALETTE_PANEL_COL_GREEN_PLUS);
    CHECK_COL(col_blue_minus, CAD_PALETTE_PANEL_COL_BLUE_MINUS);
    CHECK_COL(col_blue_plus, CAD_PALETTE_PANEL_COL_BLUE_PLUS);
    CHECK_COL(col_apply_selected, CAD_PALETTE_PANEL_COL_APPLY_SELECTED);
#undef CHECK_COL

    hit = hit_center(&layout, CAD_PALETTE_PANEL_TAB_PAL,
                     layout.col_apply_selected);
    CHECK(hit.action == CAD_PALETTE_PANEL_NONE);
    hit = hit_center(&layout, (CadPalettePanelTab)99, layout.col_red_minus);
    CHECK(hit.action == CAD_PALETTE_PANEL_NONE);
}

static void test_pal_hits_and_tab_occlusion(void) {
    CadPalettePanelLayout layout;
    CadPalettePanelHit hit;
    CadPalettePanel_ComputeLayout(
        (CadPalettePanelRect){0, 0, 720, 520}, &layout);
#define CHECK_PAL(field, action)                                              \
    check_action(&layout, CAD_PALETTE_PANEL_TAB_PAL, layout.field, (action))
    CHECK_PAL(pal_type_minus, CAD_PALETTE_PANEL_PAL_TYPE_MINUS);
    CHECK_PAL(pal_type_plus, CAD_PALETTE_PANEL_PAL_TYPE_PLUS);
    CHECK_PAL(pal_palette_number_minus,
              CAD_PALETTE_PANEL_PAL_PALETTE_NUMBER_MINUS);
    CHECK_PAL(pal_palette_number_plus,
              CAD_PALETTE_PANEL_PAL_PALETTE_NUMBER_PLUS);
    CHECK_PAL(pal_color_count_minus,
              CAD_PALETTE_PANEL_PAL_COLOR_COUNT_MINUS);
    CHECK_PAL(pal_color_count_plus,
              CAD_PALETTE_PANEL_PAL_COLOR_COUNT_PLUS);
    CHECK_PAL(pal_sample_minus, CAD_PALETTE_PANEL_PAL_SAMPLE_MINUS);
    CHECK_PAL(pal_sample_plus, CAD_PALETTE_PANEL_PAL_SAMPLE_PLUS);
    CHECK_PAL(pal_index_minus, CAD_PALETTE_PANEL_PAL_INDEX_MINUS);
    CHECK_PAL(pal_index_plus, CAD_PALETTE_PANEL_PAL_INDEX_PLUS);
    CHECK_PAL(pal_apply_selected,
              CAD_PALETTE_PANEL_PAL_APPLY_SELECTED);
#undef CHECK_PAL

    hit = hit_center(&layout, CAD_PALETTE_PANEL_TAB_COL,
                      layout.pal_type_minus);
    CHECK(hit.action == CAD_PALETTE_PANEL_COL_RED_MINUS);
    hit = hit_center(&layout, CAD_PALETTE_PANEL_TAB_COL,
                     layout.pal_apply_selected);
    CHECK(hit.action == CAD_PALETTE_PANEL_NONE);
}

static void test_primary_mapping_and_half_open_edges(void) {
    CadPalettePanelLayout layout;
    CadPalettePanelHit hit;
    int index = 999;
    CadPalettePanel_ComputeLayout(
        (CadPalettePanelRect){37, 19, 720, 520}, &layout);

    CHECK(CadPalettePanel_MapPrimary(&layout,
              layout.primary_grid.x, layout.primary_grid.y, &index));
    CHECK(index == 0);
    CHECK(CadPalettePanel_MapPrimary(&layout,
              layout.primary_grid.x + 20, layout.primary_grid.y, &index));
    CHECK(index == 1);
    CHECK(CadPalettePanel_MapPrimary(&layout,
              layout.primary_grid.x, layout.primary_grid.y + 20, &index));
    CHECK(index == 16);
    CHECK(CadPalettePanel_MapPrimary(&layout,
              layout.primary_grid.x + layout.primary_grid.w - 1,
              layout.primary_grid.y + layout.primary_grid.h - 1, &index));
    CHECK(index == 255);

    CHECK(!CadPalettePanel_MapPrimary(&layout,
               layout.primary_grid.x - 1, layout.primary_grid.y, &index));
    CHECK(index == -1);
    CHECK(!CadPalettePanel_MapPrimary(&layout,
               layout.primary_grid.x + layout.primary_grid.w,
               layout.primary_grid.y, &index));
    CHECK(index == -1);
    CHECK(!CadPalettePanel_MapPrimary(&layout,
               layout.primary_grid.x,
               layout.primary_grid.y + layout.primary_grid.h, &index));
    CHECK(index == -1);

    hit = CadPalettePanel_HitTest(
        &layout, CAD_PALETTE_PANEL_TAB_COL,
        layout.primary_grid.x + layout.primary_grid.w - 1,
        layout.primary_grid.y + layout.primary_grid.h - 1);
    CHECK(hit.action == CAD_PALETTE_PANEL_PRIMARY_GRID);
    CHECK(hit.primaryIndex == 255 && hit.sampleIndex == -1);
}

static void test_sample_mapping_and_half_open_edges(void) {
    CadPalettePanelLayout layout;
    CadPalettePanelHit hit;
    int index = 999;
    CadPalettePanel_ComputeLayout(
        (CadPalettePanelRect){13, 27, 720, 520}, &layout);

    CHECK(CadPalettePanel_MapSample(&layout,
              layout.sample_grid.x, layout.sample_grid.y, &index));
    CHECK(index == 0);
    CHECK(CadPalettePanel_MapSample(&layout,
              layout.sample_grid.x + 22, layout.sample_grid.y, &index));
    CHECK(index == 1);
    CHECK(CadPalettePanel_MapSample(&layout,
              layout.sample_grid.x, layout.sample_grid.y + 22, &index));
    CHECK(index == 16);
    CHECK(CadPalettePanel_MapSample(&layout,
              layout.sample_grid.x + layout.sample_grid.w - 1,
              layout.sample_grid.y + layout.sample_grid.h - 1, &index));
    CHECK(index == 127);

    CHECK(!CadPalettePanel_MapSample(&layout,
               layout.sample_grid.x + layout.sample_grid.w,
               layout.sample_grid.y, &index));
    CHECK(index == -1);
    CHECK(!CadPalettePanel_MapSample(&layout,
               layout.sample_grid.x,
               layout.sample_grid.y + layout.sample_grid.h, &index));
    CHECK(index == -1);

    hit = CadPalettePanel_HitTest(
        &layout, CAD_PALETTE_PANEL_TAB_PAL,
        layout.sample_grid.x + layout.sample_grid.w - 1,
        layout.sample_grid.y + layout.sample_grid.h - 1);
    CHECK(hit.action == CAD_PALETTE_PANEL_SAMPLE_GRID);
    CHECK(hit.primaryIndex == -1 && hit.sampleIndex == 127);

    hit = CadPalettePanel_HitTest(
        &layout, CAD_PALETTE_PANEL_TAB_COL,
        layout.sample_grid.x + 2, layout.sample_grid.y + 2);
    CHECK(hit.action == CAD_PALETTE_PANEL_NONE);
}

static void test_narrow_and_tiny_layout_clipping(void) {
    CadPalettePanelLayout layout;
    CadPalettePanelHit hit;
    int index = 17;
    CadPalettePanel_ComputeLayout(
        (CadPalettePanelRect){0, 0, 600, 420}, &layout);
    CHECK(layout.usable);
    CHECK(layout.new_palette.w > 0);
    CHECK(layout.primary_grid.w > 0);
    CHECK(layout.col_red_minus.w > 0);
    CHECK(layout.sample_grid.w == 0);
    CHECK(layout.pal_apply_selected.w == 0);
    CHECK(!CadPalettePanel_MapSample(&layout, 0, 0, &index));
    CHECK(index == -1);

    CadPalettePanel_ComputeLayout(
        (CadPalettePanelRect){0, 0, 360, 250}, &layout);
    CHECK(layout.usable);
    CHECK(layout.new_palette.w > 0);
    CHECK(layout.tab_pal.w > 0);
    CHECK(layout.primary_grid.w == 0);
    CHECK(layout.col_red_minus.w == 0);
    hit = CadPalettePanel_HitTest(&layout, CAD_PALETTE_PANEL_TAB_COL,
                                  359, 249);
    CHECK(hit.action == CAD_PALETTE_PANEL_NONE);

    CadPalettePanel_ComputeLayout(
        (CadPalettePanelRect){0, 0, 200, 100}, &layout);
    CHECK(!layout.usable);
    CHECK(layout.new_palette.w == 0);
    CHECK(layout.primary_grid.w == 0);
}

static void test_null_and_outside_inputs(void) {
    CadPalettePanelLayout layout;
    CadPalettePanelHit hit;
    int index = 12;
    CadPalettePanel_ComputeLayout(
        (CadPalettePanelRect){100, 100, 720, 520}, &layout);
    CadPalettePanel_ComputeLayout((CadPalettePanelRect){0, 0, 0, 0}, NULL);
    CHECK(!CadPalettePanel_MapPrimary(NULL, 0, 0, &index));
    CHECK(index == -1);
    CHECK(!CadPalettePanel_MapSample(NULL, 0, 0, &index));
    CHECK(index == -1);
    hit = CadPalettePanel_HitTest(NULL, CAD_PALETTE_PANEL_TAB_COL, 0, 0);
    CHECK(hit.action == CAD_PALETTE_PANEL_NONE);
    CHECK(hit.primaryIndex == -1 && hit.sampleIndex == -1);
    hit = CadPalettePanel_HitTest(&layout, CAD_PALETTE_PANEL_TAB_COL,
                                  layout.panel.x - 1, layout.panel.y);
    CHECK(hit.action == CAD_PALETTE_PANEL_NONE);
    hit = CadPalettePanel_HitTest(
        &layout, CAD_PALETTE_PANEL_TAB_COL,
        layout.panel.x + layout.panel.w, layout.panel.y);
    CHECK(hit.action == CAD_PALETTE_PANEL_NONE);
}

int main(void) {
    test_reference_layout_and_common_hits();
    test_col_hits_and_tab_occlusion();
    test_pal_hits_and_tab_occlusion();
    test_primary_mapping_and_half_open_edges();
    test_sample_mapping_and_half_open_edges();
    test_narrow_and_tiny_layout_clipping();
    test_null_and_outside_inputs();
    if (failures) {
        fprintf(stderr, "%d palette panel test(s) failed\n", failures);
        return 1;
    }
    puts("palette panel tests passed");
    return 0;
}
