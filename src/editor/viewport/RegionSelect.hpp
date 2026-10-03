#pragma once

#include "editor/interaction/Selection.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <vector>

struct ImDrawList;

// What a box or lasso does to the selection, with Blender's modifiers.
enum class SelectionSetMode : uint8_t {
    // Decided when the gesture ends: Shift extends, Ctrl subtracts, otherwise
    // Set.
    Auto,
    // Selects exactly the entities inside.
    Set,
    // Adds the entities inside.
    Extend,
    // Removes the entities inside.
    Subtract,
};

// Resolves Auto from the modifiers held when the gesture ends; other modes
// are returned unchanged.
SelectionSetMode resolveSelectionSetMode(SelectionSetMode mode, bool shift, bool ctrl);

// Applies the entities a box or lasso enclosed. Set (and Auto) selects
// exactly them, keeping the active entity active when it is among them;
// Extend adds the new ones after the current selection; Subtract removes
// them.
void applySelectionSet(Selection& selection, std::span<const SceneEntityId> entities, SelectionSetMode mode);

// One frame of input to a modal selection tool. Positions are view pixels:
// continuous, origin at the view image's top-left corner.
struct SelectionGestureInput {
    glm::vec2 mouse{0.0f};
    // Whether the mouse is over the view image (not its chrome).
    bool hovered{false};
    bool leftPressed{false};
    bool leftDown{false};
    bool rightPressed{false};
    bool rightDown{false};
    bool escapePressed{false};
    bool shift{false};
    bool ctrl{false};
};

enum class RegionShape : uint8_t {
    // points holds two opposite corners.
    Box,
    // points holds the outline, closed implicitly.
    Lasso,
};

// A finished box or lasso, in view pixels.
struct RegionSelection {
    RegionShape shape{RegionShape::Box};
    std::vector<glm::vec2> points;
    // Never Auto.
    SelectionSetMode mode{SelectionSetMode::Set};
};

// The smallest and largest corner of a region's points.
glm::vec2 regionMinimum(const RegionSelection& region);
glm::vec2 regionMaximum(const RegionSelection& region);
// Whether a view-pixel position lies inside the region (a box includes its
// edges; a lasso uses the even-odd rule).
bool regionContains(const RegionSelection& region, glm::vec2 point);

// Draws a region outline (and a faint fill for a box) over a view image
// whose top-left corner is at screen position origin.
void drawRegionOutline(ImDrawList& drawList, glm::vec2 origin, RegionShape shape, std::span<const glm::vec2> points);
