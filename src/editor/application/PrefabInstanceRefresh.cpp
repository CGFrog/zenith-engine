#include "PrefabInstanceRefresh.hpp"

#include "editor/application/SceneEditing.hpp"
#include "editor/document/EditorDocument.hpp"
#include "editor/document/PrefabAuthoring.hpp"

#include <utility>

PrefabResult refreshPrefabInstances(const SceneEditing& editing, const std::filesystem::path& reference,
                                    const PrefabDocument* previous, std::string stepName, size_t* refreshed) {
    if (refreshed != nullptr) {
        *refreshed = 0;
    }
    Scene& scene = editing.scene();
    const size_t instances = prefabInstancesOf(scene, reference).size();
    if (instances == 0) {
        return {.success = true};
    }
    std::string before;
    if (!editing.capture(before)) {
        return {.error = "Could not record the refresh of " + reference.generic_string() + " for undo"};
    }
    PrefabAuthoring authoring(editing.assets(), editing.document().options());
    PrefabResult result = authoring.refreshInstances(scene, reference, previous);
    if (!result) {
        return result;
    }
    editing.commit(std::move(stepName), {}, std::move(before));
    if (refreshed != nullptr) {
        *refreshed = instances;
    }
    return result;
}

std::string externalPrefabRefreshStepName(const std::string& displayName) {
    return "Refresh " + displayName + " instances (changed on disk)";
}
