#include "MeshLodSelection.hpp"

#include <algorithm>
#include <cmath>

MeshLodView makeMeshLodView(const glm::vec3& cameraPosition, float projectionYScale, uint32_t viewportHeightPixels,
                            float thresholdPixels) {
    if (!(thresholdPixels > 0.0f) || !std::isfinite(thresholdPixels) || viewportHeightPixels == 0) {
        return {.cameraPosition = cameraPosition, .errorScale = 0.0f};
    }
    // Pixels covered by one world unit at distance one.
    const float pixelsPerUnit = std::abs(projectionYScale) * 0.5f * static_cast<float>(viewportHeightPixels);
    return {.cameraPosition = cameraPosition, .errorScale = pixelsPerUnit / thresholdPixels};
}

float maximumAxisScale(const glm::mat4& world) {
    return std::max(
        {glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])), glm::length(glm::vec3(world[2]))});
}

uint32_t selectMeshLod(const MeshPrimitive& primitive, const MeshLodView& view, const glm::vec3& worldBoundsCenter,
                       float worldBoundsRadius, float worldScale) {
    if (view.errorScale <= 0.0f) {
        return 0;
    }
    const float distance = std::max(glm::length(worldBoundsCenter - view.cameraPosition) - worldBoundsRadius, 0.0f);
    uint32_t selected = 0;
    for (uint32_t level = 1; level < primitive.lodCount(); ++level) {
        if (primitive.lod(level).geometricError * worldScale * view.errorScale > distance) {
            break;
        }
        selected = level;
    }
    return selected;
}
