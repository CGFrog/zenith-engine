#pragma once

// Reading GPU pick results (id and depth grids) for click, box, and lasso selection.

#include "engine/graphics/RenderPicking.hpp"
#include "engine/scene/SceneEntityId.hpp"

#include <cstdint>
#include <glm/glm.hpp>
#include <optional>
#include <span>
#include <vector>

// Pure selection queries over a GPU pick result (RenderRuntime::requestPick):
// no ImGui, no Vulkan. Queries return pick ids (0 never appears); turn them
// into entities with resolvePickedEntities and the result's object table.
//
// Grid coordinates: pixel (0, 0) is the grid's top-left pixel and its centre is
// (0.5, 0.5); a PickResult's grid starts at its region's top-left view pixel.
//
// Viewport integration contract (Wave 2):
// - Click: request a (2r + 1)-pixel square around the cursor; on Ready, the
//   hit is pickPoint(grid, cursor - region origin, r).
// - Box (B) and lasso: request the drag's bounding rectangle; pickBox or
//   pickLasso with the shape translated by the region origin.
// - Alt+click cycling: an id buffer stores only the front-most object per
//   pixel, so candidates come from two sources: pickCandidates over the
//   clicked neighbourhood (visible objects, ordered by distance from the click,
//   then nearer depth, then coverage), followed by the objects a CPU ray hits
//   behind them in ray order (a ray-list variant of pickThroughLens), merged
//   by mergeCycleCandidates. cycleCandidate then steps from the active
//   selection to the next candidate, wrapping, so repeated Alt+clicks walk
//   front to back.
// - Surface snapping: pickWorldPosition (RenderPicking.hpp) turns a pixel's
//   depth into a world position; depthAt reads the raw depth.
// - Without a GPU result (headless runs) the editor keeps pickThroughLens.

// A view of a pick result's id and depth planes.
struct IdGrid {
    uint32_t width{0};
    uint32_t height{0};
    // width * height ids, row by row; 0 where no object was drawn.
    std::span<const uint32_t> ids;
    // Empty, or width * height reversed-Z depths (larger is nearer).
    std::span<const float> depth;

    bool valid() const {
        return ids.size() == static_cast<size_t>(width) * height && (depth.empty() || depth.size() == ids.size());
    }
};

// A rectangle of grid pixels; may extend past the grid.
struct GridRect {
    int32_t x{0};
    int32_t y{0};
    uint32_t width{0};
    uint32_t height{0};
};

// The grid of a finished pick; it references the result's vectors.
IdGrid idGrid(const PickResult& result);

// The id at a pixel; 0 outside the grid or where nothing was drawn.
uint32_t idAt(const IdGrid& grid, glm::ivec2 pixel);
// Depth of the object drawn at a pixel; empty where none was drawn or the
// grid has no depth.
std::optional<float> depthAt(const IdGrid& grid, glm::ivec2 pixel);

// The object under `pixel`, or else the one whose pixel centre lies closest
// within `tolerance` pixels (ties: nearer depth, then lower id). 0 for none.
uint32_t pickPoint(const IdGrid& grid, glm::ivec2 pixel, uint32_t tolerance);
// Distinct ids with any pixel inside the rectangle, ascending.
std::vector<uint32_t> pickBox(const IdGrid& grid, const GridRect& rect);
// Distinct ids with a pixel centre inside the polygon (even-odd rule, so
// concave and self-intersecting outlines work), ascending. Needs three points.
std::vector<uint32_t> pickLasso(const IdGrid& grid, std::span<const glm::vec2> polygon);
// Whether point lies inside the closed outline polygon by the even-odd rule;
// false for fewer than three points.
bool insidePolygon(std::span<const glm::vec2> polygon, glm::vec2 point);
// Distinct ids within `radius` pixels of `pixel`, ordered for Alt+click
// cycling: closest pixel to the click first, then nearer depth, then more
// covered pixels, then lower id.
std::vector<uint32_t> pickCandidates(const IdGrid& grid, glm::ivec2 pixel, uint32_t radius);

// Entities of pick ids through a result's table (id n is objects[n-1]), in
// order. Ids outside the table and objects without a stable id are skipped,
// and repeated entities keep their first position.
std::vector<SceneEntityId> resolvePickedEntities(std::span<const uint32_t> ids, std::span<const PickedObject> objects);
// `visible` followed by the entities of `occluded` it does not already hold.
std::vector<SceneEntityId> mergeCycleCandidates(std::span<const SceneEntityId> visible,
                                                std::span<const SceneEntityId> occluded);
// The candidate after `current` (wrapping), or the first when `current` is
// empty or not a candidate. Empty without candidates.
std::optional<SceneEntityId> cycleCandidate(std::span<const SceneEntityId> candidates,
                                            std::optional<SceneEntityId> current);
