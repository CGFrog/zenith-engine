#pragma once

// Model and prefab drops from the Asset Browser onto a viewport image.

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

// Files dropped from the operating system onto the viewport image (at
// imageMin, imageSize, in window coordinates; EditorContext::fileDrops): each
// is imported into the project (ProjectAssets::defaultImportFolder) unless it
// already lies in it, and models and prefabs are then placed on the surface
// under the drop like an Asset Browser drop. Game views must not call it.
void acceptViewportFileDrops(EditorContext& context, const SnapView& view, glm::vec2 imageMin, glm::vec2 imageSize);
