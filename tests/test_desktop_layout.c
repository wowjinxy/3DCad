#include "desktop_layout.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(expression)                                                     \
    do {                                                                      \
        if (!(expression)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,          \
                    __LINE__, #expression);                                   \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

static int rect_equal(CadUiRect first, CadUiRect second) {
    return first.x == second.x && first.y == second.y &&
           first.width == second.width && first.height == second.height;
}

static void check_standard_layout(int width, int height) {
    CadDesktopLayoutInput input;
    CadDesktopLayout layout;
    int first;
    int second;
    CadDesktopLayoutInput_Init(&input, width, height);
    CHECK(CadDesktopLayout_Compute(&input, &layout));
    CHECK(rect_equal(layout.client, (CadUiRect){0, 0, width, height}));
    CHECK(CadUiRect_Contains(layout.client, layout.menuBar));
    CHECK(CadUiRect_Contains(layout.client, layout.statusBar));
    CHECK(CadUiRect_Contains(layout.client, layout.toolPalette));
    CHECK(CadUiRect_Contains(layout.client, layout.coordinatesPanel));
    CHECK(CadUiRect_Contains(layout.client, layout.animationPanel));
    CHECK(CadUiRect_Contains(layout.client,
                             layout.floatingAnimationDefault));
    CHECK(layout.viewColumns == 2);
    CHECK(layout.viewRows == 2);

    for (first = 0; first < CAD_DESKTOP_VIEW_COUNT; ++first) {
        CHECK(!CadUiRect_IsEmpty(layout.views[first]));
        CHECK(CadUiRect_Contains(layout.viewArea, layout.views[first]));
        CHECK(!CadUiRect_Intersects(layout.views[first],
                                    layout.toolPalette));
        CHECK(!CadUiRect_Intersects(layout.views[first],
                                    layout.coordinatesPanel));
        CHECK(!CadUiRect_Intersects(layout.views[first],
                                    layout.animationPanel));
        for (second = first + 1; second < CAD_DESKTOP_VIEW_COUNT; ++second)
            CHECK(!CadUiRect_Intersects(layout.views[first],
                                        layout.views[second]));
    }
    CHECK(!CadUiRect_Intersects(layout.coordinatesPanel,
                                layout.animationPanel));
    CHECK(!CadUiRect_Intersects(layout.toolPalette,
                                layout.coordinatesPanel));
    CHECK(!CadUiRect_Intersects(layout.toolPalette,
                                layout.animationPanel));
}

static void test_reference_sizes(void) {
    check_standard_layout(1024, 768);
    check_standard_layout(1258, 983);
    check_standard_layout(3840, 2160);
}

static void test_constant_size_reflow(void) {
    CadDesktopLayoutInput input;
    CadDesktopLayout baseline;
    CadDesktopLayout repeat;
    CadDesktopLayout noCoordinates;
    CadDesktopLayout floating;
    CadDesktopLayout restored;
    CadDesktopLayoutInput_Init(&input, 1258, 983);
    CHECK(CadDesktopLayout_Compute(&input, &baseline));
    CHECK(CadDesktopLayout_Compute(&input, &repeat));
    CHECK(memcmp(&baseline, &repeat, sizeof(baseline)) == 0);

    input.coordinatesVisible = 0;
    CHECK(CadDesktopLayout_Compute(&input, &noCoordinates));
    CHECK(CadUiRect_IsEmpty(noCoordinates.coordinatesPanel));
    CHECK(noCoordinates.viewArea.height > baseline.viewArea.height);

    input.coordinatesVisible = 1;
    input.animationDocked = 0;
    CHECK(CadDesktopLayout_Compute(&input, &floating));
    CHECK(CadUiRect_IsEmpty(floating.animationPanel));
    CHECK(!CadUiRect_IsEmpty(floating.floatingAnimationDefault));
    CHECK(floating.viewArea.height > baseline.viewArea.height);

    input.animationDocked = 1;
    CHECK(CadDesktopLayout_Compute(&input, &restored));
    CHECK(memcmp(&baseline, &restored, sizeof(baseline)) == 0);
}

static void test_view_visibility(void) {
    CadDesktopLayoutInput input;
    CadDesktopLayout layout;
    int visibleCount;
    int view;
    CadDesktopLayoutInput_Init(&input, 1024, 768);
    for (visibleCount = 0; visibleCount <= CAD_DESKTOP_VIEW_COUNT;
         ++visibleCount) {
        for (view = 0; view < CAD_DESKTOP_VIEW_COUNT; ++view)
            input.viewVisible[view] = (unsigned char)(view < visibleCount);
        CHECK(CadDesktopLayout_Compute(&input, &layout));
        CHECK(layout.viewColumns == (visibleCount == 0 ? 0 :
                                     visibleCount == 1 ? 1 : 2));
        CHECK(layout.viewRows == (visibleCount == 0 ? 0 :
                                  visibleCount <= 2 ? 1 : 2));
        for (view = 0; view < CAD_DESKTOP_VIEW_COUNT; ++view) {
            if (view < visibleCount) {
                CHECK(!CadUiRect_IsEmpty(layout.views[view]));
                CHECK(CadUiRect_Contains(layout.viewArea,
                                         layout.views[view]));
            } else {
                CHECK(CadUiRect_IsEmpty(layout.views[view]));
            }
        }
    }
}

static void test_dpi_conversion(void) {
    CadUiRect logical = {10, 20, 100, 50};
    CadUiRect physical;
    CadUiRect roundTrip;
    CadDesktopLayoutInput input;
    CadDesktopLayout layout;
    CHECK(CadUi_LogicalToPhysical(10, 1.25) == 13);
    CHECK(CadUi_PhysicalToLogical(150, 1.5) == 100);
    CHECK(CadUi_LogicalToPhysical(-10, 1.25) == -13);
    CHECK(CadUi_PhysicalToLogical(-15, 1.5) == -10);
    CHECK(CadUi_LogicalToPhysical(37, 0.0) == 37);

    physical = CadUiRect_LogicalToPhysical(logical, 2.0);
    CHECK(rect_equal(physical, (CadUiRect){20, 40, 200, 100}));
    roundTrip = CadUiRect_PhysicalToLogical(physical, 2.0);
    CHECK(rect_equal(roundTrip, logical));

    CadDesktopLayoutInput_InitPhysical(&input, 2048, 1536, 2.0);
    CHECK(input.logicalWidth == 1024);
    CHECK(input.logicalHeight == 768);
    CHECK(CadDesktopLayout_Compute(&input, &layout));
    CHECK(layout.client.width == 1024);
    CHECK(layout.client.height == 768);
    CHECK(layout.dpiScale == 2.0);
}

static void test_small_client_stays_bounded(void) {
    CadDesktopLayoutInput input;
    CadDesktopLayout layout;
    int view;
    CadDesktopLayoutInput_Init(&input, 320, 200);
    CHECK(CadDesktopLayout_Compute(&input, &layout));
    CHECK(CadUiRect_Contains(layout.client, layout.toolPalette));
    CHECK(CadUiRect_Contains(layout.client, layout.coordinatesPanel));
    CHECK(CadUiRect_Contains(layout.client, layout.animationPanel));
    for (view = 0; view < CAD_DESKTOP_VIEW_COUNT; ++view)
        CHECK(CadUiRect_Contains(layout.client, layout.views[view]));
}

static void test_manual_window_reachability(void) {
    CadUiRect work = {0, 20, 640, 430};
    CadUiRect original = {900, 700, 900, 126};
    CadUiRect clamped = CadUiRect_ClampReachable(original, work, 20, 80);
    CHECK(clamped.width == original.width);
    CHECK(clamped.height == original.height);
    CHECK(clamped.x == 560);
    CHECK(clamped.y == 430);
    CHECK(CadUiRect_ClampReachable(
              (CadUiRect){-1000, -50, 300, 200}, work, 20, 80).x == -220);
    CHECK(CadUiRect_ClampReachable(
              (CadUiRect){-1000, -50, 300, 200}, work, 20, 80).y == 20);
}

static void test_focus_z_order(void) {
    int order[4] = {0, 1, 2, 3};
    CadUiRect rectangles[4] = {
        {0, 0, 100, 100}, {20, 20, 100, 100},
        {40, 40, 100, 100}, {60, 60, 100, 100}
    };
    unsigned char visible[4] = {1, 1, 1, 1};
    CHECK(CadUiZOrder_TopmostAt(order, 4, rectangles, visible, 70, 70) == 3);
    CHECK(CadUiZOrder_Raise(order, 4, 1));
    CHECK(order[3] == 1);
    CHECK(CadUiZOrder_TopmostAt(order, 4, rectangles, visible, 70, 70) == 1);
    visible[1] = 0;
    CHECK(CadUiZOrder_TopmostAt(order, 4, rectangles, visible, 70, 70) == 3);
    CHECK(!CadUiZOrder_Raise(order, 4, 99));
    CHECK(CadUiZOrder_TopmostAt(order, 4, rectangles, visible, -1, -1) == -1);
}

int main(void) {
    test_reference_sizes();
    test_constant_size_reflow();
    test_view_visibility();
    test_dpi_conversion();
    test_small_client_stays_bounded();
    test_manual_window_reachability();
    test_focus_z_order();
    if (failures) {
        fprintf(stderr, "%d desktop layout test(s) failed\n", failures);
        return 1;
    }
    puts("desktop layout tests passed");
    return 0;
}
