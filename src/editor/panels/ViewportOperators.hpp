#pragma once

// The view3d.* operators, acting on the active viewport: chrome, shading, snapping, pivot, viewpoints, framing.

#include "editor/operators/OperatorRegistry.hpp"

#include <vector>

class ViewportRegistry;

// The view3d operators, each acting on the registry's active viewport when it
// runs: the chrome toggles and shading (viewportChromeOperators), snapping
// and the pivot point (transformToolOperators),
// the numpad viewpoints and orbit steps, framing, the camera view toggle, and
// the selection and local view operators (viewportSelectOperators).
// They poll false while no viewport is open and fail with a descriptive
// message if run anyway. Navigation operators leave a camera view first, as
// in Blender. They need no window, so they also run headless; the registry
// must outlive them.
std::vector<Operator> viewportOperators(ViewportRegistry& viewports);

struct EditorOperatorServices;
// This family's entry in editorOperatorFamilies() (EditorOperatorFamilies.hpp).
OperatorRegistrationResult registerViewportOperatorFamily(OperatorRegistry& registry,
                                                          const EditorOperatorServices& services);
