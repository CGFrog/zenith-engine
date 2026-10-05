#include "MeshRendererField.hpp"

#include "editor/interaction/AssetReferenceField.hpp"
#include "editor/interaction/ReflectedFieldEditor.hpp"
#include "editor/panels/EditorContext.hpp"
#include "editor/project/ProjectFileOperations.hpp"
#include "engine/scene/Scene.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <system_error>

namespace {
// What the row shows for the active entity's mesh.
struct ShownMesh {
    // The model file, as a field value (project-relative); empty for none.
    std::string value;
    std::filesystem::path model;
    uint32_t index{0};
    bool runtime{false};
    bool loaded{false};
};

ShownMesh shownMesh(const AssetManager& assets, const AssetReferenceFieldContext& fieldContext, MeshHandle handle) {
    ShownMesh shown;
    const std::optional<MeshAssetSource> source = assets.meshSource(handle);
    if (!source || source->path.empty()) {
        shown.runtime = true;
        return shown;
    }
    shown.model = source->path;
    shown.index = source->meshIndex;
    shown.value = projectAssetValue(fieldContext.assetRoot, source->path).value_or(utf8PathText(source->path));
    shown.loaded = assets.mesh(handle) != nullptr;
    return shown;
}

// The entities a pick changes: the shown one and every other listed entity
// with a Mesh Renderer.
nlohmann::json pickTargets(const Scene& scene, std::span<const SceneEntityId> entities) {
    nlohmann::json targets = nlohmann::json::array();
    for (size_t index = 0; index < entities.size(); ++index) {
        const Entity entity = scene.findEntity(entities[index]);
        if (index == 0 || (entity != entt::null && scene.registry().all_of<MeshRendererComponent>(entity))) {
            targets.push_back({{"entity", entities[index].value}});
        }
    }
    return targets;
}

// Whether a listed entity's renderer draws another mesh than the shown one.
bool meshesDiffer(const Scene& scene, std::span<const SceneEntityId> entities, MeshHandle shown) {
    return std::ranges::any_of(entities.subspan(1), [&](SceneEntityId id) {
        const Entity entity = scene.findEntity(id);
        const auto* renderer = entity != entt::null ? scene.registry().try_get<MeshRendererComponent>(entity) : nullptr;
        return renderer != nullptr && renderer->mesh != shown;
    });
}

void assign(EditorContext& context, std::span<const SceneEntityId> entities, const std::string& asset, uint32_t mesh) {
    const nlohmann::json properties{
        {"entities", pickTargets(context.app.scene, entities)}, {"asset", asset}, {"mesh", mesh}};
    context.invoke("object.mesh_assign", properties.dump());
}

std::string choiceLabel(std::span<const MeshChoice> choices, uint32_t index) {
    const auto found = std::ranges::find(choices, index, &MeshChoice::index);
    return found != choices.end() ? found->label : "Mesh " + std::to_string(index);
}

// The mesh combo; returns the chosen index.
std::optional<uint32_t> drawMeshCombo(std::span<const MeshChoice> choices, uint32_t current, bool mixed) {
    std::optional<uint32_t> chosen;
    const std::string preview = mixed ? std::string(mixedValueText) : choiceLabel(choices, current);
    if (ImGui::BeginCombo("Mesh##index", preview.c_str())) {
        for (const MeshChoice& choice : choices) {
            ImGui::PushID(static_cast<int>(choice.index));
            const bool selected = !mixed && choice.index == current;
            if (ImGui::Selectable(choice.label.c_str(), selected) && (mixed || choice.index != current)) {
                chosen = choice.index;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return chosen;
}

// What the slots' materials come from, beside the materials: a Material
// Override that replaces them all.
std::optional<std::string> materialReplacement(const Scene& scene, const AssetManager& assets, Entity entity) {
    const auto* materialOverride = scene.registry().try_get<MaterialOverrideComponent>(entity);
    if (materialOverride == nullptr) {
        return std::nullopt;
    }
    if (!materialOverride->materialAsset.empty()) {
        return utf8PathText(pathFromUtf8Text(materialOverride->materialAsset).filename());
    }
    if (assets.material(materialOverride->material) != nullptr) {
        return std::string("a runtime material");
    }
    return std::nullopt;
}

void drawMaterialSlots(const Scene& scene, const AssetManager& assets, Entity entity, MeshHandle handle,
                       std::span<const std::string> names) {
    const MeshAsset* mesh = assets.mesh(handle);
    if (mesh == nullptr || mesh->primitives.empty()) {
        return;
    }
    ImGui::TextDisabled("Material slots");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The material each part of the mesh draws with. A Material Override tints every slot, and "
                          "its material replaces them all.");
    }
    ImGui::Indent();
    for (size_t slot = 0; slot < mesh->primitives.size(); ++slot) {
        std::string name;
        if (names.size() == mesh->primitives.size()) {
            name = names[slot];
        } else if (const std::optional<MaterialAssetSource> source =
                       assets.materialSource(mesh->primitives[slot].material)) {
            name = utf8PathText(source->path.filename());
        } else {
            name = "Material";
        }
        ImGui::Text("%zu  %s", slot, name.c_str());
    }
    if (const std::optional<std::string> replacement = materialReplacement(scene, assets, entity)) {
        ImGui::TextDisabled("Replaced by %s (Material Override)", replacement->c_str());
    }
    ImGui::Unindent();
}

void drawRuntimeMesh() {
    ImGui::BeginDisabled();
    std::string text = "(runtime mesh)";
    ImGui::InputText("Mesh##runtime", text.data(), text.size() + 1, ImGuiInputTextFlags_ReadOnly);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Built by code (AssetManager::createMesh): it has no file, so it cannot be chosen here "
                          "or saved with the scene.");
    }
}

// The first mesh of the model at path a renderer can draw.
std::optional<uint32_t> firstMesh(ModelOutlineCache& outlines, const AssetReferenceFieldContext& fieldContext,
                                  const std::string& value) {
    const std::vector<MeshChoice> choices =
        meshChoices(outlines.outline(normalizedCatalogPath(fieldContext.assetRoot / pathFromUtf8Text(value))));
    return choices.empty() ? std::nullopt : std::optional<uint32_t>(choices.front().index);
}
} // namespace

const ModelOutline& ModelOutlineCache::outline(const std::filesystem::path& model) {
    std::error_code error;
    std::optional<std::filesystem::file_time_type> writeTime = std::filesystem::last_write_time(model, error);
    if (error) {
        writeTime.reset();
    }
    const std::string key = utf8PathText(model);
    const auto found = m_entries.find(key);
    if (found != m_entries.end() && found->second.writeTime == writeTime) {
        return found->second.outline;
    }
    Entry& entry = m_entries[key];
    entry.writeTime = writeTime;
    entry.outline = readModelOutline(model);
    return entry.outline;
}

std::vector<MeshChoice> meshChoices(const ModelOutline& outline) {
    std::vector<MeshChoice> choices;
    for (size_t index = 0; index < outline.meshes.size(); ++index) {
        const ModelOutlineMesh& mesh = outline.meshes[index];
        if (mesh.primitives.empty()) {
            continue;
        }
        std::string label = mesh.name.empty() ? "Mesh " + std::to_string(index) : mesh.name;
        if (mesh.skinned) {
            label += " (skinned)";
        }
        if (mesh.morphTargetCount > 0) {
            label += " (morph)";
        }
        choices.push_back({.index = static_cast<uint32_t>(index), .label = std::move(label)});
    }
    return choices;
}

std::vector<std::string> materialSlotNames(const ModelOutline& outline, uint32_t mesh) {
    std::vector<std::string> names;
    if (mesh >= outline.meshes.size()) {
        return names;
    }
    for (const ModelOutlinePrimitive& primitive : outline.meshes[mesh].primitives) {
        if (primitive.material < 0 || static_cast<size_t>(primitive.material) >= outline.materials.size()) {
            names.emplace_back("Default material");
        } else if (const std::string& name = outline.materials[static_cast<size_t>(primitive.material)];
                   !name.empty()) {
            names.push_back(name);
        } else {
            names.push_back("Material " + std::to_string(primitive.material));
        }
    }
    return names;
}

void drawMeshRendererField(EditorContext& context, std::span<const SceneEntityId> entities,
                           ModelOutlineCache& outlines) {
    const Scene& scene = context.app.scene;
    const AssetManager& assets = context.app.assets;
    const Entity entity = entities.empty() ? Entity{entt::null} : scene.findEntity(entities.front());
    const std::optional<AssetReferenceFieldContext> fieldContext = assetReferenceFieldContext(context);
    if (entity == entt::null || !fieldContext) {
        return;
    }
    const auto* renderer = scene.registry().try_get<MeshRendererComponent>(entity);
    if (renderer == nullptr &&
        (entities.size() > 1 || scene.registry().any_of<ModelComponent, ModelNodeComponent>(entity))) {
        return;
    }
    ImGui::PushID("##MeshField");
    const ShownMesh shown = renderer != nullptr ? shownMesh(assets, *fieldContext, renderer->mesh) : ShownMesh{};
    if (shown.runtime) {
        drawRuntimeMesh();
        drawMaterialSlots(scene, assets, entity, renderer->mesh, {});
        ImGui::PopID();
        return;
    }
    // Copied: a pick below changes the scene, which may move the component.
    const std::optional<MeshHandle> shownHandle =
        renderer != nullptr ? std::optional<MeshHandle>(renderer->mesh) : std::nullopt;
    const bool mixed = shownHandle && meshesDiffer(scene, entities, *shownHandle);
    ImGui::BeginDisabled(!context.canInvoke("object.mesh_assign"));
    std::string value = shown.value;
    const std::optional<bool> loaded = shownHandle ? std::optional<bool>(shown.loaded) : std::nullopt;
    if (drawAssetReferenceField("Mesh model", value, "model", *fieldContext,
                                {.loaded = loaded, .mixed = mixed, .clearable = false}) &&
        !value.empty() && (mixed || value != shown.value)) {
        assign(context, entities, value, firstMesh(outlines, *fieldContext, value).value_or(0));
    }
    std::vector<std::string> slotNames;
    if (shownHandle) {
        const ModelOutline& outline = outlines.outline(shown.model);
        const std::vector<MeshChoice> choices = meshChoices(outline);
        if (const std::optional<uint32_t> chosen = drawMeshCombo(choices, shown.index, mixed)) {
            assign(context, entities, shown.value, *chosen);
        }
        slotNames = materialSlotNames(outline, shown.index);
    }
    ImGui::EndDisabled();
    if (shownHandle && !mixed) {
        if (const auto* current = scene.registry().try_get<MeshRendererComponent>(entity)) {
            drawMaterialSlots(scene, assets, entity, current->mesh,
                              current->mesh == *shownHandle ? std::span<const std::string>(slotNames)
                                                            : std::span<const std::string>{});
        }
    }
    ImGui::PopID();
}
