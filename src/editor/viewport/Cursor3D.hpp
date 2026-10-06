#pragma once

// Blender's 3D cursor: a world point new objects spawn at and transforms can pivot about.

#include "editor/operators/OperatorRegistry.hpp"
#include "editor/viewport/SnapQueries.hpp"

#include <glm/glm.hpp>

#include <vector>

class DebugDraw;
class EditorDocument;
class ViewportRegistry;

// Blender's 3D cursor: a world point new objects spawn at and transforms can
// pivot about. The cursor belongs to the open scene and persists with it
// (EditorDocument::editorData, a sidecar beside the scene file); it is not
// part of undo, as in Blender, but operators that move objects to it are.

// Where a click at viewPoint puts the cursor: the nearest surface under it
// (surfaceSnapPoint), or over empty space the point along the view ray at the
// cursor's current depth from the lens.
glm::vec3 cursorPlacement(const Scene& scene, const AssetManager& assets, const SnapView& view, glm::vec2 viewPoint,
                          const glm::vec3& currentCursor);
// The view depth placementUnderMouse falls back to over empty space: the
// cursor's depth from lens, or a few metres when the cursor is behind it.
float placementDepth(const RenderView& lens, const glm::vec3& currentCursor);

// On-screen radius of the cursor's ring, in view pixels.
inline constexpr float cursorRingRadiusPixels = 10.0f;

// Records the cursor at location into a view's debug list: a ring facing the
// lens with alternating dashes and four crosshair ticks, sized to stay
// cursorRingRadiusPixels on screen and drawn over every surface. Nothing is
// recorded while the cursor is behind the lens.
void recordCursor3D(const glm::vec3& location, const RenderView& lens, glm::vec2 viewSize, DebugDraw& output);

// The cursor operators, acting on document's cursor:
// - view3d.cursor3d_place {x, y}: places the cursor under a point of the
//   active viewport (view pixels; negative uses the mouse) with
//   cursorPlacement. The viewport's Shift+RMB gesture invokes it.
// - view3d.cursor_reset: back to the world origin.
// - view3d.snap_cursor_to_selected: to the median of the selected origins.
// - view3d.snap_selected_to_cursor {offset}: moves each selected object (the
//   transform roots) to the cursor, or with offset the selection as a whole
//   so its median lands there; one undo step.
// They move the cursor of the document the operator runs on, and act only
// through an active viewport showing that document. They need no window, so
// they run headless; viewports must outlive them.
std::vector<Operator> cursorOperators(ViewportRegistry& viewports);

struct EditorOperatorServices;
// This family's entry in editorOperatorFamilies() (EditorOperatorFamilies.hpp).
OperatorRegistrationResult registerCursorOperatorFamily(OperatorRegistry& registry,
                                                        const EditorOperatorServices& services);
