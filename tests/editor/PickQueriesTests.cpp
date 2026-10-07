// Selection queries over synthetic id grids (editor/interaction/PickQueries).

#include "editor/interaction/PickQueries.hpp"

#include "support/Doctest.hpp"

#include <array>
#include <iostream>
#include <utility>
#include <vector>

TEST_SUITE_BEGIN("PickQueries");

namespace {

// A width x height grid filled by a callback of (x, y).
struct SyntheticGrid {
    uint32_t width{0};
    uint32_t height{0};
    std::vector<uint32_t> ids;
    std::vector<float> depth;

    template <typename Fill>
    SyntheticGrid(uint32_t gridWidth, uint32_t gridHeight, Fill fill)
        : width(gridWidth)
        , height(gridHeight)
        , ids(static_cast<size_t>(gridWidth) * gridHeight)
        , depth(ids.size()) {
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const auto [id, surfaceDepth] = fill(x, y);
                ids[static_cast<size_t>(y) * width + x] = id;
                depth[static_cast<size_t>(y) * width + x] = id == 0 ? 0.0f : surfaceDepth;
            }
        }
    }

    IdGrid grid() const {
        return {.width = width, .height = height, .ids = ids, .depth = depth};
    }
};

TEST_CASE("PickQueries: point") {
    // Object 1 covers x < 4, object 2 covers x >= 12; columns 4..11 are empty.
    const SyntheticGrid synthetic(16, 8, [](uint32_t x, uint32_t) {
        if (x < 4) {
            return std::pair{1u, 0.5f};
        }
        return x >= 12 ? std::pair{2u, 0.25f} : std::pair{0u, 0.0f};
    });
    const IdGrid grid = synthetic.grid();
    CHECK_MESSAGE(pickPoint(grid, {1, 3}, 0) == 1, "a click on an object picks it");
    CHECK_MESSAGE(pickPoint(grid, {6, 3}, 0) == 0, "a click on empty space without tolerance picks nothing");
    CHECK_MESSAGE(pickPoint(grid, {6, 3}, 2) == 0, "objects beyond the tolerance are not picked");
    CHECK_MESSAGE(pickPoint(grid, {6, 3}, 3) == 1, "tolerance reaches the nearest object");
    CHECK_MESSAGE(pickPoint(grid, {9, 3}, 3) == 2, "tolerance prefers the closer object");
    CHECK_MESSAGE(pickPoint(grid, {-5, 3}, 2) == 0, "a click far outside the grid picks nothing");
    CHECK_MESSAGE(pickPoint(grid, {-1, 3}, 1) == 1, "tolerance reaches into the grid from just outside it");
    CHECK_MESSAGE(idAt(grid, {20, 0}) == 0, "ids outside the grid are empty");
    CHECK_MESSAGE(pickPoint(IdGrid{.width = 4, .height = 4}, {0, 0}, 4) == 0, "an invalid grid picks nothing");

    // Equidistant objects: the nearer surface wins.
    const SyntheticGrid tie(3, 1, [](uint32_t x, uint32_t) {
        if (x == 0) {
            return std::pair{5u, 0.2f};
        }
        return x == 2 ? std::pair{6u, 0.9f} : std::pair{0u, 0.0f};
    });
    CHECK_MESSAGE(pickPoint(tie.grid(), {1, 0}, 1) == 6, "equidistant candidates resolve to the nearer depth");
}

TEST_CASE("PickQueries: depth") {
    const SyntheticGrid synthetic(
        4, 4, [](uint32_t x, uint32_t y) { return x == 1 && y == 2 ? std::pair{3u, 0.75f} : std::pair{0u, 0.0f}; });
    const IdGrid grid = synthetic.grid();
    CHECK_MESSAGE(depthAt(grid, {1, 2}) == 0.75f, "depth is read at a covered pixel");
    CHECK_MESSAGE(!depthAt(grid, {0, 0}), "an empty pixel has no depth");
    IdGrid withoutDepth = grid;
    withoutDepth.depth = {};
    CHECK_MESSAGE(!depthAt(withoutDepth, {1, 2}), "a grid without a depth plane has no depth");
}

TEST_CASE("PickQueries: box") {
    const SyntheticGrid synthetic(8, 8, [](uint32_t x, uint32_t y) {
        if (x < 2 && y < 2) {
            return std::pair{4u, 0.5f};
        }
        if (x >= 6 && y >= 6) {
            return std::pair{2u, 0.5f};
        }
        return x == 4 && y == 4 ? std::pair{9u, 0.5f} : std::pair{0u, 0.0f};
    });
    const IdGrid grid = synthetic.grid();
    CHECK_MESSAGE((pickBox(grid, {.x = 0, .y = 0, .width = 8, .height = 8}) == std::vector<uint32_t>{2, 4, 9}),
                  "a box over the grid returns every id once, ascending");
    CHECK_MESSAGE((pickBox(grid, {.x = 1, .y = 1, .width = 4, .height = 4}) == std::vector<uint32_t>{4, 9}),
                  "a box returns ids touching it");
    CHECK_MESSAGE(pickBox(grid, {.x = 2, .y = 0, .width = 2, .height = 2}).empty(), "an empty box returns nothing");
    CHECK_MESSAGE((pickBox(grid, {.x = -10, .y = -10, .width = 12, .height = 12}) == std::vector<uint32_t>{4}),
                  "a box past the grid edge is clamped");
    CHECK_MESSAGE(pickBox(grid, {.x = 20, .y = 20, .width = 4, .height = 4}).empty(),
                  "a box outside the grid is empty");
}

TEST_CASE("PickQueries: lasso") {
    // Object 1 fills the left column band, object 2 the notch, object 3 the
    // right band of a 10x10 grid.
    const SyntheticGrid synthetic(10, 10, [](uint32_t x, uint32_t y) {
        if (x < 3) {
            return std::pair{1u, 0.5f};
        }
        if (x >= 4 && x < 6 && y < 4) {
            return std::pair{2u, 0.5f};
        }
        return x >= 7 ? std::pair{3u, 0.5f} : std::pair{0u, 0.0f};
    });
    const IdGrid grid = synthetic.grid();
    // A "U" outline: both arms reach the top, the notch between them (where
    // object 2 is) stays outside the concave polygon.
    const std::array<glm::vec2, 8> concave{glm::vec2{0.0f, 0.0f},   glm::vec2{3.5f, 0.0f}, glm::vec2{3.5f, 6.0f},
                                           glm::vec2{6.5f, 6.0f},   glm::vec2{6.5f, 0.0f}, glm::vec2{10.0f, 0.0f},
                                           glm::vec2{10.0f, 10.0f}, glm::vec2{0.0f, 10.0f}};
    CHECK_MESSAGE((pickLasso(grid, concave) == std::vector<uint32_t>{1, 3}), "a concave lasso skips its notch");
    const std::array<glm::vec2, 4> square{glm::vec2{3.0f, 0.0f}, glm::vec2{7.0f, 0.0f}, glm::vec2{7.0f, 5.0f},
                                          glm::vec2{3.0f, 5.0f}};
    CHECK_MESSAGE((pickLasso(grid, square) == std::vector<uint32_t>{2}), "a lasso tests pixel centres");
    const std::array<glm::vec2, 2> line{glm::vec2{0.0f, 0.0f}, glm::vec2{10.0f, 10.0f}};
    CHECK_MESSAGE(pickLasso(grid, line).empty(), "a lasso needs three points");
    // A bow tie: the even-odd rule keeps both lobes.
    const std::array<glm::vec2, 4> bowTie{glm::vec2{0.0f, 0.0f}, glm::vec2{10.0f, 10.0f}, glm::vec2{10.0f, 0.0f},
                                          glm::vec2{0.0f, 10.0f}};
    CHECK_MESSAGE((pickLasso(grid, bowTie) == std::vector<uint32_t>{1, 3}),
                  "a self-intersecting lasso keeps both lobes");
}

TEST_CASE("PickQueries: cycling") {
    // Around the click at (5, 5): object 7 covers the clicked pixel (far),
    // object 8 is two pixels away (near), object 9 is two pixels away (far,
    // larger), object 10 sits outside the radius.
    const SyntheticGrid synthetic(12, 12, [](uint32_t x, uint32_t y) {
        if (x == 5 && y == 5) {
            return std::pair{7u, 0.1f};
        }
        if (x == 7 && y == 5) {
            return std::pair{8u, 0.9f};
        }
        if (x == 3 && (y == 5 || y == 4)) {
            return std::pair{9u, 0.3f};
        }
        return x == 11 && y == 11 ? std::pair{10u, 0.9f} : std::pair{0u, 0.0f};
    });
    const IdGrid grid = synthetic.grid();
    const std::vector<uint32_t> ids = pickCandidates(grid, {5, 5}, 2);
    CHECK_MESSAGE((ids == std::vector<uint32_t>{7, 8, 9}),
                  "candidates order by distance, then nearer depth, within the radius");

    const std::vector<PickedObject> table{
        {.entity = SceneEntityId{100}, .object = 0}, {.entity = SceneEntityId{101}, .object = 1},
        {.entity = SceneEntityId{}, .object = 2},    {.entity = SceneEntityId{103}, .object = 3},
        {.entity = SceneEntityId{104}, .object = 4}, {.entity = SceneEntityId{105}, .object = 5},
        {.entity = SceneEntityId{106}, .object = 6}, {.entity = SceneEntityId{107}, .object = 7},
        {.entity = SceneEntityId{108}, .object = 8}, {.entity = SceneEntityId{109}, .object = 9}};
    const std::vector<SceneEntityId> visible = resolvePickedEntities(ids, table);
    CHECK_MESSAGE((visible == std::vector<SceneEntityId>{{106}, {107}, {108}}),
                  "ids resolve through the table in order");
    const std::array<uint32_t, 5> awkward{0, 3, 1, 99, 1};
    CHECK_MESSAGE((resolvePickedEntities(awkward, table) == std::vector<SceneEntityId>{{100}}),
                  "empty, unknown, id-less, and repeated ids are skipped");

    // The CPU ray adds an occluded entity behind the visible ones.
    const std::array<SceneEntityId, 3> rayHits{SceneEntityId{107}, SceneEntityId{200}, SceneEntityId{106}};
    const std::vector<SceneEntityId> candidates = mergeCycleCandidates(visible, rayHits);
    CHECK_MESSAGE((candidates == std::vector<SceneEntityId>{{106}, {107}, {108}, {200}}),
                  "occluded ray hits follow the visible candidates");
    CHECK_MESSAGE(cycleCandidate(candidates, std::nullopt) == SceneEntityId{106},
                  "without a selection cycling starts first");
    CHECK_MESSAGE(cycleCandidate(candidates, SceneEntityId{106}) == SceneEntityId{107},
                  "cycling steps to the next candidate");
    CHECK_MESSAGE(cycleCandidate(candidates, SceneEntityId{108}) == SceneEntityId{200},
                  "cycling reaches occluded candidates");
    CHECK_MESSAGE(cycleCandidate(candidates, SceneEntityId{200}) == SceneEntityId{106}, "cycling wraps around");
    CHECK_MESSAGE(cycleCandidate(candidates, SceneEntityId{999}) == SceneEntityId{106},
                  "a selection outside the candidates restarts at the first");
    CHECK_MESSAGE(!cycleCandidate({}, SceneEntityId{106}), "no candidates select nothing");
}

TEST_CASE("PickQueries: result grid") {
    PickResult result{.region = {.x = 10, .y = 20, .width = 2, .height = 1}, .ids = {0, 1}, .depth = {0.0f, 0.5f}};
    const IdGrid grid = idGrid(result);
    CHECK_MESSAGE((grid.valid() && grid.width == 2 && grid.height == 1 && idAt(grid, {1, 0}) == 1),
                  "a result's grid covers its region");
    result.depth.clear();
    CHECK_MESSAGE(idGrid(result).valid(), "a result without depth still forms a grid");
}
} // namespace

TEST_SUITE_END();
