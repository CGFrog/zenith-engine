#pragma once

#include "engine/scene/Scene.hpp"

#include <glm/glm.hpp>

struct EditableTransform {
    glm::vec3 translation{0.0f};
    glm::vec3 rotationDegrees{0.0f};
    glm::vec3 scale{1.0f};
};

namespace ComponentEditors {
bool decomposeTransform(const glm::mat4& matrix, EditableTransform& transform);
glm::mat4 composeTransform(const EditableTransform& transform);

bool drawTransform(TransformComponent& component);
bool drawCamera(CameraComponent& component);
bool drawDirectionalLight(DirectionalLightComponent& component);
bool drawEnvironmentLight(EnvironmentLightComponent& component);
bool drawPointLight(PointLightComponent& component);
bool drawSpotLight(SpotLightComponent& component);
bool drawCollider(ColliderComponent& component);
bool drawRigidBody(RigidBodyComponent& component);
bool drawAnimation(AnimationComponent& component, const ModelAsset* model);
bool drawMorphWeights(MorphWeightsComponent& component);
} // namespace ComponentEditors
