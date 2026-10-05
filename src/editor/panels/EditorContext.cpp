#include "EditorContext.hpp"

#include "editor/application/EditorDiagnostics.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/interaction/EntityEyedropper.hpp"
#include "editor/viewport/SnapQueries.hpp"

#include <nlohmann/json.hpp>

namespace {
// The properties object.place_model and prefab.instantiate share: the
// asset, the parent's stable id, and a placement under the mouse (without
// one the operator starts at the 3D cursor).
nlohmann::json placementProperties(const std::filesystem::path& path, Entity parent, const Scene& scene,
                                   const Placement* placement) {
    nlohmann::json properties{{"asset", path.generic_string()}};
    if (parent != entt::null) {
        properties["parent"] = std::to_string(scene.id(parent).value);
    }
    if (placement != nullptr) {
        properties["location"] = {placement->position.x, placement->position.y, placement->position.z};
        if (placement->normal) {
            properties["normal"] = {placement->normal->x, placement->normal->y, placement->normal->z};
        }
    }
    return properties;
}
} // namespace

void EditorContext::placeModel(const std::filesystem::path& path, Entity parent, const Placement* placement) {
    invoke("object.place_model", placementProperties(path, parent, app.scene, placement).dump());
}

void EditorContext::placePrefab(const std::filesystem::path& path, Entity parent, const Placement* placement) {
    invoke("prefab.instantiate", placementProperties(path, parent, app.scene, placement).dump());
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
