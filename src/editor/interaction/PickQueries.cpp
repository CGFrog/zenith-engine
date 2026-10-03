#include "PickQueries.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <tuple>

namespace {
bool insideGrid(const IdGrid& grid, glm::ivec2 pixel) {
    return pixel.x >= 0 && pixel.y >= 0 && static_cast<uint32_t>(pixel.x) < grid.width &&
           static_cast<uint32_t>(pixel.y) < grid.height;
}

size_t gridIndex(const IdGrid& grid, glm::ivec2 pixel) {
    return static_cast<size_t>(pixel.y) * grid.width + static_cast<size_t>(pixel.x);
}

// Depth used for ordering; without a depth plane every object ties.
float orderingDepth(const IdGrid& grid, size_t index) {
    return grid.depth.empty() ? 0.0f : grid.depth[index];
}

std::vector<uint32_t> sortedUnique(std::vector<uint32_t> ids) {
    std::ranges::sort(ids);
    const auto duplicates = std::ranges::unique(ids);
    ids.erase(duplicates.begin(), duplicates.end());
    return ids;
}

// Even-odd rule: a horizontal ray from the point crosses the outline an odd
// number of times when the point is inside.
bool insidePolygon(std::span<const glm::vec2> polygon, glm::vec2 point) {
    bool inside = false;
    for (size_t current = 0, previous = polygon.size() - 1; current < polygon.size(); previous = current++) {
        const glm::vec2 a = polygon[current];
        const glm::vec2 b = polygon[previous];
        if ((a.y > point.y) != (b.y > point.y)) {
            const float crossingX = a.x + (point.y - a.y) * (b.x - a.x) / (b.y - a.y);
            if (point.x < crossingX) {
                inside = !inside;
            }
        }
    }
    return inside;
}
} // namespace

IdGrid idGrid(const PickResult& result) {
    return {.width = result.region.width, .height = result.region.height, .ids = result.ids, .depth = result.depth};
}

uint32_t idAt(const IdGrid& grid, glm::ivec2 pixel) {
    if (!grid.valid() || !insideGrid(grid, pixel)) {
        return 0;
    }
    return grid.ids[gridIndex(grid, pixel)];
}

std::optional<float> depthAt(const IdGrid& grid, glm::ivec2 pixel) {
    if (grid.depth.empty() || idAt(grid, pixel) == 0) {
        return std::nullopt;
    }
    return grid.depth[gridIndex(grid, pixel)];
}

uint32_t pickPoint(const IdGrid& grid, glm::ivec2 pixel, uint32_t tolerance) {
    if (!grid.valid()) {
        return 0;
    }
    const int32_t radius = static_cast<int32_t>(tolerance);
    const int64_t radiusSquared = static_cast<int64_t>(radius) * radius;
    uint32_t best = 0;
    // (squared distance, negated depth, id): smaller is better.
    std::tuple<int64_t, float, uint32_t> bestKey{std::numeric_limits<int64_t>::max(), 0.0f, 0};
    for (int32_t dy = -radius; dy <= radius; ++dy) {
        for (int32_t dx = -radius; dx <= radius; ++dx) {
            const int64_t distanceSquared = static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dy) * dy;
            const glm::ivec2 sample = pixel + glm::ivec2(dx, dy);
            if (distanceSquared > radiusSquared || !insideGrid(grid, sample)) {
                continue;
            }
            const size_t index = gridIndex(grid, sample);
            const uint32_t id = grid.ids[index];
            if (id == 0) {
                continue;
            }
            const std::tuple key{distanceSquared, -orderingDepth(grid, index), id};
            if (best == 0 || key < bestKey) {
                best = id;
                bestKey = key;
            }
        }
    }
    return best;
}

std::vector<uint32_t> pickBox(const IdGrid& grid, const GridRect& rect) {
    if (!grid.valid()) {
        return {};
    }
    const int64_t left = std::max<int64_t>(rect.x, 0);
    const int64_t top = std::max<int64_t>(rect.y, 0);
    const int64_t right = std::min<int64_t>(static_cast<int64_t>(rect.x) + rect.width, grid.width);
    const int64_t bottom = std::min<int64_t>(static_cast<int64_t>(rect.y) + rect.height, grid.height);
    std::vector<uint32_t> ids;
    for (int64_t y = top; y < bottom; ++y) {
        for (int64_t x = left; x < right; ++x) {
            const uint32_t id = grid.ids[static_cast<size_t>(y) * grid.width + static_cast<size_t>(x)];
            if (id != 0) {
                ids.push_back(id);
            }
        }
    }
    return sortedUnique(std::move(ids));
}

std::vector<uint32_t> pickLasso(const IdGrid& grid, std::span<const glm::vec2> polygon) {
    if (!grid.valid() || polygon.size() < 3) {
        return {};
    }
    glm::vec2 minimum = polygon.front();
    glm::vec2 maximum = polygon.front();
    for (const glm::vec2& point : polygon) {
        minimum = glm::min(minimum, point);
        maximum = glm::max(maximum, point);
    }
    // Pixels whose centres can lie inside the outline's bounds.
    const int64_t left = std::max<int64_t>(static_cast<int64_t>(std::floor(minimum.x - 0.5f)), 0);
    const int64_t top = std::max<int64_t>(static_cast<int64_t>(std::floor(minimum.y - 0.5f)), 0);
    const int64_t right = std::min<int64_t>(static_cast<int64_t>(std::ceil(maximum.x - 0.5f)) + 1, grid.width);
    const int64_t bottom = std::min<int64_t>(static_cast<int64_t>(std::ceil(maximum.y - 0.5f)) + 1, grid.height);
    std::vector<uint32_t> ids;
    for (int64_t y = top; y < bottom; ++y) {
        for (int64_t x = left; x < right; ++x) {
            const uint32_t id = grid.ids[static_cast<size_t>(y) * grid.width + static_cast<size_t>(x)];
            if (id != 0 && insidePolygon(polygon, glm::vec2(static_cast<float>(x), static_cast<float>(y)) + 0.5f)) {
                ids.push_back(id);
            }
        }
    }
    return sortedUnique(std::move(ids));
}

std::vector<uint32_t> pickCandidates(const IdGrid& grid, glm::ivec2 pixel, uint32_t radius) {
    if (!grid.valid()) {
        return {};
    }

    struct Candidate {
        uint32_t id{0};
        int64_t closestDistanceSquared{std::numeric_limits<int64_t>::max()};
        float nearestDepth{0.0f};
        uint32_t coverage{0};
    };

    std::vector<Candidate> candidates;
    const int32_t extent = static_cast<int32_t>(radius);
    const int64_t radiusSquared = static_cast<int64_t>(extent) * extent;
    for (int32_t dy = -extent; dy <= extent; ++dy) {
        for (int32_t dx = -extent; dx <= extent; ++dx) {
            const int64_t distanceSquared = static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dy) * dy;
            const glm::ivec2 sample = pixel + glm::ivec2(dx, dy);
            if (distanceSquared > radiusSquared || !insideGrid(grid, sample)) {
                continue;
            }
            const size_t index = gridIndex(grid, sample);
            const uint32_t id = grid.ids[index];
            if (id == 0) {
                continue;
            }
            auto found = std::ranges::find(candidates, id, &Candidate::id);
            if (found == candidates.end()) {
                candidates.push_back({.id = id, .nearestDepth = orderingDepth(grid, index)});
                found = candidates.end() - 1;
            }
            found->closestDistanceSquared = std::min(found->closestDistanceSquared, distanceSquared);
            found->nearestDepth = std::max(found->nearestDepth, orderingDepth(grid, index));
            ++found->coverage;
        }
    }
    std::ranges::sort(candidates, [](const Candidate& first, const Candidate& second) {
        return std::tuple(first.closestDistanceSquared, -first.nearestDepth, -static_cast<int64_t>(first.coverage),
                          first.id) < std::tuple(second.closestDistanceSquared, -second.nearestDepth,
                                                 -static_cast<int64_t>(second.coverage), second.id);
    });
    std::vector<uint32_t> ordered;
    ordered.reserve(candidates.size());
    for (const Candidate& candidate : candidates) {
        ordered.push_back(candidate.id);
    }
    return ordered;
}

std::vector<SceneEntityId> resolvePickedEntities(std::span<const uint32_t> ids, std::span<const PickedObject> objects) {
    std::vector<SceneEntityId> entities;
    for (uint32_t id : ids) {
        if (id == 0 || id > objects.size()) {
            continue;
        }
        const SceneEntityId entity = objects[id - 1].entity;
        if (entity && std::ranges::find(entities, entity) == entities.end()) {
            entities.push_back(entity);
        }
    }
    return entities;
}

std::vector<SceneEntityId> mergeCycleCandidates(std::span<const SceneEntityId> visible,
                                                std::span<const SceneEntityId> occluded) {
    std::vector<SceneEntityId> merged(visible.begin(), visible.end());
    for (SceneEntityId entity : occluded) {
        if (entity && std::ranges::find(merged, entity) == merged.end()) {
            merged.push_back(entity);
        }
    }
    return merged;
}

std::optional<SceneEntityId> cycleCandidate(std::span<const SceneEntityId> candidates,
                                            std::optional<SceneEntityId> current) {
    if (candidates.empty()) {
        return std::nullopt;
    }
    if (current) {
        const auto found = std::ranges::find(candidates, *current);
        if (found != candidates.end()) {
            const auto next = std::next(found);
            return next == candidates.end() ? candidates.front() : *next;
        }
    }
    return candidates.front();
}
