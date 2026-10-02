#pragma once

#include "engine/assets/IrradianceVolumeAsset.hpp"
#include "engine/scene/Scene.hpp"

#include <glm/glm.hpp>

struct EditableTransform {
    glm::vec3 translation{0.0f};
    glm::vec3 rotationDegrees{0.0f};
    glm::vec3 scale{1.0f};
};

// Hand-written inspectors for components whose data the generic reflected
// field editor cannot present well: the transform matrix, the collider shape
// variant, clips that depend on the model, per-target morph weights, and the
// irradiance probe budget. Every other component is drawn from its reflected
// fields (see ReflectedFieldEditor).
namespace ComponentEditors {
bool decomposeTransform(const glm::mat4& matrix, EditableTransform& transform);
glm::mat4 composeTransform(const EditableTransform& transform);

bool drawTransform(TransformComponent& component);
bool drawCollider(ColliderComponent& component);
bool drawAnimation(AnimationComponent& component, const ModelAsset* model);
bool drawMorphWeights(MorphWeightsComponent& component);
// Applies requested per-axis probe counts clamped to 1..maximumIrradianceProbesPerAxis;
// rejects (returns false, leaving counts) a grid above maximumIrradianceProbes.
// Returns whether counts changed.
bool clampIrradianceProbeCounts(const glm::ivec3& requested, glm::uvec3& counts);
bool drawIrradianceVolume(IrradianceVolumeComponent& component);
} // namespace ComponentEditors
