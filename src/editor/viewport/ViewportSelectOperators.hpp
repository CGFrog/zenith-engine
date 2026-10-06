#pragma once

// Viewport selection and local view operators (view3d.select, select_box, select_lasso, select_cycle, localview).

#include "editor/operators/Operator.hpp"

#include <vector>

class ViewportRegistry;

// The viewport selection and local view operators, acting on the registry's
// active viewport (polling false while none is open):
// - view3d.select / view3d.select_cycle: a click pick at x, y (view pixels;
//   negative uses the mouse), extend adding to the selection. The viewport's
//   next draw resolves it (ViewportPicker) and applies it to the selection
//   or the armed eyedropper. Internal: the viewport invokes them for a left
//   click (Alt cycles).
// - view3d.select_box (B) and view3d.select_lasso (the viewport's Ctrl+RMB
//   gesture): start the modal box or lasso tool with a mode of auto, set,
//   extend, or subtract; right_drag draws the lasso while the right button
//   is held.
// - view3d.localview (Numpad /): isolates the selection in the viewport and
//   frames it, or leaves local view and restores the camera.
// They need no window, so they run headless; the registry must outlive them.
std::vector<Operator> viewportSelectOperators(ViewportRegistry& viewports);
