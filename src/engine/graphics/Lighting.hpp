#pragma once

#include "engine/scene/Scene.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

enum class LocalLightType : uint8_t {
    Point,
    Spot,
};

struct LocalLight {
    LocalLightType type{LocalLightType::Point};
    glm::vec3 position{0.0f};
    glm::vec3 direction{0.0f, -1.0f, 0.0f};
    glm::vec3 color{1.0f};
    float intensity{1.0f};
    float range{1.0f};
    float innerConeCosine{1.0f};
    float outerConeCosine{0.0f};
    uint64_t stableId{0};
};

std::vector<LocalLight> gatherLocalLights(const Scene& scene);
// Keeps at most maximumLights lights, preferring those whose range reaches
// closest to the camera; the result stays in stable-id order.
std::vector<LocalLight> selectLocalLights(std::span<const LocalLight> lights, const glm::vec3& cameraPosition,
                                          uint32_t maximumLights);
// Returns the light with position and direction in the space of transform,
// which must be rigid (ranges are not rescaled).
LocalLight transformLocalLight(const LocalLight& light, const glm::mat4& transform);

// Clustered forward lighting divides the view frustum into a screen-space grid
// of tiles and exponentially spaced view-depth slices. light_clusters.comp
// assigns lights to clusters each frame and mesh.frag shades only its own
// cluster's lights; both mirror the math below.
//
// View depth is the positive distance along the camera's forward axis
// (-z in right-handed view space). The grid covers the internal render extent,
// with pixel (0, 0) at the top-left, matching gl_FragCoord.

// Mirrors MAXIMUM_LIGHTS_PER_CLUSTER in light_clusters.comp.
inline constexpr uint32_t maximumLightsPerCluster = 128;

struct ClusterGridConfiguration {
    uint32_t columns{16};
    uint32_t rows{9};
    uint32_t depthSlices{24};
    // Slices are spaced exponentially up to this view depth (or the far plane
    // when nearer); the last slice then extends to the far plane.
    float maximumSliceDepth{500.0f};
};

// A view-space axis-aligned box.
struct ClusterBounds {
    glm::vec3 minimum{0.0f};
    glm::vec3 maximum{0.0f};
};

struct ClusterGrid {
    ClusterGridConfiguration configuration;
    glm::vec2 renderExtent{1.0f};
    // For a pixel at NDC (x, y): view.x / depth = (x + offset.x) / scale.x,
    // and likewise for y. Taken from projection [0][0], [1][1], [2][0], [2][1].
    glm::vec2 projectionScale{1.0f};
    glm::vec2 projectionOffset{0.0f};
    float nearDepth{0.1f};
    float sliceEndDepth{1.0f};
    float farDepth{1.0f};

    uint32_t clusterCount() const;
    uint32_t clusterIndex(uint32_t column, uint32_t row, uint32_t slice) const;
    // View depth where a slice begins; slice depthSlices yields the far depth.
    float sliceStart(uint32_t slice) const;
    // Shader slice lookup: slice = log(depth) * scale + bias, clamped.
    glm::vec2 sliceScaleBias() const;
    uint32_t sliceIndex(float viewDepth) const;
    uint32_t clusterIndexAt(const glm::vec2& pixel, float viewDepth) const;
    ClusterBounds bounds(uint32_t column, uint32_t row, uint32_t slice) const;
};

// Builds the grid for a perspective projection whose depth terms follow
// DepthConvention.hpp; nearDepth and farDepth are the camera clip distances.
ClusterGrid makeClusterGrid(const ClusterGridConfiguration& configuration, const glm::mat4& projection,
                            const glm::uvec2& renderExtent, float nearDepth, float farDepth);

// Conservative test of a view-space light against a cluster: the light's
// range sphere must touch the box and, for spots, the outer cone must reach
// the box's bounding sphere.
bool localLightAffectsCluster(const LocalLight& viewSpaceLight, const ClusterBounds& bounds);

struct ClusterLightLists {
    // Per cluster: x = first entry in lightIndices, y = light count.
    std::vector<glm::uvec2> ranges;
    std::vector<uint32_t> lightIndices;
    uint32_t maximumLightsInCluster{0};
    // Assignments dropped because a cluster exceeded maximumLightsPerCluster.
    uint32_t discardedAssignments{0};
};

// CPU reference of light_clusters.comp: each cluster owns a fixed block of
// maximumLightsPerCluster entries and lists its lights in ascending order.
ClusterLightLists assignLightsToClusters(const ClusterGrid& grid, std::span<const LocalLight> viewSpaceLights);

struct ProbeBlend {
    Entity entity{entt::null};
    // Share of this probe among the returned probes; the weights sum to one.
    float weight{0.0f};
    // Unnormalized influence at the query point: 1 inside the box, falling to 0
    // across the blend distance.
    float influence{0.0f};
};

struct LocalShadowRequest {
    uint64_t stableId{0};
    LocalLightType type{LocalLightType::Spot};
    uint32_t resolution{256};
    float priority{0.0f};
};

struct LocalShadowTile {
    uint64_t stableId{0};
    uint32_t face{0};
    uint32_t x{0};
    uint32_t y{0};
    uint32_t size{0};
};

struct LocalShadowAtlas {
    uint32_t width{0};
    uint32_t height{0};
    std::vector<LocalShadowTile> tiles;
    uint32_t rejectedLights{0};
};

LocalShadowAtlas allocateLocalShadowAtlas(std::span<const LocalShadowRequest> requests, uint32_t width,
                                          uint32_t height);

std::vector<ProbeBlend> blendReflectionProbes(const Scene& scene, const glm::vec3& worldPosition,
                                              size_t maximumProbes = 4);
