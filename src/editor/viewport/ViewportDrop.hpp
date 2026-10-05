#pragma once

#include "editor/viewport/SnapQueries.hpp"

#include <glm/glm.hpp>

struct EditorContext;

// Model and prefab drops from the Asset Browser onto a viewport image. Call
// right after the image item. A drop lands on the surface under the mouse
// (placementUnderMouse, falling back to the 3D cursor's depth over empty
// space) and runs object.place_model or prefab.instantiate with that point
// and the surface normal as properties, so Adjust Last Operation can still
// move it or toggle align_to_normal. Game views must not call it.
void acceptViewportAssetDrops(EditorContext& context, const SnapView& view, glm::vec2 viewPoint);
