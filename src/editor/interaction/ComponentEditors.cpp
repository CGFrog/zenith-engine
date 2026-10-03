#include "ComponentEditors.hpp"

#include "editor/theme/EditorTheme.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>
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
        ImGui::TextColored(toImVec4(palette::error), "Transform cannot be decomposed.");
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
        ImGui::TextColored(toImVec4(palette::error), "Bake missing: %s", component.bakeAsset.c_str());
    } else {
        ImGui::TextDisabled("Bake: %s", component.bakeAsset.c_str());
    }
    return changed;
}

} // namespace ComponentEditors
