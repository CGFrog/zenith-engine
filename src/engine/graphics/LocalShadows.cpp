#include "LocalShadows.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <tuple>

namespace {
// Smallest tile the allocator accepts.
constexpr uint32_t minimumTileResolution = 64;
// Widens every tile's field of view so percentage-closer filtering near the
// edge of a spot cone or a cube face still reads inside the tile.
constexpr float fieldOfViewMargin = 1.05f;
// Wide spot cones are clamped to an 85-degree half angle; a perspective tile
// cannot cover a hemisphere.
constexpr float maximumTanHalfFov = 11.43f;
// Projection entries are compared with this tolerance to detect light motion.
constexpr float projectionTolerance = 0.0001f;

// Distance from the camera to the light's range sphere; zero inside it.
float cameraReach(const glm::vec3& lightPosition, float range, const glm::vec3& cameraPosition) {
    return std::max(glm::distance(lightPosition, cameraPosition) - range, 0.0f);
}

float nearPlane(const LocalLight& light) {
    return std::clamp(light.range * 0.01f, 0.01f, 0.5f);
}

bool projectionChanged(const glm::mat4& left, const glm::mat4& right) {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (std::abs(left[column][row] - right[column][row]) > projectionTolerance) {
                return true;
            }
        }
    }
    return false;
}

bool castersTouchLight(std::span<const ShadowCasterBounds> changedCasters, const glm::vec3& lightPosition,
                       float range) {
    return std::ranges::any_of(changedCasters, [&](const ShadowCasterBounds& caster) {
        return glm::distance(caster.center, lightPosition) <= caster.radius + range;
    });
}
} // namespace

LocalShadowProfile localShadowProfile(LocalShadowQuality quality) {
    switch (quality) {
    case LocalShadowQuality::Off:
        return {};
    case LocalShadowQuality::Low:
        return {.atlasSize = 2048,
                .spotResolution = 256,
                .pointResolution = 128,
                .maximumResolution = 512,
                .maximumLights = 8,
                .updateBudgetTiles = 6};
    case LocalShadowQuality::Medium:
        return {.atlasSize = 4096,
                .spotResolution = 512,
                .pointResolution = 256,
                .maximumResolution = 1024,
                .maximumLights = 16,
                .updateBudgetTiles = 12};
    case LocalShadowQuality::High:
        return {.atlasSize = 4096,
                .spotResolution = 1024,
                .pointResolution = 512,
                .maximumResolution = 1024,
                .maximumLights = 24,
                .updateBudgetTiles = 24};
    }
    return {};
}

uint32_t localShadowFaceCount(LocalLightType type) {
    return type == LocalLightType::Point ? pointShadowFaceCount : 1u;
}

std::vector<LocalShadowRequest> buildLocalShadowRequests(std::span<const LocalLight> lights,
                                                         const glm::vec3& cameraPosition,
                                                         const LocalShadowProfile& profile) {
    if (profile.atlasSize == 0 || profile.maximumLights == 0) {
        return {};
    }
    std::vector<const LocalLight*> casters;
    for (const LocalLight& light : lights) {
        if (light.castsShadows && light.range > 0.0f) {
            casters.push_back(&light);
        }
    }
    const auto moreImportant = [&](const LocalLight* left, const LocalLight* right) {
        const float leftReach = cameraReach(left->position, left->range, cameraPosition);
        const float rightReach = cameraReach(right->position, right->range, cameraPosition);
        return std::tuple(-left->shadowPriority, leftReach, left->stableId) <
               std::tuple(-right->shadowPriority, rightReach, right->stableId);
    };
    if (casters.size() > profile.maximumLights) {
        std::ranges::nth_element(casters, casters.begin() + profile.maximumLights, moreImportant);
        casters.resize(profile.maximumLights);
    }
    const auto resolution = [&](const LocalLight& light) {
        if (light.shadowResolution != 0) {
            return std::clamp(light.shadowResolution, minimumTileResolution,
                              std::max(profile.maximumResolution, minimumTileResolution));
        }
        return light.type == LocalLightType::Spot ? profile.spotResolution : profile.pointResolution;
    };
    std::ranges::sort(casters, [&](const LocalLight* left, const LocalLight* right) {
        const bool leftPoint = left->type == LocalLightType::Point;
        const bool rightPoint = right->type == LocalLightType::Point;
        return std::tuple(leftPoint, -left->shadowPriority, -static_cast<int64_t>(resolution(*left)), left->stableId) <
               std::tuple(rightPoint, -right->shadowPriority, -static_cast<int64_t>(resolution(*right)),
                          right->stableId);
    });
    std::vector<LocalShadowRequest> requests;
    requests.reserve(casters.size());
    for (size_t index = 0; index < casters.size(); ++index) {
        const LocalLight& light = *casters[index];
        requests.push_back({.stableId = light.stableId,
                            .type = light.type,
                            .resolution = resolution(light),
                            .priority = static_cast<float>(casters.size() - index)});
    }
    return requests;
}

uint32_t localShadowFace(const glm::vec3& lightToPoint) {
    const glm::vec3 magnitude = glm::abs(lightToPoint);
    if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z) {
        return lightToPoint.x >= 0.0f ? 0u : 1u;
    }
    if (magnitude.y >= magnitude.z) {
        return lightToPoint.y >= 0.0f ? 2u : 3u;
    }
    return lightToPoint.z >= 0.0f ? 4u : 5u;
}

float localShadowTanHalfFov(const LocalLight& light) {
    if (light.type == LocalLightType::Point) {
        return fieldOfViewMargin;
    }
    const float coneCosine = std::clamp(light.outerConeCosine, -1.0f, 1.0f);
    const float coneSine = std::sqrt(std::max(1.0f - coneCosine * coneCosine, 0.0f));
    if (coneCosine <= 0.0f) {
        return maximumTanHalfFov;
    }
    // At least a one-degree cone keeps the projection well conditioned.
    const float tangent = std::max(coneSine / coneCosine, 0.0175f);
    return std::min(tangent * fieldOfViewMargin, maximumTanHalfFov);
}

glm::mat4 localShadowViewProjection(const LocalLight& light, uint32_t face) {
    static const std::array<glm::vec3, pointShadowFaceCount> faceForward{{{1.0f, 0.0f, 0.0f},
                                                                          {-1.0f, 0.0f, 0.0f},
                                                                          {0.0f, 1.0f, 0.0f},
                                                                          {0.0f, -1.0f, 0.0f},
                                                                          {0.0f, 0.0f, 1.0f},
                                                                          {0.0f, 0.0f, -1.0f}}};
    glm::vec3 forward = light.direction;
    if (light.type == LocalLightType::Point) {
        forward = faceForward[std::min(face, pointShadowFaceCount - 1)];
    } else if (glm::dot(forward, forward) < 0.000001f) {
        forward = {0.0f, -1.0f, 0.0f};
    }
    forward = glm::normalize(forward);
    const glm::vec3 up = std::abs(forward.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::mat4 view = glm::lookAtRH(light.position, light.position + forward, up);
    const float nearDepth = nearPlane(light);
    const float farDepth = std::max(light.range, nearDepth * 2.0f);
    glm::mat4 projection =
        glm::perspectiveRH_ZO(2.0f * std::atan(localShadowTanHalfFov(light)), 1.0f, nearDepth, farDepth);
    projection[1][1] *= -1.0f;
    return projection * view;
}

void LocalShadowCache::synchronize(const LocalShadowAtlas& atlas, std::span<const LocalLight> lights,
                                   std::span<const ShadowCasterBounds> changedCasters, bool invalidateAll) {
    std::unordered_map<uint64_t, const LocalLight*> lightsById;
    lightsById.reserve(lights.size());
    for (const LocalLight& light : lights) {
        lightsById.emplace(light.stableId, &light);
    }
    std::vector<CachedLocalShadowTile> next;
    next.reserve(atlas.tiles.size());
    std::unordered_map<uint64_t, uint32_t> nextFirstTiles;
    for (const LocalShadowTile& tile : atlas.tiles) {
        const auto light = lightsById.find(tile.stableId);
        if (light == lightsById.end()) {
            continue;
        }
        const LocalLight& source = *light->second;
        CachedLocalShadowTile entry{.tile = tile,
                                    .type = source.type,
                                    .lightPosition = source.position,
                                    .lightRange = source.range,
                                    .tanHalfFov = localShadowTanHalfFov(source),
                                    .viewProjection = localShadowViewProjection(source, tile.face)};
        if (tile.face == 0) {
            nextFirstTiles[tile.stableId] = static_cast<uint32_t>(next.size());
        }
        const auto previousFirst = m_firstTiles.find(tile.stableId);
        const size_t previousIndex =
            previousFirst == m_firstTiles.end() ? m_tiles.size() : size_t{previousFirst->second} + tile.face;
        if (previousIndex < m_tiles.size()) {
            const CachedLocalShadowTile& previous = m_tiles[previousIndex];
            const bool samePlacement = previous.tile.stableId == tile.stableId && previous.tile.face == tile.face &&
                                       previous.tile.x == tile.x && previous.tile.y == tile.y &&
                                       previous.tile.size == tile.size && previous.type == source.type;
            if (samePlacement && previous.rendered) {
                entry.rendered = true;
                entry.renderedViewProjection = previous.renderedViewProjection;
                entry.stale = previous.stale || invalidateAll ||
                              projectionChanged(entry.viewProjection, previous.renderedViewProjection) ||
                              castersTouchLight(changedCasters, source.position, source.range);
            }
        }
        next.push_back(entry);
    }
    m_tiles = std::move(next);
    m_firstTiles = std::move(nextFirstTiles);
}

std::vector<uint64_t> LocalShadowCache::scheduleUpdates(const glm::vec3& cameraPosition, uint32_t budgetTiles) const {
    struct Candidate {
        uint64_t stableId;
        bool unrendered;
        float reach;
        uint32_t tiles;
    };

    std::vector<Candidate> candidates;
    for (const auto& [stableId, first] : m_firstTiles) {
        const uint32_t faces = localShadowFaceCount(m_tiles[first].type);
        bool unrendered = false;
        bool stale = false;
        for (uint32_t face = 0; face < faces && first + face < m_tiles.size(); ++face) {
            unrendered |= !m_tiles[first + face].rendered;
            stale |= m_tiles[first + face].stale;
        }
        if (unrendered || stale) {
            const CachedLocalShadowTile& tile = m_tiles[first];
            candidates.push_back(
                {stableId, unrendered, cameraReach(tile.lightPosition, tile.lightRange, cameraPosition), faces});
        }
    }
    std::ranges::sort(candidates, [](const Candidate& left, const Candidate& right) {
        return std::tuple(!left.unrendered, left.reach, left.stableId) <
               std::tuple(!right.unrendered, right.reach, right.stableId);
    });
    std::vector<uint64_t> scheduled;
    uint32_t remaining = budgetTiles;
    for (const Candidate& candidate : candidates) {
        if (candidate.tiles <= remaining) {
            remaining -= candidate.tiles;
            scheduled.push_back(candidate.stableId);
        } else if (scheduled.empty()) {
            remaining = 0;
            scheduled.push_back(candidate.stableId);
        }
    }
    return scheduled;
}

void LocalShadowCache::markRendered(uint64_t stableId) {
    const std::optional<uint32_t> first = firstTile(stableId);
    if (!first) {
        return;
    }
    const uint32_t faces = localShadowFaceCount(m_tiles[*first].type);
    for (uint32_t face = 0; face < faces && *first + face < m_tiles.size(); ++face) {
        CachedLocalShadowTile& tile = m_tiles[*first + face];
        tile.rendered = true;
        tile.stale = false;
        tile.renderedViewProjection = tile.viewProjection;
    }
}

void LocalShadowCache::invalidate(uint64_t stableId) {
    const std::optional<uint32_t> first = firstTile(stableId);
    if (!first) {
        return;
    }
    const uint32_t faces = localShadowFaceCount(m_tiles[*first].type);
    for (uint32_t face = 0; face < faces && *first + face < m_tiles.size(); ++face) {
        m_tiles[*first + face].rendered = false;
        m_tiles[*first + face].stale = false;
    }
}

std::optional<uint32_t> LocalShadowCache::firstTile(uint64_t stableId) const {
    const auto found = m_firstTiles.find(stableId);
    if (found == m_firstTiles.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<uint32_t> LocalShadowCache::shadowTile(uint64_t stableId) const {
    const std::optional<uint32_t> first = firstTile(stableId);
    if (!first) {
        return std::nullopt;
    }
    const uint32_t faces = localShadowFaceCount(m_tiles[*first].type);
    if (*first + faces > m_tiles.size()) {
        return std::nullopt;
    }
    for (uint32_t face = 0; face < faces; ++face) {
        if (!m_tiles[*first + face].rendered || m_tiles[*first + face].tile.stableId != stableId) {
            return std::nullopt;
        }
    }
    return first;
}

uint32_t LocalShadowCache::pendingTileCount() const {
    return static_cast<uint32_t>(
        std::ranges::count_if(m_tiles, [](const CachedLocalShadowTile& tile) { return !tile.rendered || tile.stale; }));
}

void LocalShadowCache::clear() {
    m_tiles.clear();
    m_firstTiles.clear();
}
