#include "ComponentEditors.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <type_traits>

namespace ComponentEditors {
bool decomposeTransform(const glm::mat4& matrix, EditableTransform& transform) {
    glm::vec3 skew;
    glm::vec4 perspective;
    glm::quat rotation;
    if (!glm::decompose(matrix, transform.scale, rotation, transform.translation, skew, perspective)) {
        return false;
    }
    transform.rotationDegrees = glm::degrees(glm::eulerAngles(glm::normalize(rotation)));
    const auto finite = [](const glm::vec3& value) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    };
    return finite(transform.translation) && finite(transform.rotationDegrees) && finite(transform.scale);
}

glm::mat4 composeTransform(const EditableTransform& transform) {
    return glm::translate(glm::mat4(1.0f), transform.translation) *
           glm::mat4_cast(glm::quat(glm::radians(transform.rotationDegrees))) *
           glm::scale(glm::mat4(1.0f), transform.scale);
}

bool drawTransform(TransformComponent& component) {
    EditableTransform transform;
    if (!decomposeTransform(component.local, transform)) {
        ImGui::TextColored({1.0f, 0.35f, 0.35f, 1.0f}, "Transform cannot be decomposed.");
        return false;
    }
    bool changed = ImGui::DragFloat3("Position", &transform.translation.x, 0.05f);
    changed |= ImGui::DragFloat3("Rotation", &transform.rotationDegrees.x, 0.25f);
    changed |= ImGui::DragFloat3("Scale", &transform.scale.x, 0.02f, 0.001f, 1000.0f);
    if (changed) {
        component.local = composeTransform(transform);
    }
    return changed;
}

bool drawCamera(CameraComponent& component) {
    float fovDegrees = glm::degrees(component.verticalFovRadians);
    bool changed = ImGui::DragFloat("Vertical FOV", &fovDegrees, 0.25f, 1.0f, 179.0f, "%.1f deg");
    changed |= ImGui::DragFloat("Near plane", &component.nearPlane, 0.01f, 0.001f, component.farPlane);
    changed |= ImGui::DragFloat("Far plane", &component.farPlane, 1.0f, component.nearPlane, 100000.0f);
    changed |= ImGui::Checkbox("Primary", &component.primary);
    component.verticalFovRadians = glm::radians(fovDegrees);
    return changed;
}

bool drawDirectionalLight(DirectionalLightComponent& component) {
    bool changed = ImGui::DragFloat3("Direction", &component.direction.x, 0.01f, -1.0f, 1.0f);
    changed |= ImGui::ColorEdit3("Color", &component.color.x);
    changed |= ImGui::DragFloat("Intensity", &component.intensity, 0.05f, 0.0f, 10000.0f);
    changed |= ImGui::Checkbox("Primary", &component.primary);
    changed |= ImGui::Checkbox("Cast shadows", &component.shadows.enabled);
    if (ImGui::TreeNode("Shadow tuning")) {
        changed |= ImGui::DragFloat("Constant bias", &component.shadows.constantBias, 0.01f, 0.0f, 10.0f);
        changed |= ImGui::DragFloat("Slope bias", &component.shadows.slopeBias, 0.01f, 0.0f, 10.0f);
        changed |= ImGui::DragFloat("Normal bias", &component.shadows.normalBias, 0.0005f, 0.0f, 1.0f);
        changed |= ImGui::DragFloat("Focus distance", &component.shadows.focusDistance, 0.1f, 0.0f, 10000.0f);
        changed |= ImGui::DragFloat("Depth range", &component.shadows.depthRange, 0.5f, 1.0f, 10000.0f);
        ImGui::TreePop();
    }
    return changed;
}

bool drawEnvironmentLight(EnvironmentLightComponent& component) {
    bool changed = ImGui::ColorEdit3("Sky color", &component.skyColor.x);
    changed |= ImGui::ColorEdit3("Ground color", &component.groundColor.x);
    changed |= ImGui::DragFloat("Diffuse intensity", &component.diffuseIntensity, 0.01f, 0.0f, 100.0f);
    changed |= ImGui::DragFloat("Specular intensity", &component.specularIntensity, 0.01f, 0.0f, 100.0f);
    changed |= ImGui::Checkbox("Primary", &component.primary);
    return changed;
}

bool drawVolumetricFog(VolumetricFogComponent& component) {
    bool changed = ImGui::DragFloat("Density", &component.density, 0.001f, 0.0f, 10.0f, "%.4f");
    changed |= ImGui::DragFloat("Height falloff", &component.heightFalloff, 0.005f, 0.0f, 10.0f, "%.3f");
    changed |= ImGui::DragFloat("Base height", &component.baseHeight, 0.05f, -10000.0f, 10000.0f);
    changed |= ImGui::ColorEdit3("Albedo", &component.albedo.x);
    changed |= ImGui::SliderFloat("Anisotropy", &component.anisotropy, -0.95f, 0.95f, "%.2f");
    changed |= ImGui::SliderFloat("Noise amount", &component.noiseAmount, 0.0f, 1.0f, "%.2f");
    changed |= ImGui::DragFloat("Noise scale", &component.noiseScale, 0.05f, 0.05f, 1000.0f);
    changed |= ImGui::Checkbox("Primary", &component.primary);
    return changed;
}

bool drawCollider(ColliderComponent& component) {
    bool changed = false;
    std::visit(
        [&](auto& shape) {
            using Shape = std::decay_t<decltype(shape)>;
            if constexpr (std::is_same_v<Shape, BoxCollider>) {
                changed = ImGui::DragFloat3("Half extents", &shape.halfExtents.x, 0.01f, 0.001f, 10000.0f);
            } else if constexpr (std::is_same_v<Shape, SphereCollider>) {
                changed = ImGui::DragFloat("Radius", &shape.radius, 0.01f, 0.001f, 10000.0f);
            } else if constexpr (std::is_same_v<Shape, CapsuleCollider>) {
                changed = ImGui::DragFloat("Half height", &shape.halfHeight, 0.01f, 0.001f, 10000.0f);
                changed |= ImGui::DragFloat("Radius", &shape.radius, 0.01f, 0.001f, 10000.0f);
            } else {
                ImGui::TextDisabled("Triangle mesh collider");
            }
        },
        component.shape);
    return changed;
}

bool drawRigidBody(RigidBodyComponent& component) {
    constexpr const char* motionTypes[] = {"Static", "Kinematic", "Dynamic"};
    int motionType = static_cast<int>(component.motionType);
    bool changed = ImGui::Combo("Motion", &motionType, motionTypes, std::size(motionTypes));
    component.motionType = static_cast<PhysicsMotionType>(motionType);
    changed |= ImGui::DragFloat("Mass", &component.mass, 0.05f, 0.001f, 100000.0f);
    changed |= ImGui::SliderFloat("Friction", &component.friction, 0.0f, 1.0f);
    changed |= ImGui::SliderFloat("Restitution", &component.restitution, 0.0f, 1.0f);
    changed |= ImGui::Checkbox("Sensor", &component.sensor);
    changed |= ImGui::Checkbox("Allow sleeping", &component.allowSleeping);
    changed |= ImGui::Checkbox("Continuous collision", &component.continuousCollision);
    changed |= ImGui::Checkbox("Lock rotation", &component.lockRotation);
    return changed;
}

bool drawAnimation(AnimationComponent& component, const ModelAsset* model) {
    bool changed = false;
    if (model == nullptr || model->animations.empty()) {
        ImGui::TextDisabled("Model has no animation clips.");
        return false;
    }
    component.clipIndex = std::min(component.clipIndex, static_cast<uint32_t>(model->animations.size() - 1));
    const char* preview = model->animations[component.clipIndex].name.c_str();
    if (ImGui::BeginCombo("Clip", preview)) {
        for (uint32_t index = 0; index < model->animations.size(); ++index) {
            const bool selected = index == component.clipIndex;
            if (ImGui::Selectable(model->animations[index].name.c_str(), selected)) {
                component.clipIndex = index;
                component.time = 0.0f;
                component.blendClipIndex.reset();
                changed = true;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    changed |= ImGui::Checkbox("Preview", &component.playing);
    changed |= ImGui::Checkbox("Loop", &component.looping);
    changed |= ImGui::DragFloat("Playback speed", &component.playbackSpeed, 0.05f, -8.0f, 8.0f);
    const float duration = model->animations[component.clipIndex].duration;
    changed |= ImGui::SliderFloat("Time", &component.time, 0.0f, std::max(duration, 0.001f), "%.3f s");
    return changed;
}

bool drawMorphWeights(MorphWeightsComponent& component) {
    bool changed = false;
    for (size_t index = 0; index < component.weights.size(); ++index) {
        const std::string label = "Target " + std::to_string(index);
        changed |= ImGui::SliderFloat(label.c_str(), &component.weights[index], -1.0f, 1.0f);
    }
    if (component.weights.empty()) {
        ImGui::TextDisabled("Model node has no morph targets.");
    }
    return changed;
}
} // namespace ComponentEditors
