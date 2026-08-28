#include "desktop_layout.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

static int minimum_int(int first, int second) {
    return first < second ? first : second;
}

static int maximum_int(int first, int second) {
    return first > second ? first : second;
}

static int clamp_nonnegative(int value) {
    return value > 0 ? value : 0;
}

static double valid_scale(double scale) {
    return isfinite(scale) && scale > 0.0 ? scale : 1.0;
}

static int rounded_int(double value) {
    if (!isfinite(value)) return value < 0.0 ? INT_MIN : INT_MAX;
    if (value >= (double)INT_MAX) return INT_MAX;
    if (value <= (double)INT_MIN) return INT_MIN;
    return value >= 0.0 ? (int)(value + 0.5) : (int)(value - 0.5);
}

static CadUiRect make_rect(int x, int y, int width, int height) {
    CadUiRect result;
    result.x = x;
    result.y = y;
    result.width = clamp_nonnegative(width);
    result.height = clamp_nonnegative(height);
    return result;
}

static void divide_panel_budget(int requestedFirst, int requestedSecond,
                                int budget, int* first, int* second) {
    int requestedTotal = requestedFirst + requestedSecond;
    if (!first || !second) return;
    *first = 0;
    *second = 0;
    if (budget <= 0 || requestedTotal <= 0) return;
    if (budget >= requestedTotal) {
        *first = requestedFirst;
        *second = requestedSecond;
        return;
    }
    *first = (int)(((long long)budget * requestedFirst) / requestedTotal);
    *second = budget - *first;
}

void CadDesktopLayoutMetrics_Default(CadDesktopLayoutMetrics* metrics) {
    if (!metrics) return;
    metrics->margin = 4;
    metrics->menuHeight = 20;
    metrics->statusHeight = 22;
    metrics->toolPaletteWidth = 86;
    metrics->coordinatesHeight = 54;
    metrics->animationHeight = 126;
    metrics->minimumViewHeight = 80;
    metrics->floatingAnimationWidth = 900;
}

void CadDesktopLayoutInput_Init(CadDesktopLayoutInput* input,
                                int logicalWidth, int logicalHeight) {
    int view;
    if (!input) return;
    memset(input, 0, sizeof(*input));
    input->logicalWidth = logicalWidth;
    input->logicalHeight = logicalHeight;
    input->dpiScale = 1.0;
    for (view = 0; view < CAD_DESKTOP_VIEW_COUNT; ++view)
        input->viewVisible[view] = 1;
    input->toolPaletteVisible = 1;
    input->coordinatesVisible = 1;
    input->animationVisible = 1;
    input->animationDocked = 1;
    CadDesktopLayoutMetrics_Default(&input->metrics);
}

void CadDesktopLayoutInput_InitPhysical(CadDesktopLayoutInput* input,
                                        int physicalWidth, int physicalHeight,
                                        double dpiScale) {
    if (!input) return;
    CadDesktopLayoutInput_Init(input,
        CadUi_PhysicalToLogical(physicalWidth, dpiScale),
        CadUi_PhysicalToLogical(physicalHeight, dpiScale));
    input->dpiScale = valid_scale(dpiScale);
}

int CadDesktopLayout_Compute(const CadDesktopLayoutInput* input,
                             CadDesktopLayout* output) {
    CadDesktopLayoutMetrics metrics;
    int width;
    int height;
    int margin;
    int menuHeight;
    int statusHeight;
    int workTop;
    int workBottom;
    int workHeight;
    int contentX;
    int contentRight;
    int contentWidth;
    int paletteWidth = 0;
    int panelCount = 0;
    int panelHeightBudget;
    int requestedCoordinates = 0;
    int requestedAnimation = 0;
    int coordinatesHeight = 0;
    int animationHeight = 0;
    int panelCursor;
    int visibleCount = 0;
    int view;

    if (!input || !output || input->logicalWidth <= 0 ||
        input->logicalHeight <= 0) return 0;

    memset(output, 0, sizeof(*output));
    metrics = input->metrics;
    width = input->logicalWidth;
    height = input->logicalHeight;
    margin = clamp_nonnegative(metrics.margin);
    menuHeight = minimum_int(clamp_nonnegative(metrics.menuHeight), height);
    statusHeight = minimum_int(clamp_nonnegative(metrics.statusHeight),
                               height - menuHeight);
    workTop = minimum_int(height, menuHeight + margin);
    workBottom = maximum_int(workTop, height - statusHeight - margin);
    workHeight = workBottom - workTop;

    output->client = make_rect(0, 0, width, height);
    output->menuBar = make_rect(0, 0, width, menuHeight);
    output->statusBar = make_rect(0, height - statusHeight, width,
                                  statusHeight);
    output->workArea = make_rect(margin, workTop,
                                 maximum_int(0, width - margin * 2),
                                 workHeight);
    output->dpiScale = valid_scale(input->dpiScale);

    if (input->toolPaletteVisible && output->workArea.width > 0) {
        int maximumPalette = maximum_int(0, output->workArea.width - margin);
        paletteWidth = minimum_int(clamp_nonnegative(metrics.toolPaletteWidth),
                                   maximumPalette);
        output->toolPalette = make_rect(output->workArea.x, workTop,
                                         paletteWidth, workHeight);
    }

    contentX = output->workArea.x;
    if (paletteWidth > 0) contentX += paletteWidth + margin;
    contentRight = maximum_int(contentX, width - margin);
    contentWidth = contentRight - contentX;

    if (input->coordinatesVisible) {
        requestedCoordinates = clamp_nonnegative(metrics.coordinatesHeight);
        ++panelCount;
    }
    if (input->animationVisible && input->animationDocked) {
        requestedAnimation = clamp_nonnegative(metrics.animationHeight);
        ++panelCount;
    }

    /* Keep some view space when possible.  On very small clients the docked
       panels shrink proportionally rather than escaping the client bounds. */
    panelHeightBudget = workHeight -
        minimum_int(workHeight, clamp_nonnegative(metrics.minimumViewHeight)) -
        panelCount * margin;
    panelHeightBudget = clamp_nonnegative(panelHeightBudget);
    divide_panel_budget(requestedCoordinates, requestedAnimation,
                        panelHeightBudget, &coordinatesHeight,
                        &animationHeight);

    panelCursor = workBottom;
    if (coordinatesHeight > 0) {
        panelCursor -= coordinatesHeight;
        output->coordinatesPanel = make_rect(contentX, panelCursor,
                                              contentWidth,
                                              coordinatesHeight);
        panelCursor = maximum_int(workTop, panelCursor - margin);
    }
    if (animationHeight > 0) {
        panelCursor -= animationHeight;
        if (panelCursor < workTop) panelCursor = workTop;
        output->animationPanel = make_rect(contentX, panelCursor,
                                            contentWidth,
                                            animationHeight);
        panelCursor = maximum_int(workTop, panelCursor - margin);
    }
    output->viewArea = make_rect(contentX, workTop, contentWidth,
                                  panelCursor - workTop);

    if (input->animationVisible) {
        int floatingWidth = minimum_int(contentWidth,
            clamp_nonnegative(metrics.floatingAnimationWidth));
        int floatingHeight = minimum_int(workHeight,
            clamp_nonnegative(metrics.animationHeight));
        output->floatingAnimationDefault = make_rect(
            contentX + (contentWidth - floatingWidth) / 2,
            workBottom - floatingHeight, floatingWidth, floatingHeight);
    }

    for (view = 0; view < CAD_DESKTOP_VIEW_COUNT; ++view)
        if (input->viewVisible[view]) ++visibleCount;

    if (visibleCount > 0 && !CadUiRect_IsEmpty(output->viewArea)) {
        int columns = visibleCount == 1 ? 1 : 2;
        int rows = (visibleCount + columns - 1) / columns;
        int horizontalGap = columns > 1
            ? minimum_int(margin, output->viewArea.width) : 0;
        int verticalGap = rows > 1
            ? minimum_int(margin, output->viewArea.height) : 0;
        int tiledWidth = maximum_int(0, output->viewArea.width -
                                         horizontalGap * (columns - 1));
        int tiledHeight = maximum_int(0, output->viewArea.height -
                                          verticalGap * (rows - 1));
        int slot = 0;
        output->viewColumns = columns;
        output->viewRows = rows;
        for (view = 0; view < CAD_DESKTOP_VIEW_COUNT; ++view) {
            int column;
            int row;
            int x0;
            int x1;
            int y0;
            int y1;
            if (!input->viewVisible[view]) continue;
            column = slot % columns;
            row = slot / columns;
            x0 = output->viewArea.x + horizontalGap * column +
                 (tiledWidth * column) / columns;
            x1 = output->viewArea.x + horizontalGap * column +
                 (tiledWidth * (column + 1)) / columns;
            y0 = output->viewArea.y + verticalGap * row +
                 (tiledHeight * row) / rows;
            y1 = output->viewArea.y + verticalGap * row +
                 (tiledHeight * (row + 1)) / rows;
            output->views[view] = make_rect(x0, y0, x1 - x0, y1 - y0);
            ++slot;
        }
    }
    return 1;
}

int CadUiRect_IsEmpty(CadUiRect rect) {
    return rect.width <= 0 || rect.height <= 0;
}

int CadUiRect_Contains(CadUiRect outer, CadUiRect inner) {
    long long outerRight;
    long long outerBottom;
    long long innerRight;
    long long innerBottom;
    if (CadUiRect_IsEmpty(inner)) return 1;
    if (CadUiRect_IsEmpty(outer)) return 0;
    outerRight = (long long)outer.x + outer.width;
    outerBottom = (long long)outer.y + outer.height;
    innerRight = (long long)inner.x + inner.width;
    innerBottom = (long long)inner.y + inner.height;
    return inner.x >= outer.x && inner.y >= outer.y &&
           innerRight <= outerRight && innerBottom <= outerBottom;
}

int CadUiRect_Intersects(CadUiRect first, CadUiRect second) {
    long long firstRight;
    long long firstBottom;
    long long secondRight;
    long long secondBottom;
    if (CadUiRect_IsEmpty(first) || CadUiRect_IsEmpty(second)) return 0;
    firstRight = (long long)first.x + first.width;
    firstBottom = (long long)first.y + first.height;
    secondRight = (long long)second.x + second.width;
    secondBottom = (long long)second.y + second.height;
    return first.x < secondRight && second.x < firstRight &&
           first.y < secondBottom && second.y < firstBottom;
}

int CadUi_LogicalToPhysical(int logicalPixels, double dpiScale) {
    return rounded_int((double)logicalPixels * valid_scale(dpiScale));
}

int CadUi_PhysicalToLogical(int physicalPixels, double dpiScale) {
    return rounded_int((double)physicalPixels / valid_scale(dpiScale));
}

CadUiRect CadUiRect_LogicalToPhysical(CadUiRect logical, double dpiScale) {
    int left = CadUi_LogicalToPhysical(logical.x, dpiScale);
    int top = CadUi_LogicalToPhysical(logical.y, dpiScale);
    int right = CadUi_LogicalToPhysical(logical.x + logical.width, dpiScale);
    int bottom = CadUi_LogicalToPhysical(logical.y + logical.height, dpiScale);
    return make_rect(left, top, right - left, bottom - top);
}

CadUiRect CadUiRect_PhysicalToLogical(CadUiRect physical, double dpiScale) {
    int left = CadUi_PhysicalToLogical(physical.x, dpiScale);
    int top = CadUi_PhysicalToLogical(physical.y, dpiScale);
    int right = CadUi_PhysicalToLogical(physical.x + physical.width, dpiScale);
    int bottom = CadUi_PhysicalToLogical(physical.y + physical.height,
                                         dpiScale);
    return make_rect(left, top, right - left, bottom - top);
}

CadUiRect CadUiRect_ClampReachable(CadUiRect window, CadUiRect workArea,
                                   int titleHeight, int minimumVisibleWidth) {
    int visibleWidth;
    int visibleTitle;
    int minimumX;
    int maximumX;
    int maximumY;
    if (CadUiRect_IsEmpty(window) || CadUiRect_IsEmpty(workArea)) return window;
    visibleWidth = minimumVisibleWidth;
    if (visibleWidth < 1) visibleWidth = 1;
    if (visibleWidth > window.width) visibleWidth = window.width;
    if (visibleWidth > workArea.width) visibleWidth = workArea.width;
    visibleTitle = titleHeight;
    if (visibleTitle < 1) visibleTitle = 1;
    if (visibleTitle > window.height) visibleTitle = window.height;
    if (visibleTitle > workArea.height) visibleTitle = workArea.height;

    minimumX = workArea.x - window.width + visibleWidth;
    maximumX = workArea.x + workArea.width - visibleWidth;
    if (window.x < minimumX) window.x = minimumX;
    if (window.x > maximumX) window.x = maximumX;
    if (window.y < workArea.y) window.y = workArea.y;
    maximumY = workArea.y + workArea.height - visibleTitle;
    if (window.y > maximumY) window.y = maximumY;
    return window;
}

int CadUiZOrder_Raise(int* order, int count, int windowId) {
    int position = -1;
    int index;
    if (!order || count <= 0) return 0;
    for (index = 0; index < count; ++index) {
        if (order[index] == windowId) {
            position = index;
            break;
        }
    }
    if (position < 0) return 0;
    for (index = position; index + 1 < count; ++index)
        order[index] = order[index + 1];
    order[count - 1] = windowId;
    return 1;
}

int CadUiZOrder_TopmostAt(const int* order, int count,
                          const CadUiRect* rectangles,
                          const unsigned char* visible,
                          int x, int y) {
    int slot;
    if (!order || !rectangles || count <= 0) return -1;
    for (slot = count - 1; slot >= 0; --slot) {
        int id = order[slot];
        const CadUiRect* rectangle;
        long long right;
        long long bottom;
        if (id < 0 || id >= count || (visible && !visible[id])) continue;
        rectangle = &rectangles[id];
        if (CadUiRect_IsEmpty(*rectangle)) continue;
        right = (long long)rectangle->x + rectangle->width;
        bottom = (long long)rectangle->y + rectangle->height;
        if (x >= rectangle->x && y >= rectangle->y &&
            (long long)x < right && (long long)y < bottom)
            return id;
    }
    return -1;
}
