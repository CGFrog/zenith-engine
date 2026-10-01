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

struct LightTileGrid {
    uint32_t columns{0};
    uint32_t rows{0};
    uint32_t maximumLightsPerTile{0};
    std::vector<uint32_t> offsets;
    std::vector<uint32_t> lightIndices;
    uint32_t discardedAssignments{0};

    std::span<const uint32_t> tile(uint32_t x, uint32_t y) const;
};

std::vector<LocalLight> gatherLocalLights(const Scene& scene);
LightTileGrid selectTiledLights(std::span<const LocalLight> lights, const glm::mat4& viewProjection, uint32_t width,
                                uint32_t height, uint32_t tileSize = 16, uint32_t maximumLightsPerTile = 64);
std::vector<uint32_t> selectTiledLightSet(const LightTileGrid& grid, std::span<const LocalLight> lights,
                                          uint32_t maximumLights);

struct ProbeBlend {
    Entity entity{entt::null};
    float weight{0.0f};
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
