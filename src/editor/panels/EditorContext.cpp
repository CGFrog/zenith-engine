#include "EditorContext.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/application/EntityCommands.hpp"
#include "editor/interaction/EntityEyedropper.hpp"

#include <nlohmann/json.hpp>

void EditorContext::placeModel(const std::filesystem::path& path, Entity parent) {
    if (!editMode()) {
        diagnostics.reportError("Assets can only be placed in Edit mode");
        return;
    }
    EntityCommands::placeModel(editing, selection, path, parent);
}

void EditorContext::placePrefab(const std::filesystem::path& path, Entity parent) {
    nlohmann::json properties{{"asset", path.generic_string()}};
    if (parent != entt::null) {
        properties["parent"] = std::to_string(app.scene.id(parent).value);
    }
    invoke("prefab.instantiate", properties.dump());
}

bool EditorContext::eyedropperArmed() const {
    return eyedropper != nullptr && eyedropper->armed();
}

bool EditorContext::eyedropperBlocksContextMenus() const {
    return eyedropper != nullptr && (eyedropper->armed() || eyedropper->cancellingClickHeld());
}

void EditorContext::pickEyedropperEntity(SceneEntityId entity) {
    invoke("ui.eyedropper_pick", nlohmann::json{{"entity", entity.value}}.dump());
}
