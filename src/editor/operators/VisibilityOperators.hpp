#pragma once

#include "editor/operators/OperatorRegistry.hpp"

// Headless viewport-hide operators, Blender's H / Alt+H / Shift+H. They set
// the editor-only VisibilityComponent::hidden flag, which is saved with the
// scene and undoable but never hides anything in a game or in Play mode.
//
//   object.hide      hide the selected objects (H)
//   object.unhide    reveal every hidden object and select it (Alt+H)
//   object.isolate   hide every object that is not selected (Shift+H)
//
// Hidden objects leave the selection, as they cannot be picked.

struct UnhideProperties {
    // Selects the revealed objects, replacing the selection.
    bool select{true};
};

OperatorRegistrationResult registerVisibilityOperators(OperatorRegistry& registry);
