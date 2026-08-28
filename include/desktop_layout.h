#ifndef DESKTOP_LAYOUT_H
#define DESKTOP_LAYOUT_H

/* SDL/Win32-independent geometry for the four-view editor desktop.
   All layout rectangles use logical pixels.  The explicit scale helpers are
   provided for boundaries that report physical pixels (framebuffer sizes,
   native hit-test coordinates, and DPI-sensitive tests). */

#ifdef __cplusplus
extern "C" {
#endif

#define CAD_DESKTOP_VIEW_COUNT 4

typedef enum CadDesktopViewSlot {
    CAD_DESKTOP_VIEW_TOP = 0,
    CAD_DESKTOP_VIEW_3D,
    CAD_DESKTOP_VIEW_FRONT,
    CAD_DESKTOP_VIEW_RIGHT
} CadDesktopViewSlot;

typedef struct CadUiRect {
    int x;
    int y;
    int width;
    int height;
} CadUiRect;

typedef struct CadDesktopLayoutMetrics {
    int margin;
    int menuHeight;
    int statusHeight;
    int toolPaletteWidth;
    int coordinatesHeight;
    int animationHeight;
    int minimumViewHeight;
    int floatingAnimationWidth;
} CadDesktopLayoutMetrics;

typedef struct CadDesktopLayoutInput {
    int logicalWidth;
    int logicalHeight;
    double dpiScale;
    unsigned char viewVisible[CAD_DESKTOP_VIEW_COUNT];
    unsigned char toolPaletteVisible;
    unsigned char coordinatesVisible;
    unsigned char animationVisible;
    unsigned char animationDocked;
    CadDesktopLayoutMetrics metrics;
} CadDesktopLayoutInput;

typedef struct CadDesktopLayout {
    CadUiRect client;
    CadUiRect menuBar;
    CadUiRect statusBar;
    CadUiRect workArea;
    CadUiRect viewArea;
    CadUiRect toolPalette;
    CadUiRect coordinatesPanel;
    CadUiRect animationPanel;
    /* Suggested initial rectangle for a floating animation panel.  Floating
       panels intentionally overlap the desktop, so this rectangle is not
       included in the non-overlapping dock geometry. */
    CadUiRect floatingAnimationDefault;
    CadUiRect views[CAD_DESKTOP_VIEW_COUNT];
    int viewColumns;
    int viewRows;
    double dpiScale;
} CadDesktopLayout;

void CadDesktopLayoutMetrics_Default(CadDesktopLayoutMetrics* metrics);
void CadDesktopLayoutInput_Init(CadDesktopLayoutInput* input,
                                int logicalWidth, int logicalHeight);
void CadDesktopLayoutInput_InitPhysical(CadDesktopLayoutInput* input,
                                        int physicalWidth, int physicalHeight,
                                        double dpiScale);

/* Recomputes every output rectangle from the supplied state.  It deliberately
   has no size-change cache: visibility or docking changes reflow correctly
   even when the client dimensions are unchanged. */
int CadDesktopLayout_Compute(const CadDesktopLayoutInput* input,
                             CadDesktopLayout* output);

int CadUiRect_IsEmpty(CadUiRect rect);
int CadUiRect_Contains(CadUiRect outer, CadUiRect inner);
int CadUiRect_Intersects(CadUiRect first, CadUiRect second);

int CadUi_LogicalToPhysical(int logicalPixels, double dpiScale);
int CadUi_PhysicalToLogical(int physicalPixels, double dpiScale);
CadUiRect CadUiRect_LogicalToPhysical(CadUiRect logical, double dpiScale);
CadUiRect CadUiRect_PhysicalToLogical(CadUiRect physical, double dpiScale);

/* Repositions a manually arranged window just enough to keep its title strip
   and a useful horizontal portion reachable inside the client work area.  It
   deliberately preserves the window size and therefore the user's layout. */
CadUiRect CadUiRect_ClampReachable(CadUiRect window, CadUiRect workArea,
                                   int titleHeight, int minimumVisibleWidth);

/* Small deterministic desktop stack helpers. order is back-to-front and
   contains stable window IDs indexing rectangles/visibility. */
int CadUiZOrder_Raise(int* order, int count, int windowId);
int CadUiZOrder_TopmostAt(const int* order, int count,
                          const CadUiRect* rectangles,
                          const unsigned char* visible,
                          int x, int y);

#ifdef __cplusplus
}
#endif

#endif
