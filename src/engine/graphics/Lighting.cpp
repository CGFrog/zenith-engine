#include "Lighting.hpp"

#include <algorithm>
#include <cmath>

std::span<const uint32_t> LightTileGrid::tile(uint32_t x, uint32_t y) const {
    if (x >= columns || y >= rows || offsets.empty()) {
        return {};
    }
    const size_t index = static_cast<size_t>(y) * columns + x;
    return {lightIndices.data() + offsets[index], offsets[index + 1] - offsets[index]};
}

std::vector<LocalLight> gatherLocalLights(const Scene& scene) {
    std::vector<LocalLight> result;
    const auto points = scene.registry().view<const PointLightComponent, const TransformComponent>();
    for (Entity entity : points) {
        const PointLightComponent& source = points.get<const PointLightComponent>(entity);
        const TransformComponent& transform = points.get<const TransformComponent>(entity);
        result.push_back({LocalLightType::Point,
                          glm::vec3(transform.world[3]),
                          {},
                          glm::max(source.color, glm::vec3(0.0f)),
                          std::max(source.intensity, 0.0f),
                          std::max(source.range, 0.001f),
                          1.0f,
                          0.0f,
                          scene.id(entity).value});
    }
    const auto spots = scene.registry().view<const SpotLightComponent, const TransformComponent>();
    for (Entity entity : spots) {
        const SpotLightComponent& source = spots.get<const SpotLightComponent>(entity);
        const TransformComponent& transform = spots.get<const TransformComponent>(entity);
        glm::vec3 direction = glm::mat3(transform.world) * source.direction;
        if (glm::dot(direction, direction) < 0.000001f) {
            direction = {0.0f, -1.0f, 0.0f};
        }
        result.push_back({LocalLightType::Spot, glm::vec3(transform.world[3]), glm::normalize(direction),
                          glm::max(source.color, glm::vec3(0.0f)), std::max(source.intensity, 0.0f),
                          std::max(source.range, 0.001f), std::cos(source.innerConeRadians),
                          std::cos(source.outerConeRadians), scene.id(entity).value});
    }
    std::ranges::sort(result,
                      [](const LocalLight& left, const LocalLight& right) { return left.stableId < right.stableId; });
    return result;
}

LightTileGrid selectTiledLights(std::span<const LocalLight> lights, const glm::mat4& viewProjection, uint32_t width,
                                uint32_t height, uint32_t tileSize, uint32_t maximumLightsPerTile) {
    LightTileGrid grid;
    tileSize = std::max(tileSize, 1u);
    grid.columns = (width + tileSize - 1) / tileSize;
    grid.rows = (height + tileSize - 1) / tileSize;
    grid.maximumLightsPerTile = maximumLightsPerTile;
    std::vector<std::vector<std::pair<float, uint32_t>>> candidates(static_cast<size_t>(grid.columns) * grid.rows);
    for (uint32_t index = 0; index < lights.size(); ++index) {
        const LocalLight& light = lights[index];
        const glm::vec4 clip = viewProjection * glm::vec4(light.position, 1.0f);
        if (clip.w <= 0.0001f) {
            continue;
        }
        const glm::vec2 center = (glm::vec2(clip) / clip.w * 0.5f + 0.5f) * glm::vec2(width, height);
        const float radiusPixels = std::max(1.0f, light.range / clip.w * static_cast<float>(height));
        const int minimumX = std::max(0, static_cast<int>((center.x - radiusPixels) / tileSize));
        const int maximumX =
            std::min(static_cast<int>(grid.columns) - 1, static_cast<int>((center.x + radiusPixels) / tileSize));
        const int minimumY = std::max(0, static_cast<int>((center.y - radiusPixels) / tileSize));
        const int maximumY =
            std::min(static_cast<int>(grid.rows) - 1, static_cast<int>((center.y + radiusPixels) / tileSize));
        const float score = light.intensity / std::max(clip.w * clip.w, 0.001f);
        for (int y = minimumY; y <= maximumY; ++y) {
            for (int x = minimumX; x <= maximumX; ++x) {
                candidates[static_cast<size_t>(y) * grid.columns + x].emplace_back(score, index);
            }
        }
    }
    grid.offsets.reserve(candidates.size() + 1);
    grid.offsets.push_back(0);
    for (auto& tile : candidates) {
        std::ranges::sort(tile, [](const auto& left, const auto& right) {
            return left.first != right.first ? left.first > right.first : left.second < right.second;
        });
        if (tile.size() > maximumLightsPerTile) {
            grid.discardedAssignments += static_cast<uint32_t>(tile.size() - maximumLightsPerTile);
            tile.resize(maximumLightsPerTile);
        }
        for (const auto& [_, index] : tile) {
            grid.lightIndices.push_back(index);
        }
        grid.offsets.push_back(static_cast<uint32_t>(grid.lightIndices.size()));
    }
    return grid;
}

std::vector<uint32_t> selectTiledLightSet(const LightTileGrid& grid, std::span<const LocalLight> lights,
                                          uint32_t maximumLights) {
    struct Candidate {
        uint32_t index{0};
        uint32_t coveredTiles{0};
    };

    std::vector<Candidate> candidates(lights.size());
    for (uint32_t index = 0; index < candidates.size(); ++index) {
        candidates[index].index = index;
    }
    for (uint32_t index : grid.lightIndices) {
        if (index < candidates.size()) {
            ++candidates[index].coveredTiles;
        }
    }
    std::erase_if(candidates, [](const Candidate& candidate) { return candidate.coveredTiles == 0; });
    std::ranges::sort(candidates, [&](const Candidate& left, const Candidate& right) {
        if (left.coveredTiles != right.coveredTiles) {
            return left.coveredTiles > right.coveredTiles;
        }
        const LocalLight& leftLight = lights[left.index];
        const LocalLight& rightLight = lights[right.index];
        if (leftLight.intensity != rightLight.intensity) {
            return leftLight.intensity > rightLight.intensity;
        }
        return leftLight.stableId < rightLight.stableId;
    });
    candidates.resize(std::min<size_t>(candidates.size(), maximumLights));
    std::vector<uint32_t> result;
    result.reserve(candidates.size());
    for (const Candidate& candidate : candidates) {
        result.push_back(candidate.index);
    }
    return result;
}

std::vector<ProbeBlend> blendReflectionProbes(const Scene& scene, const glm::vec3& worldPosition,
                                              size_t maximumProbes) {
    struct Candidate {
        ProbeBlend blend;
        int32_t priority;
    };

    std::vector<Candidate> candidates;
    const auto probes = scene.registry().view<const ReflectionProbeComponent, const TransformComponent>();
    for (Entity entity : probes) {
        const auto& probe = probes.get<const ReflectionProbeComponent>(entity);
        const glm::vec3 center = glm::vec3(probes.get<const TransformComponent>(entity).world[3]);
        const glm::vec3 outside = glm::max(glm::abs(worldPosition - center) - probe.halfExtents, glm::vec3(0.0f));
        const float distance = glm::length(outside);
        if (distance > probe.blendDistance) {
            continue;
        }
        const float weight = probe.blendDistance <= 0.0f ? 1.0f : 1.0f - distance / probe.blendDistance;
        candidates.push_back({{entity, weight}, probe.priority});
    }
    std::ranges::sort(candidates, [](const Candidate& left, const Candidate& right) {
        if (left.priority != right.priority) {
            return left.priority > right.priority;
        }
        if (left.blend.weight != right.blend.weight) {
            return left.blend.weight > right.blend.weight;
        }
        return entt::to_integral(left.blend.entity) < entt::to_integral(right.blend.entity);
    });
    candidates.resize(std::min(maximumProbes, candidates.size()));
    float total = 0.0f;
    for (const Candidate& candidate : candidates) {
        total += candidate.blend.weight;
    }
    std::vector<ProbeBlend> result;
    for (const Candidate& candidate : candidates) {
        result.push_back({candidate.blend.entity, total > 0.0f ? candidate.blend.weight / total : 0.0f});
    }
    return result;
}
