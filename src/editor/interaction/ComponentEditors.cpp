#include "ComponentEditors.hpp"

#include "editor/theme/EditorTheme.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

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

std::span<const FieldDescriptor> editableTransformFields() {
    // The speeds and scale range drawTransform uses.
    static const std::vector<FieldDescriptor> fields = [] {
        Fields<EditableTransform> transform;
        transform.add("position", &EditableTransform::translation).label("Position").drag(0.05f);
        transform.add("rotation", &EditableTransform::rotationDegrees).label("Rotation").drag(0.25f);
        transform.add("scale", &EditableTransform::scale).label("Scale").drag(0.02f, 0.001, 1000.0);
        return std::move(transform).build();
    }();
    return fields;
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

namespace {
constexpr std::array<const char*, 4> colliderShapeLabels{"Box", "Sphere", "Capsule", "Triangle Mesh"};
constexpr float minimumColliderSize = 0.001f;

bool drawColliderMesh(TriangleMeshCollider& shape, std::span<const ColliderMeshChoice> meshes) {
    const auto current = std::ranges::find(meshes, shape.mesh, &ColliderMeshChoice::mesh);
    const char* preview = current != meshes.end() ? current->name.c_str() : "(another mesh)";
    bool changed = false;
    if (ImGui::BeginCombo("Mesh", preview)) {
        for (size_t index = 0; index < meshes.size(); ++index) {
            ImGui::PushID(static_cast<int>(index));
            const bool selected = meshes[index].mesh == shape.mesh;
            if (ImGui::Selectable(meshes[index].name.c_str(), selected)) {
                changed = !selected;
                shape.mesh = meshes[index].mesh;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("One of the entity's model meshes; triangle meshes collide only on static bodies");
    return changed;
}

bool drawColliderOffset(ColliderComponent& component) {
    bool changed = ImGui::DragFloat3("Offset", &component.offsetPosition.x, 0.01f);
    ImGui::SetItemTooltip("Where the shape sits, in the entity's local axes before its scale");
    glm::vec3 degrees = glm::degrees(glm::eulerAngles(glm::normalize(component.offsetRotation)));
    if (ImGui::DragFloat3("Offset rotation", &degrees.x, 0.25f)) {
        component.offsetRotation = glm::normalize(glm::quat(glm::radians(degrees)));
        changed = true;
    }
    return changed;
}
} // namespace

std::vector<ColliderMeshChoice> colliderMeshChoices(const Scene& scene, const AssetManager& assets, Entity entity) {
    std::vector<ColliderMeshChoice> choices;
    const auto add = [&](MeshHandle mesh, const std::string& name) {
        if (mesh && assets.mesh(mesh) != nullptr &&
            std::ranges::find(choices, mesh, &ColliderMeshChoice::mesh) == choices.end()) {
            choices.push_back(
                {.mesh = mesh, .name = name.empty() ? "Mesh " + std::to_string(choices.size() + 1) : name});
        }
    };
    const entt::registry& registry = scene.registry();
    if (!registry.valid(entity)) {
        return choices;
    }
    ModelHandle model;
    std::optional<uint32_t> ownNode;
    if (const auto* instance = registry.try_get<ModelComponent>(entity)) {
        model = instance->model;
    } else if (const auto* node = registry.try_get<ModelNodeComponent>(entity)) {
        model = node->model;
        ownNode = node->nodeIndex;
    }
    const ModelAsset* asset = model ? assets.model(model) : nullptr;
    if (const auto* renderer = registry.try_get<MeshRendererComponent>(entity)) {
        const std::string name = asset != nullptr && ownNode && *ownNode < asset->nodes.size()
                                     ? asset->nodes[*ownNode].name
                                     : std::string("Mesh Renderer");
        add(renderer->mesh, name);
    }
    if (asset != nullptr) {
        for (const ModelNode& node : asset->nodes) {
            add(node.mesh, node.name);
        }
    }
    return choices;
}

ColliderShape convertColliderShape(const ColliderShape& shape, ColliderShapeKind kind, MeshHandle mesh) {
    // The source's half size along each axis, which the new shape keeps.
    glm::vec3 halfExtents{0.5f};
    if (const auto* box = std::get_if<BoxCollider>(&shape)) {
        halfExtents = box->halfExtents;
    } else if (const auto* sphere = std::get_if<SphereCollider>(&shape)) {
        halfExtents = glm::vec3(sphere->radius);
    } else if (const auto* capsule = std::get_if<CapsuleCollider>(&shape)) {
        halfExtents = {capsule->radius, capsule->halfHeight + capsule->radius, capsule->radius};
    }
    switch (kind) {
    case ColliderShapeKind::Box:
        return BoxCollider{.halfExtents = halfExtents};
    case ColliderShapeKind::Sphere:
        return SphereCollider{.radius = std::max({halfExtents.x, halfExtents.y, halfExtents.z})};
    case ColliderShapeKind::Capsule: {
        const float radius = std::max(halfExtents.x, halfExtents.z);
        return CapsuleCollider{.halfHeight = std::max(halfExtents.y - radius, minimumColliderSize), .radius = radius};
    }
    case ColliderShapeKind::TriangleMesh:
        return TriangleMeshCollider{.mesh = mesh};
    }
    return shape;
}

bool drawCollider(ColliderComponent& component, std::span<const ColliderMeshChoice> meshes) {
    bool changed = false;
    const size_t current = component.shape.index();
    if (ImGui::BeginCombo("Shape", colliderShapeLabels[current])) {
        for (size_t index = 0; index < colliderShapeLabels.size(); ++index) {
            const auto kind = static_cast<ColliderShapeKind>(index);
            // A triangle mesh needs one of the entity's meshes.
            const bool available = kind != ColliderShapeKind::TriangleMesh || !meshes.empty();
            if (ImGui::Selectable(colliderShapeLabels[index], index == current,
                                  available ? ImGuiSelectableFlags_None : ImGuiSelectableFlags_Disabled) &&
                available && index != current) {
                component.shape =
                    convertColliderShape(component.shape, kind, meshes.empty() ? MeshHandle{} : meshes.front().mesh);
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    std::visit(
        [&](auto& shape) {
            using Shape = std::decay_t<decltype(shape)>;
            if constexpr (std::is_same_v<Shape, BoxCollider>) {
                changed |=
                    ImGui::DragFloat3("Half extents", &shape.halfExtents.x, 0.01f, minimumColliderSize, 10000.0f);
            } else if constexpr (std::is_same_v<Shape, SphereCollider>) {
                changed |= ImGui::DragFloat("Radius", &shape.radius, 0.01f, minimumColliderSize, 10000.0f);
            } else if constexpr (std::is_same_v<Shape, CapsuleCollider>) {
                changed |= ImGui::DragFloat("Half height", &shape.halfHeight, 0.01f, minimumColliderSize, 10000.0f);
                changed |= ImGui::DragFloat("Radius", &shape.radius, 0.01f, minimumColliderSize, 10000.0f);
            } else {
                changed |= drawColliderMesh(shape, meshes);
            }
        },
        component.shape);
    changed |= drawColliderOffset(component);
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

bool drawTags(TagsComponent& component, std::span<const std::string> projectTags, std::string& otherTag) {
    bool changed = false;
    std::optional<size_t> removed;
    for (size_t index = 0; index < component.tags.size(); ++index) {
        const std::string& tag = component.tags[index];
        ImGui::PushID(static_cast<int>(index));
        if (ImGui::SmallButton("x")) {
            removed = index;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(tag.c_str());
        if (std::ranges::find(projectTags, tag) == projectTags.end()) {
            ImGui::SameLine();
            ImGui::TextColored(toImVec4(palette::warning), "(not a project tag)");
            ImGui::SetItemTooltip("Declare it in Edit > Project Settings > Tags, or check its spelling.");
        }
        ImGui::PopID();
    }
    if (removed) {
        component.tags.erase(component.tags.begin() + static_cast<std::ptrdiff_t>(*removed));
        changed = true;
    }
    if (component.tags.empty()) {
        ImGui::TextDisabled("No tags");
    }
    if (ImGui::Button("Add Tag")) {
        ImGui::OpenPopup("##AddTag");
    }
    if (ImGui::BeginPopup("##AddTag")) {
        bool any = false;
        for (const std::string& tag : projectTags) {
            if (component.has(tag)) {
                continue;
            }
            any = true;
            if (ImGui::MenuItem(tag.c_str())) {
                changed = component.add(tag) || changed;
            }
        }
        if (!any) {
            ImGui::TextDisabled(projectTags.empty() ? "The project declares no tags" : "Every project tag is added");
        }
        ImGui::Separator();
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::InputTextWithHint("##OtherTag", "Other tag", &otherTag, ImGuiInputTextFlags_EnterReturnsTrue) &&
            !otherTag.empty()) {
            changed = component.add(std::exchange(otherTag, {})) || changed;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return changed;
}
} // namespace ComponentEditors
