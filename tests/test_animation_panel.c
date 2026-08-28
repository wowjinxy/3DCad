#include "animation_panel.h"

#include <math.h>
#include <stdio.h>

static int failures;

#define CHECK(expression)                                                     \
    do {                                                                      \
        if (!(expression)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,         \
                    __LINE__, #expression);                                   \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

static void test_reference_layout_and_hits(void) {
    CadAnimationPanelLayout layout;
    CadAnimationPanelHit hit;
    CadAnimationPanel_ComputeLayout(
        (CadAnimationPanelRect){90, 610, 900, 126}, &layout);
    CHECK(layout.usable);
    CHECK(layout.strip.w == 880);
    CHECK(layout.static_copy.w == 128);
    hit = CadAnimationPanel_HitTest(&layout, layout.play.x + 2,
                                    layout.play.y + 2, 16, 1);
    CHECK(hit.action == CAD_ANIMATION_PANEL_PLAY_PAUSE);
    hit = CadAnimationPanel_HitTest(&layout, layout.static_copy.x + 2,
                                    layout.static_copy.y + 2, 16, 1);
    CHECK(hit.action == CAD_ANIMATION_PANEL_MAKE_STATIC_COPY);
}

static void test_strip_mapping(void) {
    CadAnimationPanelLayout layout;
    CadAnimationPanelHit hit;
    int frame = -1;
    double position = -1.0;
    CadAnimationPanel_ComputeLayout(
        (CadAnimationPanelRect){0, 0, 900, 126}, &layout);
    CHECK(CadAnimationPanel_MapFrame(&layout, layout.strip.x, 16, 1,
                                     &frame, &position));
    CHECK(frame == 0 && position == 0.0);
    CHECK(CadAnimationPanel_MapFrame(&layout,
              layout.strip.x +
                  (int)lround(5.5 * (double)layout.strip.w / 64.0),
              16, 1, &frame, &position));
    CHECK(frame == 5);
    CHECK(fabs(position - 5.0) < 0.1);
    CHECK(CadAnimationPanel_MapFrame(&layout,
              layout.strip.x + layout.strip.w + 100, 16, 1,
              &frame, &position));
    CHECK(frame == 15 && position == 15.0);
    hit = CadAnimationPanel_HitTest(
        &layout, layout.strip.x + layout.strip.w - 1,
        layout.strip.y + 2, 16, 0);
    CHECK(hit.action == CAD_ANIMATION_PANEL_STRIP);
    CHECK(hit.frameIndex == 15);
}

static void test_narrow_layout_clips_controls(void) {
    CadAnimationPanelLayout layout;
    CadAnimationPanelHit hit;
    CadAnimationPanel_ComputeLayout(
        (CadAnimationPanelRect){0, 0, 410, 126}, &layout);
    CHECK(layout.usable);
    CHECK(layout.first.w > 0);
    CHECK(layout.loop.w > 0);
    CHECK(layout.interpolation.w == 0);
    CHECK(layout.static_copy.w > 0);
    hit = CadAnimationPanel_HitTest(&layout, 405, 31, 8, 0);
    CHECK(hit.action == CAD_ANIMATION_PANEL_NONE);

    CadAnimationPanel_ComputeLayout(
        (CadAnimationPanelRect){0, 0, 320, 100}, &layout);
    CHECK(!layout.usable);
    CHECK(layout.strip.w == 0);
}

static void test_action_capability_matches_disabled_controls(void) {
    CadAnimationPanelState state = {0};
    state.informationValid = 1;
    state.staticFaceCount = 3;
    state.selectedStaticFaceCount = 1;
    state.maximumFrameCount = 16;
    CHECK(CadAnimationPanel_IsActionEnabled(CAD_ANIMATION_PANEL_CREATE_ALL,
                                            &state));
    CHECK(CadAnimationPanel_IsActionEnabled(CAD_ANIMATION_PANEL_CREATE_SELECTED,
                                            &state));
    CHECK(!CadAnimationPanel_IsActionEnabled(CAD_ANIMATION_PANEL_PLAY_PAUSE,
                                             &state));
    CHECK(!CadAnimationPanel_IsActionEnabled(CAD_ANIMATION_PANEL_STRIP,
                                             &state));
    CHECK(CadAnimationPanel_IsActionEnabled(CAD_ANIMATION_PANEL_TOGGLE_DOCK,
                                            &state));

    state.editable = 1;
    state.frameCount = 16;
    state.selectedPointCount = 2;
    state.hasAnimation = 1;
    CHECK(CadAnimationPanel_IsActionEnabled(CAD_ANIMATION_PANEL_PLAY_PAUSE,
                                            &state));
    CHECK(CadAnimationPanel_IsActionEnabled(CAD_ANIMATION_PANEL_COPY_SELECTED,
                                            &state));
    CHECK(CadAnimationPanel_IsActionEnabled(CAD_ANIMATION_PANEL_ADD_FACES,
                                            &state));
    CHECK(!CadAnimationPanel_IsActionEnabled(CAD_ANIMATION_PANEL_COUNT_UP,
                                             &state));
    CHECK(!CadAnimationPanel_IsActionEnabled(CAD_ANIMATION_PANEL_INSERT,
                                             &state));
    CHECK(!CadAnimationPanel_IsActionEnabled(CAD_ANIMATION_PANEL_DUPLICATE,
                                             &state));
    CHECK(CadAnimationPanel_IsActionEnabled(
              CAD_ANIMATION_PANEL_MAKE_STATIC_COPY, &state));

    state.frameCount = 1;
    state.maximumFrameCount = 16;
    state.selectedPointCount = 0;
    state.selectedStaticFaceCount = 0;
    CHECK(!CadAnimationPanel_IsActionEnabled(CAD_ANIMATION_PANEL_PLAY_PAUSE,
                                             &state));
    CHECK(!CadAnimationPanel_IsActionEnabled(CAD_ANIMATION_PANEL_DELETE,
                                             &state));
    CHECK(!CadAnimationPanel_IsActionEnabled(CAD_ANIMATION_PANEL_COPY_SELECTED,
                                             &state));
    CHECK(!CadAnimationPanel_IsActionEnabled(CAD_ANIMATION_PANEL_ADD_FACES,
                                             &state));
    CHECK(CadAnimationPanel_IsActionEnabled(CAD_ANIMATION_PANEL_COUNT_UP,
                                            &state));
}

int main(void) {
    test_reference_layout_and_hits();
    test_strip_mapping();
    test_narrow_layout_clips_controls();
    test_action_capability_matches_disabled_controls();
    if (failures) {
        fprintf(stderr, "%d animation panel test(s) failed\n", failures);
        return 1;
    }
    puts("animation panel tests passed");
    return 0;
}
