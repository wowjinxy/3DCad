#ifndef ANIMATION_PANEL_H
#define ANIMATION_PANEL_H

/* SDL-independent geometry and hit model for the compact animation timeline.
   The GUI owns drawing and command dispatch; this module keeps panel reflow,
   clipping, and the zero-based 64-frame strip deterministic and testable. */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CadAnimationPanelRect {
    int x;
    int y;
    int w;
    int h;
} CadAnimationPanelRect;

typedef enum CadAnimationPanelAction {
    CAD_ANIMATION_PANEL_NONE = 0,
    CAD_ANIMATION_PANEL_FIRST,
    CAD_ANIMATION_PANEL_PREVIOUS,
    CAD_ANIMATION_PANEL_PLAY_PAUSE,
    CAD_ANIMATION_PANEL_STOP,
    CAD_ANIMATION_PANEL_NEXT,
    CAD_ANIMATION_PANEL_LAST,
    CAD_ANIMATION_PANEL_TOGGLE_LOOP,
    CAD_ANIMATION_PANEL_TOGGLE_INTERPOLATION,
    CAD_ANIMATION_PANEL_FPS_DOWN,
    CAD_ANIMATION_PANEL_FPS_UP,
    CAD_ANIMATION_PANEL_TOGGLE_ALL_FRAMES,
    CAD_ANIMATION_PANEL_TOGGLE_DOCK,
    CAD_ANIMATION_PANEL_STRIP,
    CAD_ANIMATION_PANEL_CREATE_ALL,
    CAD_ANIMATION_PANEL_CREATE_SELECTED,
    CAD_ANIMATION_PANEL_COUNT_DOWN,
    CAD_ANIMATION_PANEL_COUNT_UP,
    CAD_ANIMATION_PANEL_INSERT,
    CAD_ANIMATION_PANEL_DUPLICATE,
    CAD_ANIMATION_PANEL_DELETE,
    CAD_ANIMATION_PANEL_COPY_ALL,
    CAD_ANIMATION_PANEL_COPY_SELECTED,
    CAD_ANIMATION_PANEL_ADD_FACES,
    CAD_ANIMATION_PANEL_MAKE_STATIC_COPY
} CadAnimationPanelAction;

typedef struct CadAnimationPanelLayout {
    CadAnimationPanelRect panel;
    CadAnimationPanelRect inner;
    CadAnimationPanelRect first, previous, play, stop, next, last;
    CadAnimationPanelRect loop, interpolation, fps_down, fps_up, all_frames, dock;
    CadAnimationPanelRect strip;
    CadAnimationPanelRect create_all, create_selected, count_down, count_up;
    CadAnimationPanelRect insert, duplicate, delete_frame;
    CadAnimationPanelRect copy_all, copy_selected, add_faces, static_copy;
    int usable;
} CadAnimationPanelLayout;

typedef struct CadAnimationPanelHit {
    CadAnimationPanelAction action;
    int frameIndex;
    double framePosition;
} CadAnimationPanelHit;

/* The panel's command capability model.  Drawing and dispatch must both ask
   this API so a greyed control can never remain clickable. */
typedef struct CadAnimationPanelState {
    int informationValid;
    int editable;
    int frameCount;
    int maximumFrameCount;
    int staticFaceCount;
    int selectedStaticFaceCount;
    int selectedPointCount;
    int hasAnimation;
} CadAnimationPanelState;

void CadAnimationPanel_ComputeLayout(CadAnimationPanelRect panel,
                                     CadAnimationPanelLayout* output);

/* Maps a horizontal pointer coordinate into the fixed 64-cell strip.  The
   returned frame is clamped to the stored frame count.  Fractional mapping is
   intended for interpolation preview while dragging. */
int CadAnimationPanel_MapFrame(const CadAnimationPanelLayout* layout,
                               int pointerX, int frameCount, int fractional,
                               int* frameIndex, double* framePosition);

CadAnimationPanelHit CadAnimationPanel_HitTest(
    const CadAnimationPanelLayout* layout, int pointerX, int pointerY,
    int frameCount, int fractionalStrip);

int CadAnimationPanel_IsActionEnabled(
    CadAnimationPanelAction action, const CadAnimationPanelState* state);

#ifdef __cplusplus
}
#endif

#endif
