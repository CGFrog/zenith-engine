#pragma once

// Blender's Adjust Last Operation panel: edit the last registered operator's properties and run it again.

#include "editor/operators/OperatorRunner.hpp"

#include <glm/glm.hpp>

#include <optional>

// Blender's Adjust Last Operation panel: a small collapsible window anchored
// to the bottom-left of the viewport that shows the last Register operator's
// properties. Editing one returns the edited properties; the caller re-runs
// the operator with them through OperatorRunner::adjustLast.
class LastOperatorPanel {
  public:
    // anchor is the viewport's bottom-left corner in screen pixels. Draws
    // nothing when last is null or the operator has no properties.
    std::optional<OperatorProperties> draw(const LastOperator* last, glm::vec2 anchor);
};
