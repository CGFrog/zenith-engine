#include "Lighting.hpp"
#include "ReflectionProbes.hpp"
#include "engine/scene/SceneVisibility.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

LocalShadowAtlas allocateLocalShadowAtlas(std::span<const LocalShadowRequest> requests, uint32_t width,
                                          uint32_t height) {
    LocalShadowAtlas atlas{.width = width, .height = height};
    std::vector<LocalShadowRequest> ordered(requests.begin(), requests.end());
    std::ranges::sort(ordered, [](const LocalShadowRequest& left, const LocalShadowRequest& right) {
        if (left.priority != right.priority) {
            return left.priority > right.priority;
        }
        return left.stableId < right.stableId;
    });
    uint32_t cursorX = 0;
    uint32_t cursorY = 0;
    uint32_t rowHeight = 0;
    for (const LocalShadowRequest& request : ordered) {
        const uint32_t requested = std::clamp(request.resolution, 64u, 1024u);
        const uint32_t size = std::bit_floor(requested);
        const uint32_t faces = request.type == LocalLightType::Point ? 6u : 1u;
        const uint32_t firstTile = static_cast<uint32_t>(atlas.tiles.size());
        uint32_t savedX = cursorX;
        uint32_t savedY = cursorY;
        uint32_t savedRowHeight = rowHeight;
        bool allocated = true;
        for (uint32_t face = 0; face < faces; ++face) {
            if (cursorX + size > width) {
                cursorX = 0;
                cursorY += rowHeight;
                rowHeight = 0;
            }
            if (cursorY + size > height) {
                allocated = false;
                break;
            }
            atlas.tiles.push_back({request.stableId, face, cursorX, cursorY, size});
            cursorX += size;
            rowHeight = std::max(rowHeight, size);
        }
        if (!allocated) {
            atlas.tiles.resize(firstTile);
            cursorX = savedX;
            cursorY = savedY;
            rowHeight = savedRowHeight;
            ++atlas.rejectedLights;
        }
    }
    return atlas;
}

std::vector<LocalLight> gatherLocalLights(const Scene& scene, SceneVisibilityMode visibilityMode) {
    std::vector<LocalLight> result;
    const SceneVisibility visibility(scene, visibilityMode);
    const auto points = scene.registry().view<const PointLightComponent, const TransformComponent>();
    for (Entity entity : points) {
        if (!visibility.drawn(entity)) {
            continue;
        }
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
                          scene.id(entity).value,
                          source.castsShadows,
                          source.shadowResolution,
                          source.shadowPriority});
    }
    const auto spots = scene.registry().view<const SpotLightComponent, const TransformComponent>();
    for (Entity entity : spots) {
        if (!visibility.drawn(entity)) {
            continue;
        }
        const SpotLightComponent& source = spots.get<const SpotLightComponent>(entity);
        const TransformComponent& transform = spots.get<const TransformComponent>(entity);
        glm::vec3 direction = glm::mat3(transform.world) * source.direction;
        if (glm::dot(direction, direction) < 0.000001f) {
            direction = {0.0f, -1.0f, 0.0f};
        }
        result.push_back({LocalLightType::Spot, glm::vec3(transform.world[3]), glm::normalize(direction),
                          glm::max(source.color, glm::vec3(0.0f)), std::max(source.intensity, 0.0f),
                          std::max(source.range, 0.001f), std::cos(source.innerConeRadians),
                          std::cos(source.outerConeRadians), scene.id(entity).value, source.castsShadows,
                          source.shadowResolution, source.shadowPriority});
    }
    std::ranges::sort(result,
                      [](const LocalLight& left, const LocalLight& right) { return left.stableId < right.stableId; });
    return result;
}

std::vector<LocalLight> selectLocalLights(std::span<const LocalLight> lights, const glm::vec3& cameraPosition,
                                          uint32_t maximumLights) {
    std::vector<LocalLight> selected(lights.begin(), lights.end());
    if (selected.size() <= maximumLights) {
        return selected;
    }
    // Distance from the camera to the light's range sphere; zero inside it.
    const auto reach = [&](const LocalLight& light) {
        return std::max(glm::distance(light.position, cameraPosition) - light.range, 0.0f);
    };
    const auto nearer = [&](const LocalLight& left, const LocalLight& right) {
        const float leftReach = reach(left);
        const float rightReach = reach(right);
        if (leftReach != rightReach) {
            return leftReach < rightReach;
        }
        if (left.intensity != right.intensity) {
            return left.intensity > right.intensity;
        }
        return left.stableId < right.stableId;
    };
    std::ranges::nth_element(selected, selected.begin() + maximumLights, nearer);
    selected.resize(maximumLights);
    std::ranges::sort(selected,
                      [](const LocalLight& left, const LocalLight& right) { return left.stableId < right.stableId; });
    return selected;
}

LocalLight transformLocalLight(const LocalLight& light, const glm::mat4& transform) {
    LocalLight transformed = light;
    transformed.position = glm::vec3(transform * glm::vec4(light.position, 1.0f));
    transformed.direction = glm::mat3(transform) * light.direction;
    return transformed;
}

uint32_t ClusterGrid::clusterCount() const {
    return configuration.columns * configuration.rows * configuration.depthSlices;
}

uint32_t ClusterGrid::clusterIndex(uint32_t column, uint32_t row, uint32_t slice) const {
    return column + configuration.columns * (row + configuration.rows * slice);
}

float ClusterGrid::sliceStart(uint32_t slice) const {
    if (slice >= configuration.depthSlices) {
        return farDepth;
    }
    const float fraction = static_cast<float>(slice) / static_cast<float>(configuration.depthSlices);
    return nearDepth * std::pow(sliceEndDepth / nearDepth, fraction);
}

glm::vec2 ClusterGrid::sliceScaleBias() const {
    const float scale = static_cast<float>(configuration.depthSlices) / std::log(sliceEndDepth / nearDepth);
    return {scale, -std::log(nearDepth) * scale};
}

uint32_t ClusterGrid::sliceIndex(float viewDepth) const {
    const glm::vec2 scaleBias = sliceScaleBias();
    const float slice = std::log(std::max(viewDepth, nearDepth)) * scaleBias.x + scaleBias.y;
    return std::min(static_cast<uint32_t>(std::max(slice, 0.0f)), configuration.depthSlices - 1);
}

uint32_t ClusterGrid::clusterIndexAt(const glm::vec2& pixel, float viewDepth) const {
    const glm::vec2 tile =
        glm::max(pixel, glm::vec2(0.0f)) * glm::vec2(configuration.columns, configuration.rows) / renderExtent;
    const uint32_t column = std::min(static_cast<uint32_t>(tile.x), configuration.columns - 1);
    const uint32_t row = std::min(static_cast<uint32_t>(tile.y), configuration.rows - 1);
    return clusterIndex(column, row, sliceIndex(viewDepth));
}

ClusterBounds ClusterGrid::bounds(uint32_t column, uint32_t row, uint32_t slice) const {
    const glm::vec2 tileSize = glm::vec2(2.0f) / glm::vec2(configuration.columns, configuration.rows);
    const glm::vec2 ndcMinimum = glm::vec2(column, row) * tileSize - 1.0f;
    const float depths[2]{sliceStart(slice), sliceStart(slice + 1)};
    ClusterBounds result{.minimum = glm::vec3(std::numeric_limits<float>::max()),
                         .maximum = glm::vec3(std::numeric_limits<float>::lowest())};
    // A tile's view rays are linear in depth, so its frustum segment is the
    // hull of these eight corners.
    for (uint32_t corner = 0; corner < 4; ++corner) {
        const glm::vec2 ndc = ndcMinimum + glm::vec2(corner & 1u, corner >> 1u) * tileSize;
        const glm::vec2 ray = (ndc + projectionOffset) / projectionScale;
        for (float depth : depths) {
            const glm::vec3 point(ray * depth, -depth);
            result.minimum = glm::min(result.minimum, point);
            result.maximum = glm::max(result.maximum, point);
        }
    }
    return result;
}

ClusterGrid makeClusterGrid(const ClusterGridConfiguration& configuration, const glm::mat4& projection,
                            const glm::uvec2& renderExtent, float nearDepth, float farDepth) {
    ClusterGrid grid{.configuration = configuration};
    grid.configuration.columns = std::max(configuration.columns, 1u);
    grid.configuration.rows = std::max(configuration.rows, 1u);
    grid.configuration.depthSlices = std::max(configuration.depthSlices, 1u);
    grid.renderExtent = glm::max(glm::vec2(renderExtent), glm::vec2(1.0f));
    grid.projectionScale = {projection[0][0], projection[1][1]};
    grid.projectionOffset = {projection[2][0], projection[2][1]};
    grid.nearDepth = std::max(nearDepth, 0.0001f);
    grid.farDepth = std::max(farDepth, grid.nearDepth * 1.001f);
    grid.sliceEndDepth = std::clamp(configuration.maximumSliceDepth, grid.nearDepth * 1.001f, grid.farDepth);
    return grid;
}

bool localLightAffectsCluster(const LocalLight& viewSpaceLight, const ClusterBounds& bounds) {
    const glm::vec3 closest = glm::clamp(viewSpaceLight.position, bounds.minimum, bounds.maximum);
    const glm::vec3 offset = closest - viewSpaceLight.position;
    if (glm::dot(offset, offset) > viewSpaceLight.range * viewSpaceLight.range) {
        return false;
    }
    // Cones wider than a hemisphere keep the sphere test alone.
    const float coneCosine = viewSpaceLight.outerConeCosine;
    if (viewSpaceLight.type != LocalLightType::Spot || coneCosine <= 0.0f) {
        return true;
    }
    const glm::vec3 center = (bounds.minimum + bounds.maximum) * 0.5f;
    const float radius = glm::length(bounds.maximum - bounds.minimum) * 0.5f;
    const glm::vec3 toCenter = center - viewSpaceLight.position;
    const float alongAxis = glm::dot(toCenter, viewSpaceLight.direction);
    const float acrossAxis = std::sqrt(std::max(glm::dot(toCenter, toCenter) - alongAxis * alongAxis, 0.0f));
    const float coneSine = std::sqrt(std::max(1.0f - coneCosine * coneCosine, 0.0f));
    // Signed distance from the bounding sphere's center to the cone surface.
    const float coneDistance = coneCosine * acrossAxis - alongAxis * coneSine;
    return coneDistance <= radius && alongAxis >= -radius;
}

ClusterLightLists assignLightsToClusters(const ClusterGrid& grid, std::span<const LocalLight> viewSpaceLights) {
    ClusterLightLists lists;
    lists.ranges.resize(grid.clusterCount());
    lists.lightIndices.resize(static_cast<size_t>(grid.clusterCount()) * maximumLightsPerCluster);
    const ClusterGridConfiguration& configuration = grid.configuration;
    for (uint32_t slice = 0; slice < configuration.depthSlices; ++slice) {
        for (uint32_t row = 0; row < configuration.rows; ++row) {
            for (uint32_t column = 0; column < configuration.columns; ++column) {
                const uint32_t cluster = grid.clusterIndex(column, row, slice);
                const ClusterBounds bounds = grid.bounds(column, row, slice);
                const uint32_t first = cluster * maximumLightsPerCluster;
                uint32_t affecting = 0;
                for (uint32_t light = 0; light < viewSpaceLights.size(); ++light) {
                    if (!localLightAffectsCluster(viewSpaceLights[light], bounds)) {
                        continue;
                    }
                    if (affecting < maximumLightsPerCluster) {
                        lists.lightIndices[first + affecting] = light;
                    }
                    ++affecting;
                }
                const uint32_t stored = std::min(affecting, maximumLightsPerCluster);
                lists.ranges[cluster] = {first, stored};
                lists.maximumLightsInCluster = std::max(lists.maximumLightsInCluster, affecting);
                lists.discardedAssignments += affecting - stored;
            }
        }
    }
    return lists;
}

std::vector<ProbeBlend> blendReflectionProbes(const Scene& scene, const glm::vec3& worldPosition,
                                              size_t maximumProbes) {
    // Probes arrive in entity order, which breaks ties as documented.
    const std::vector<ReflectionProbeVolume> probes = gatherReflectionProbes(scene);
    std::vector<ProbeBlend> result;
    for (const ReflectionProbeWeight& blend : blendReflectionProbeVolumes(probes, worldPosition, maximumProbes)) {
        result.push_back({probes[blend.probe].entity, blend.weight, blend.influence});
    }
    return result;
}
