#include "Decals.hpp"

#include "engine/scene/SceneVisibility.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>

namespace {
// Smallest cosine span of the angle fade, so equal start and end angles give a
// hard edge instead of a division by zero; mirrored by the packed scale.
constexpr float minimumAngleFadeSpan = 1.0e-4f;

bool finite(const glm::mat4& matrix) {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (!std::isfinite(matrix[column][row])) {
                return false;
            }
        }
    }
    return true;
}

glm::mat4 decalToWorld(const DecalProjector& decal) {
    return glm::inverse(decal.worldToDecal);
}
} // namespace

std::optional<DecalProjector> makeDecalProjector(const DecalComponent& decal, const glm::mat4& world) {
    glm::mat4 boxToWorld = world;
    for (int axis = 0; axis < 3; ++axis) {
        boxToWorld[axis] *= decal.halfExtents[axis];
    }
    const glm::mat3 axes(boxToWorld);
    const float determinant = glm::determinant(axes);
    if (!finite(boxToWorld) || !std::isfinite(determinant) || std::abs(determinant) < 1.0e-12f) {
        return std::nullopt;
    }
    const float startCosine = std::cos(std::clamp(decal.angleFadeStartRadians, 0.0f, glm::pi<float>()));
    const float endCosine = std::cos(std::clamp(decal.angleFadeEndRadians, 0.0f, glm::pi<float>()));
    const float scale = 1.0f / std::max(startCosine - endCosine, minimumAngleFadeSpan);
    return DecalProjector{.worldToDecal = glm::inverse(boxToWorld),
                          .center = glm::vec3(boxToWorld[3]),
                          .boundingRadius = glm::length(axes[0]) + glm::length(axes[1]) + glm::length(axes[2]),
                          .sortOrder = decal.sortOrder,
                          .albedoTexture = decal.albedoTexture,
                          .normalTexture = decal.normalTexture,
                          .tint = glm::max(decal.tint, glm::vec3(0.0f)),
                          .opacity = std::clamp(decal.opacity, 0.0f, 1.0f),
                          .baseColorStrength = std::clamp(decal.baseColorStrength, 0.0f, 1.0f),
                          .normalStrength = std::clamp(decal.normalStrength, 0.0f, 1.0f),
                          .roughness = std::clamp(decal.roughness, 0.0f, 1.0f),
                          .roughnessStrength = std::clamp(decal.roughnessStrength, 0.0f, 1.0f),
                          .metallic = std::clamp(decal.metallic, 0.0f, 1.0f),
                          .metallicStrength = std::clamp(decal.metallicStrength, 0.0f, 1.0f),
                          .angleFadeScale = scale,
                          .angleFadeBias = -endCosine * scale};
}

std::vector<DecalProjector> gatherDecals(const Scene& scene, SceneVisibilityMode visibilityMode) {
    // Ties break on the stable scene id, so layering survives save and reload.
    std::vector<std::pair<SceneEntityId, DecalProjector>> ordered;
    const SceneVisibility visibility(scene, visibilityMode);
    const auto decals = scene.registry().view<const DecalComponent, const TransformComponent>();
    for (Entity entity : decals) {
        if (!visibility.drawn(entity)) {
            continue;
        }
        std::optional<DecalProjector> projector = makeDecalProjector(
            decals.get<const DecalComponent>(entity), decals.get<const TransformComponent>(entity).world);
        if (projector) {
            projector->entity = entity;
            ordered.emplace_back(scene.id(entity), *projector);
        }
    }
    std::ranges::sort(ordered, [](const auto& left, const auto& right) {
        if (left.second.sortOrder != right.second.sortOrder) {
            return left.second.sortOrder < right.second.sortOrder;
        }
        return left.first < right.first;
    });
    std::vector<DecalProjector> result;
    result.reserve(ordered.size());
    for (const auto& entry : ordered) {
        result.push_back(entry.second);
    }
    return result;
}

std::vector<DecalProjector> selectDecals(std::span<const DecalProjector> decals, const glm::vec3& cameraPosition,
                                         uint32_t maximumCount) {
    if (decals.size() <= maximumCount) {
        return {decals.begin(), decals.end()};
    }
    std::vector<uint32_t> order(decals.size());
    std::iota(order.begin(), order.end(), 0u);
    const auto distance = [&](uint32_t index) {
        return std::max(glm::distance(decals[index].center, cameraPosition) - decals[index].boundingRadius, 0.0f);
    };
    std::ranges::nth_element(order, order.begin() + maximumCount, [&](uint32_t left, uint32_t right) {
        const float leftDistance = distance(left);
        const float rightDistance = distance(right);
        return leftDistance != rightDistance ? leftDistance < rightDistance : left < right;
    });
    order.resize(maximumCount);
    std::ranges::sort(order);
    std::vector<DecalProjector> result;
    result.reserve(order.size());
    for (uint32_t index : order) {
        result.push_back(decals[index]);
    }
    return result;
}

glm::vec3 decalSpacePosition(const DecalProjector& decal, const glm::vec3& worldPosition) {
    return glm::vec3(decal.worldToDecal * glm::vec4(worldPosition, 1.0f));
}

std::optional<glm::vec2> decalUv(const DecalProjector& decal, const glm::vec3& worldPosition) {
    const glm::vec3 local = decalSpacePosition(decal, worldPosition);
    if (glm::any(glm::greaterThan(glm::abs(local), glm::vec3(1.0f)))) {
        return std::nullopt;
    }
    return glm::vec2(local.x, local.z) * 0.5f + 0.5f;
}

glm::vec3 decalFacingAxis(const DecalProjector& decal) {
    // The gradient of decal-space y: row 1 of the world-to-decal matrix.
    const glm::mat4& matrix = decal.worldToDecal;
    return glm::normalize(glm::vec3(matrix[0][1], matrix[1][1], matrix[2][1]));
}

float decalAngleFade(const DecalProjector& decal, const glm::vec3& worldNormal) {
    const float cosine = glm::dot(glm::normalize(worldNormal), decalFacingAxis(decal));
    return std::clamp(cosine * decal.angleFadeScale + decal.angleFadeBias, 0.0f, 1.0f);
}

float decalDepthFade(float decalSpaceY) {
    return std::clamp((1.0f - std::abs(decalSpaceY)) / decalDepthFadeFraction, 0.0f, 1.0f);
}

float decalCoverage(const DecalProjector& decal, const glm::vec3& worldPosition, const glm::vec3& worldNormal) {
    const glm::vec3 local = decalSpacePosition(decal, worldPosition);
    if (glm::any(glm::greaterThan(glm::abs(local), glm::vec3(1.0f)))) {
        return 0.0f;
    }
    return decal.opacity * decalAngleFade(decal, worldNormal) * decalDepthFade(local.y);
}

ClusterBounds decalViewBounds(const DecalProjector& decal, const glm::mat4& view) {
    const glm::mat4 boxToView = view * decalToWorld(decal);
    const glm::vec3 center(boxToView[3]);
    // Each view axis spans the absolute projection of the three box axes.
    glm::vec3 extents{0.0f};
    for (int axis = 0; axis < 3; ++axis) {
        extents += glm::abs(glm::vec3(boxToView[axis]));
    }
    return {.minimum = center - extents, .maximum = center + extents};
}

bool decalAffectsCluster(const ClusterBounds& decalViewBounds, const ClusterBounds& cluster) {
    return glm::all(glm::lessThanEqual(decalViewBounds.minimum, cluster.maximum)) &&
           glm::all(glm::lessThanEqual(cluster.minimum, decalViewBounds.maximum));
}

ClusterDecalLists assignDecalsToClusters(const ClusterGrid& grid, std::span<const ClusterBounds> decalViewBounds) {
    const uint32_t clusterCount = grid.clusterCount();
    ClusterDecalLists lists{.counts = std::vector<uint32_t>(clusterCount, 0u),
                            .indices = std::vector<uint32_t>(size_t{clusterCount} * maximumDecalsPerCluster, 0u)};
    const ClusterGridConfiguration& configuration = grid.configuration;
    const size_t decalCount = std::min<size_t>(decalViewBounds.size(), maximumDecals);
    for (uint32_t slice = 0; slice < configuration.depthSlices; ++slice) {
        for (uint32_t row = 0; row < configuration.rows; ++row) {
            for (uint32_t column = 0; column < configuration.columns; ++column) {
                const ClusterBounds bounds = grid.bounds(column, row, slice);
                const uint32_t cluster = grid.clusterIndex(column, row, slice);
                uint32_t affecting = 0;
                for (uint32_t decal = 0; decal < decalCount; ++decal) {
                    if (!decalAffectsCluster(decalViewBounds[decal], bounds)) {
                        continue;
                    }
                    if (affecting < maximumDecalsPerCluster) {
                        lists.indices[size_t{cluster} * maximumDecalsPerCluster + affecting] = decal;
                    }
                    ++affecting;
                }
                lists.counts[cluster] = std::min(affecting, maximumDecalsPerCluster);
                lists.maximumDecalsInCluster = std::max(lists.maximumDecalsInCluster, affecting);
                lists.discardedAssignments += affecting - lists.counts[cluster];
            }
        }
    }
    return lists;
}
