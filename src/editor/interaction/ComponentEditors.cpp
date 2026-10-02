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

namespace {
bool drawLocalLightShadows(bool& castsShadows, uint32_t& resolution, int32_t& priority) {
    bool changed = ImGui::Checkbox("Cast shadows", &castsShadows);
    ImGui::BeginDisabled(!castsShadows);
    int texels = static_cast<int>(resolution);
    if (ImGui::DragInt("Shadow resolution", &texels, 8.0f, 0, 4096, texels == 0 ? "Quality default" : "%d texels")) {
        resolution = static_cast<uint32_t>(std::max(texels, 0));
        changed = true;
    }
    changed |= ImGui::DragInt("Shadow priority", &priority, 0.1f);
    ImGui::EndDisabled();
    return changed;
}
} // namespace

bool drawPointLight(PointLightComponent& component) {
    bool changed = ImGui::ColorEdit3("Color", &component.color.x);
    changed |= ImGui::DragFloat("Intensity", &component.intensity, 0.05f, 0.0f, 10000.0f);
    changed |= ImGui::DragFloat("Range", &component.range, 0.05f, 0.01f, 10000.0f);
    changed |= drawLocalLightShadows(component.castsShadows, component.shadowResolution, component.shadowPriority);
    return changed;
}

bool drawSpotLight(SpotLightComponent& component) {
    bool changed = ImGui::DragFloat3("Direction", &component.direction.x, 0.01f, -1.0f, 1.0f);
    changed |= ImGui::ColorEdit3("Color", &component.color.x);
    changed |= ImGui::DragFloat("Intensity", &component.intensity, 0.05f, 0.0f, 10000.0f);
    changed |= ImGui::DragFloat("Range", &component.range, 0.05f, 0.01f, 10000.0f);
    // The outer cone never narrows below the inner one (SceneDocument rejects it).
    changed |= ImGui::SliderAngle("Inner cone", &component.innerConeRadians, 0.0f, 89.0f);
    changed |= ImGui::SliderAngle("Outer cone", &component.outerConeRadians, 0.0f, 89.0f);
    component.outerConeRadians = std::max(component.outerConeRadians, component.innerConeRadians);
    changed |= drawLocalLightShadows(component.castsShadows, component.shadowResolution, component.shadowPriority);
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

namespace {
// Edits a [minimum, maximum] pair, keeping the maximum at least the minimum.
bool drawRange(const char* label, float& minimum, float& maximum, float speed, float lowest, float highest) {
    float range[2]{minimum, maximum};
    if (!ImGui::DragFloat2(label, range, speed, lowest, highest)) {
        return false;
    }
    minimum = std::clamp(range[0], lowest, highest);
    maximum = std::clamp(std::max(range[1], minimum), lowest, highest);
    return true;
}
} // namespace

bool drawParticleEmitter(ParticleEmitterComponent& component) {
    int maxParticles = static_cast<int>(component.maxParticles);
    bool changed = false;
    if (ImGui::DragInt("Max particles", &maxParticles, 16.0f, 1, static_cast<int>(maximumParticlesPerEmitter))) {
        component.maxParticles =
            static_cast<uint32_t>(std::clamp(maxParticles, 1, static_cast<int>(maximumParticlesPerEmitter)));
        changed = true;
    }
    changed |= ImGui::Checkbox("Emitting", &component.emitting);
    changed |= ImGui::DragFloat("Spawn rate", &component.spawnRate, 0.5f, 0.0f, 100000.0f, "%.1f /s");
    int burstCount = static_cast<int>(component.burstCount);
    if (ImGui::DragInt("Burst count", &burstCount, 1.0f, 0, static_cast<int>(maximumParticlesPerEmitter))) {
        component.burstCount = static_cast<uint32_t>(std::max(burstCount, 0));
        changed = true;
    }
    changed |= ImGui::DragFloat("Burst interval", &component.burstInterval, 0.01f, 0.0f, 3600.0f, "%.2f s");
    changed |= drawRange("Lifetime", component.lifetimeMin, component.lifetimeMax, 0.01f, 0.001f, 3600.0f);
    changed |= ImGui::DragFloat3("Direction", &component.direction.x, 0.01f, -1.0f, 1.0f);
    if (glm::dot(component.direction, component.direction) < 1e-8f) {
        component.direction = {0.0f, 1.0f, 0.0f};
    }
    changed |= ImGui::SliderAngle("Cone angle", &component.coneAngleRadians, 0.0f, 180.0f);
    changed |= drawRange("Speed", component.speedMin, component.speedMax, 0.01f, 0.0f, 10000.0f);
    changed |= ImGui::DragFloat("Spawn radius", &component.spawnRadius, 0.01f, 0.0f, 10000.0f);
    changed |= ImGui::DragFloat3("Gravity", &component.gravity.x, 0.05f, -1000.0f, 1000.0f);
    changed |= ImGui::DragFloat("Drag", &component.drag, 0.01f, 0.0f, 100.0f);
    changed |= ImGui::DragFloat("Start size", &component.startSize, 0.005f, 0.0f, 1000.0f);
    changed |= ImGui::DragFloat("End size", &component.endSize, 0.005f, 0.0f, 1000.0f);
    changed |= ImGui::DragFloat("Angular speed", &component.angularSpeed, 0.01f, 0.0f, 100.0f, "%.2f rad/s");
    changed |= ImGui::ColorEdit4("Start color", &component.startColor.x, ImGuiColorEditFlags_Float);
    changed |= ImGui::ColorEdit4("End color", &component.endColor.x, ImGuiColorEditFlags_Float);
    changed |= ImGui::DragFloat("Emissive intensity", &component.emissiveIntensity, 0.05f, 0.0f, 10000.0f);
    changed |= ImGui::Checkbox("Lit", &component.lit);
    int blendMode = component.blendMode == ParticleBlendMode::Additive ? 1 : 0;
    if (ImGui::Combo("Blend mode", &blendMode, "Alpha\0Additive\0")) {
        component.blendMode = blendMode == 1 ? ParticleBlendMode::Additive : ParticleBlendMode::Alpha;
        changed = true;
    }
    int space = component.space == ParticleSimulationSpace::Local ? 1 : 0;
    if (ImGui::Combo("Simulation space", &space, "World\0Local\0")) {
        component.space = space == 1 ? ParticleSimulationSpace::Local : ParticleSimulationSpace::World;
        changed = true;
    }
    changed |= ImGui::DragFloat("Soft distance", &component.softDistance, 0.01f, 0.0f, 100.0f);
    ImGui::TextDisabled("Sprite: %s",
                        component.textureAsset.empty() ? "procedural round" : component.textureAsset.c_str());
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

bool clampIrradianceProbeCounts(const glm::ivec3& requested, glm::uvec3& counts) {
    const glm::uvec3 clamped(glm::clamp(requested, glm::ivec3(1), glm::ivec3(maximumIrradianceProbesPerAxis)));
    if (static_cast<uint64_t>(clamped.x) * clamped.y * clamped.z > maximumIrradianceProbes) {
        return false;
    }
    const bool changed = clamped != counts;
    counts = clamped;
    return changed;
}

bool drawIrradianceVolume(IrradianceVolumeComponent& component) {
    bool changed = ImGui::DragFloat3("Half extents", &component.halfExtents.x, 0.01f, 0.01f, 10000.0f);
    component.halfExtents = glm::max(component.halfExtents, glm::vec3(0.01f));
    glm::ivec3 counts(component.probeCounts);
    if (ImGui::DragInt3("Probe counts", &counts.x, 0.05f, 1, static_cast<int>(maximumIrradianceProbesPerAxis))) {
        changed |= clampIrradianceProbeCounts(counts, component.probeCounts);
    }
    changed |= ImGui::DragFloat("Intensity", &component.intensity, 0.01f, 0.0f, 100.0f);
    changed |= ImGui::DragFloat("Blend distance", &component.blendDistance, 0.01f, 0.0f, 100.0f);
    changed |= ImGui::DragFloat("Normal bias", &component.normalBias, 0.005f, 0.0f, 10.0f);
    changed |= ImGui::DragFloat("View bias", &component.viewBias, 0.005f, 0.0f, 10.0f);
    component.intensity = std::max(component.intensity, 0.0f);
    component.blendDistance = std::max(component.blendDistance, 0.0f);
    component.normalBias = std::max(component.normalBias, 0.0f);
    component.viewBias = std::max(component.viewBias, 0.0f);
    if (component.bakeAsset.empty()) {
        ImGui::TextDisabled("Bake: (not baked)");
    } else if (!component.bakedData) {
        ImGui::TextColored({1.0f, 0.35f, 0.35f, 1.0f}, "Bake missing: %s", component.bakeAsset.c_str());
    } else {
        ImGui::TextDisabled("Bake: %s", component.bakeAsset.c_str());
    }
    return changed;
}
} // namespace ComponentEditors
