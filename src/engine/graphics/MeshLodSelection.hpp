#pragma once

#include "engine/assets/Assets.hpp"

#include <cstdint>
#include <glm/glm.hpp>

// Per-view terms for choosing a primitive's level of detail by projected
// screen-space error. selectLodBatch in shaders/gpu_visibility.comp mirrors
// selectMeshLod; keep their math identical so both submission paths agree.
struct MeshLodView {
    glm::vec3 cameraPosition{0.0f};
    // A level is acceptable when worldError * errorScale <= distance, which is
    // projected error <= threshold pixels. Zero selects full detail only.
    float errorScale{0.0f};
};

// projectionYScale is |projection[1][1]|, that is 1 / tan(verticalFov / 2).
// A non-positive threshold disables simplified levels.
MeshLodView makeMeshLodView(const glm::vec3& cameraPosition, float projectionYScale, uint32_t viewportHeightPixels,
                            float thresholdPixels);

// Largest axis scale of a transform; scales object-space errors and radii.
float maximumAxisScale(const glm::mat4& world);

// Returns the coarsest level whose projected error stays within the view's
// threshold. Distance is measured to the nearest point of the world-space
// bounding sphere, so a camera inside the bounds keeps zero-error levels only.
uint32_t selectMeshLod(const MeshPrimitive& primitive, const MeshLodView& view, const glm::vec3& worldBoundsCenter,
                       float worldBoundsRadius, float worldScale);
