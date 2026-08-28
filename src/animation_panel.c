#include "animation_panel.h"

#include "cad_file.h"

#include <math.h>
#include <string.h>

static CadAnimationPanelRect make_rect(int x, int y, int w, int h) {
    CadAnimationPanelRect result = {x, y, w > 0 ? w : 0, h > 0 ? h : 0};
    return result;
}

static int contains(CadAnimationPanelRect rect, int x, int y) {
    return rect.w > 0 && rect.h > 0 && x >= rect.x && y >= rect.y &&
           x < rect.x + rect.w && y < rect.y + rect.h;
}

static int fits(CadAnimationPanelRect child, CadAnimationPanelRect parent) {
    return child.w > 0 && child.h > 0 && child.x >= parent.x &&
           child.y >= parent.y && child.x + child.w <= parent.x + parent.w &&
           child.y + child.h <= parent.y + parent.h;
}

static void clip_control(CadAnimationPanelRect* control,
                         CadAnimationPanelRect inner) {
    if (control && !fits(*control, inner)) memset(control, 0, sizeof(*control));
}

void CadAnimationPanel_ComputeLayout(CadAnimationPanelRect panel,
                                     CadAnimationPanelLayout* output) {
    int x;
    int y;
    if (!output) return;
    memset(output, 0, sizeof(*output));
    output->panel = panel;
    output->inner = make_rect(panel.x + 6, panel.y + 26,
                              panel.w - 12, panel.h - 32);
    output->usable = output->inner.w >= 380 && output->inner.h >= 92;
    if (!output->usable) return;

    x = output->inner.x + 4;
    y = output->inner.y + 3;
#define ADD_CONTROL(field, width)                                             \
    do {                                                                      \
        output->field = make_rect(x, y, (width), 20);                         \
        x += (width) + 4;                                                     \
    } while (0)
    ADD_CONTROL(first, 28);
    ADD_CONTROL(previous, 28);
    ADD_CONTROL(play, 48);
    ADD_CONTROL(stop, 42);
    ADD_CONTROL(next, 28);
    ADD_CONTROL(last, 28);
    /* The zero-based frame/FPS readout is painted in this gap.  Give the
       full label room on normal desktop widths, but retain the recovered
       compact layout (and its useful Loop control) in a narrow float. */
    x += output->inner.w >= 700 ? 132 : 108;
    ADD_CONTROL(loop, 48);
    ADD_CONTROL(interpolation, 60);
    ADD_CONTROL(fps_down, 40);
    ADD_CONTROL(fps_up, 40);
    ADD_CONTROL(all_frames, 88);
    ADD_CONTROL(dock, 52);

    output->strip = make_rect(output->inner.x + 4, output->inner.y + 27,
                              output->inner.w - 8, 18);

    x = output->inner.x + 4;
    y = output->inner.y + 49;
    ADD_CONTROL(create_all, 72);
    ADD_CONTROL(create_selected, 80);
    ADD_CONTROL(count_down, 58);
    ADD_CONTROL(count_up, 58);
    ADD_CONTROL(insert, 52);
    ADD_CONTROL(duplicate, 68);
    ADD_CONTROL(delete_frame, 56);

    x = output->inner.x + 4;
    y = output->inner.y + 72;
    ADD_CONTROL(copy_all, 66);
    ADD_CONTROL(copy_selected, 80);
    ADD_CONTROL(add_faces, 74);
    ADD_CONTROL(static_copy, 128);
#undef ADD_CONTROL

#define CLIP(field) clip_control(&output->field, output->inner)
    CLIP(first); CLIP(previous); CLIP(play); CLIP(stop); CLIP(next); CLIP(last);
    CLIP(loop); CLIP(interpolation); CLIP(fps_down); CLIP(fps_up);
    CLIP(all_frames); CLIP(dock); CLIP(strip);
    CLIP(create_all); CLIP(create_selected); CLIP(count_down); CLIP(count_up);
    CLIP(insert); CLIP(duplicate); CLIP(delete_frame);
    CLIP(copy_all); CLIP(copy_selected); CLIP(add_faces); CLIP(static_copy);
#undef CLIP
}

int CadAnimationPanel_MapFrame(const CadAnimationPanelLayout* layout,
                               int pointerX, int frameCount, int fractional,
                               int* frameIndex, double* framePosition) {
    double cell;
    double raw;
    int frame;
    if (!layout || layout->strip.w <= 0) return 0;
    if (frameCount < 1) frameCount = 1;
    if (frameCount > CAD_ANIMATION_FRAMES) frameCount = CAD_ANIMATION_FRAMES;
    cell = (double)layout->strip.w / (double)CAD_ANIMATION_FRAMES;
    frame = (int)floor(((double)pointerX - layout->strip.x) / cell);
    if (frame < 0) frame = 0;
    if (frame >= frameCount) frame = frameCount - 1;
    raw = fractional
              ? ((double)pointerX - layout->strip.x) / cell - 0.5
              : (double)frame;
    if (raw < 0.0) raw = 0.0;
    if (raw > frameCount - 1) raw = (double)(frameCount - 1);
    if (frameIndex) *frameIndex = frame;
    if (framePosition) *framePosition = raw;
    return 1;
}

CadAnimationPanelHit CadAnimationPanel_HitTest(
    const CadAnimationPanelLayout* layout, int pointerX, int pointerY,
    int frameCount, int fractionalStrip) {
    CadAnimationPanelHit hit;
    memset(&hit, 0, sizeof(hit));
    hit.frameIndex = -1;
    if (!layout || !layout->usable ||
        !contains(layout->panel, pointerX, pointerY)) return hit;
    if (contains(layout->strip, pointerX, pointerY)) {
        hit.action = CAD_ANIMATION_PANEL_STRIP;
        CadAnimationPanel_MapFrame(layout, pointerX, frameCount,
                                   fractionalStrip, &hit.frameIndex,
                                   &hit.framePosition);
        return hit;
    }
#define HIT(field, value)                                                     \
    if (contains(layout->field, pointerX, pointerY)) {                        \
        hit.action = (value);                                                 \
        return hit;                                                           \
    }
    HIT(first, CAD_ANIMATION_PANEL_FIRST);
    HIT(previous, CAD_ANIMATION_PANEL_PREVIOUS);
    HIT(play, CAD_ANIMATION_PANEL_PLAY_PAUSE);
    HIT(stop, CAD_ANIMATION_PANEL_STOP);
    HIT(next, CAD_ANIMATION_PANEL_NEXT);
    HIT(last, CAD_ANIMATION_PANEL_LAST);
    HIT(loop, CAD_ANIMATION_PANEL_TOGGLE_LOOP);
    HIT(interpolation, CAD_ANIMATION_PANEL_TOGGLE_INTERPOLATION);
    HIT(fps_down, CAD_ANIMATION_PANEL_FPS_DOWN);
    HIT(fps_up, CAD_ANIMATION_PANEL_FPS_UP);
    HIT(all_frames, CAD_ANIMATION_PANEL_TOGGLE_ALL_FRAMES);
    HIT(dock, CAD_ANIMATION_PANEL_TOGGLE_DOCK);
    HIT(create_all, CAD_ANIMATION_PANEL_CREATE_ALL);
    HIT(create_selected, CAD_ANIMATION_PANEL_CREATE_SELECTED);
    HIT(count_down, CAD_ANIMATION_PANEL_COUNT_DOWN);
    HIT(count_up, CAD_ANIMATION_PANEL_COUNT_UP);
    HIT(insert, CAD_ANIMATION_PANEL_INSERT);
    HIT(duplicate, CAD_ANIMATION_PANEL_DUPLICATE);
    HIT(delete_frame, CAD_ANIMATION_PANEL_DELETE);
    HIT(copy_all, CAD_ANIMATION_PANEL_COPY_ALL);
    HIT(copy_selected, CAD_ANIMATION_PANEL_COPY_SELECTED);
    HIT(add_faces, CAD_ANIMATION_PANEL_ADD_FACES);
    HIT(static_copy, CAD_ANIMATION_PANEL_MAKE_STATIC_COPY);
#undef HIT
    return hit;
}

int CadAnimationPanel_IsActionEnabled(
    CadAnimationPanelAction action, const CadAnimationPanelState* state) {
    if (!state) return 0;
    switch (action) {
    case CAD_ANIMATION_PANEL_TOGGLE_DOCK:
        return 1;
    case CAD_ANIMATION_PANEL_FIRST:
    case CAD_ANIMATION_PANEL_PREVIOUS:
    case CAD_ANIMATION_PANEL_STOP:
    case CAD_ANIMATION_PANEL_NEXT:
    case CAD_ANIMATION_PANEL_LAST:
    case CAD_ANIMATION_PANEL_TOGGLE_LOOP:
    case CAD_ANIMATION_PANEL_TOGGLE_INTERPOLATION:
    case CAD_ANIMATION_PANEL_FPS_DOWN:
    case CAD_ANIMATION_PANEL_FPS_UP:
    case CAD_ANIMATION_PANEL_TOGGLE_ALL_FRAMES:
    case CAD_ANIMATION_PANEL_COPY_ALL:
    case CAD_ANIMATION_PANEL_STRIP:
        return state->editable;
    case CAD_ANIMATION_PANEL_PLAY_PAUSE:
        return state->editable && state->frameCount > 1;
    case CAD_ANIMATION_PANEL_CREATE_ALL:
        return state->informationValid && !state->editable &&
               state->staticFaceCount > 0;
    case CAD_ANIMATION_PANEL_CREATE_SELECTED:
        return state->informationValid && !state->editable &&
               state->selectedStaticFaceCount > 0;
    case CAD_ANIMATION_PANEL_COUNT_DOWN:
    case CAD_ANIMATION_PANEL_DELETE:
        return state->editable && state->frameCount > 1;
    case CAD_ANIMATION_PANEL_COUNT_UP:
    case CAD_ANIMATION_PANEL_INSERT:
    case CAD_ANIMATION_PANEL_DUPLICATE:
        return state->editable && state->frameCount < state->maximumFrameCount;
    case CAD_ANIMATION_PANEL_COPY_SELECTED:
        return state->editable && state->selectedPointCount > 0;
    case CAD_ANIMATION_PANEL_ADD_FACES:
        return state->editable && state->staticFaceCount > 0 &&
               state->selectedStaticFaceCount > 0;
    case CAD_ANIMATION_PANEL_MAKE_STATIC_COPY:
        return state->hasAnimation;
    case CAD_ANIMATION_PANEL_NONE:
    default:
        return 0;
    }
}
